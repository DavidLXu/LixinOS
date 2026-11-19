"""Package the source and SDK header dependencies for the native ARM64 build."""
import json
from pathlib import Path
import shlex
import subprocess
import sys

build = Path(sys.argv[1]).resolve()
upstream = build / "tinycc"
cc = "aarch64-none-elf-gcc"
# Ask the configured compiler for its actual newlib headers. Its sysroot
# can be empty even when it finds newlib through built-in search paths.
dependency_rule = subprocess.check_output(
    [cc, "-M", "-IUser/tcc", "-I" + str(build), "-I" + str(upstream), "User/tcc/port.c"],
    text=True,
)
deps = [Path(name).resolve() for name in shlex.split(dependency_rule.replace("\\\n", " "))[1:]]
sdk = next(path.parent for path in deps if path.name == "newlib.h")
files = {"upstream/COPYING": upstream / "COPYING",
         "builtin/tccdefs.h": upstream / "include/tccdefs.h"}
for name in ("port.c", "kernel-elf.h", "config.h", "slots.def"):
    files[name] = Path("User/tcc", name).resolve()
for path in deps:
    if path.is_relative_to(upstream):
        files["upstream/" + path.name] = path
    elif path.is_relative_to(sdk):
        files["sdk/" + str(path.relative_to(sdk))] = path
for name in ("stdarg.h", "stddef.h", "float.h"):
    files["sdk/" + name] = upstream / "include" / name
dirs = set()
with (build / "tcc-sources.rs").open("w") as manifest:
    for name, path in sorted(files.items()):
        guest = Path("/src/tinycc") / name
        for parent in reversed(guest.parents):
            if parent.is_relative_to("/src/tinycc") and parent != Path("/src/tinycc") and parent not in dirs:
                print(f"({json.dumps(str(parent))}, None),", file=manifest)
                dirs.add(parent)
        print(f"({json.dumps(str(guest))}, Some(include_bytes!({json.dumps(str(path))}))),", file=manifest)
