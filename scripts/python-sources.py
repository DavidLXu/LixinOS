"""Stage the locked MicroPython port's C translation units and configured headers."""
import json
from pathlib import Path
import shlex
import sys

build = Path(sys.argv[1]).resolve()
upstream = build / "micropython"
objects = build / "python-objects"
port = Path("User/python").resolve()
package = Path("/src/micropython")
files = {package / "LICENSE": upstream / "LICENSE"}
sources = set()
for rule in sorted(objects.rglob("*.P")):
    line = rule.read_text().replace("\\\n", " ").splitlines()[0]
    inputs = [(port / name).resolve() for name in shlex.split(line)[1:]]
    for path in inputs:
        for base in (upstream, objects, port):
            if path.is_relative_to(base):
                guest = package / path.relative_to(base)
                files[guest] = path
                if path == inputs[0]:
                    sources.add(guest)
                break
        else:
            if path.name in ("alloca.h", "assert.h") and "/aarch64-none-elf/include/" in str(path):
                files[package / path.name] = path
files[package / "stdbool.h"] = build / "tinycc/include/stdbool.h"
sources.discard(package / "shared/libc/printf.c")  # supplied by the LXE runtime
source_list = build / "micropython-sources.txt"
source_list.write_text("".join(f"{path}\n" for path in sorted(sources)))
files[package / "sources.txt"] = source_list
with (build / "micropython-sources.rs").open("w") as out:
    dirs = set()
    for path, source in sorted(files.items()):
        for parent in reversed(path.parents):
            if parent.is_relative_to(package) and parent not in dirs:
                print(f"({json.dumps(str(parent))}, None),", file=out)
                dirs.add(parent)
        print(f"({json.dumps(str(path))}, Some(include_bytes!({json.dumps(str(source))}))),", file=out)
