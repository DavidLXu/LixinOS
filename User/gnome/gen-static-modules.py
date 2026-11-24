"""Generate GModule's static symbol tables from the GIRs and target archives."""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

CORE = "{http://www.gtk.org/introspection/core/1.0}"
C = "{http://www.gtk.org/introspection/c/1.0}"
GLIB = "{http://www.gtk.org/introspection/glib/1.0}"
IDENTIFIER = re.compile(r"[a-zA-Z_][a-zA-Z0-9_]*\Z")


def read_exports(nm, archives):
    output = subprocess.check_output([nm, "-g", "--defined-only", *archives], text=True)
    exports = set()
    for line in output.splitlines():
        words = line.split()
        if len(words) == 3 and words[1] in {"T", "W", "i"}:
            exports.add(words[2])
    return exports


def read_modules(girs, exports):
    modules = defaultdict(set)
    for gir in girs:
        namespace = ET.parse(gir).getroot().find(CORE + "namespace")
        libraries = namespace.get("shared-library", "").split(",")
        symbols = {
            symbol
            for item in namespace.iter()
            for attribute in (C + "identifier", GLIB + "get-type")
            if (symbol := item.get(attribute)) in exports and IDENTIFIER.fullmatch(symbol)
        }
        for library in filter(None, libraries):
            modules[library].update(symbols)
    return modules


def generate(nm, archives, girs, output):
    modules = read_modules(girs, read_exports(nm, archives))
    symbols = sorted(set().union(*modules.values()))
    with output.open("w") as stream:
        print('#include "static-modules.h"\n', file=stream)
        for symbol in symbols:
            print(f'extern void address_{symbol}(void) __asm__("{symbol}");', file=stream)
        for number, (_, exported) in enumerate(sorted(modules.items())):
            print(f"\nstatic const struct lx_module_symbol symbols_{number}[] = {{", file=stream)
            for symbol in sorted(exported):
                print(f'    {{ "{symbol}", (void *)address_{symbol} }},', file=stream)
            print("    { 0, 0 }\n};", file=stream)
        print("\nstatic const struct lx_module modules[] = {", file=stream)
        for number, library in enumerate(sorted(modules)):
            print(f'    {{ "{library}", symbols_{number} }},', file=stream)
        print("    { 0, 0 }\n};\n\nconst struct lx_module *lx_static_modules(void)", file=stream)
        print("{\n    return modules;\n}", file=stream)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--archive", action="append", required=True)
    parser.add_argument("--gir", action="append", required=True)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    generate(options.nm, options.archive, options.gir, options.output)
