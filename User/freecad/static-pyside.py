"""Give static Qt Python modules the private symbols they normally get from a DSO."""
from pathlib import Path
import subprocess
import sys

build, toolbin = map(Path, sys.argv[1:])
output = build / "static-bindings"
output.mkdir(exist_ok=True)
nm = toolbin / "aarch64-none-elf-nm"
objcopy = toolbin / "aarch64-none-elf-objcopy"
seen = set()
for module in ("QtCore", "QtGui", "QtWidgets", "QtPrintSupport", "QtUiTools", "libshiboken6"):
    if module == "libshiboken6":
        archive = build / "pyside-sdk/lib/libshiboken6.abi3.a"
        # Shiboken's limited-API shims also exist in the embedded libpython.
        private = {"PyMethod_Function", "PyMethod_New", "PyMethod_Self",
                   "PyRun_String", "PyStaticMethod_New"}
    else:
        archive = build / f"pyside-build/PySide6/{module}/{module}.abi3.a"
        listing = subprocess.check_output([nm, "-g", "--defined-only", archive], text=True)
        symbols = {fields[2] for line in listing.splitlines() if
                   len(fields := line.split()) == 3 and fields[1] in ("B", "D", "R", "T")}
        private = symbols & seen
        seen.update(symbols)
    names = output / (module + ".syms")
    data = "".join(f"{name} lx_{module}_{name}\n" for name in sorted(private))
    dest = output / archive.name
    if dest.exists() and dest.stat().st_mtime >= archive.stat().st_mtime and names.exists() and names.read_text() == data:
        continue
    names.write_text(data)
    temp = dest.with_suffix(".tmp")
    subprocess.run([objcopy, "--redefine-syms", names, archive, temp], check=True)
    temp.replace(dest)
