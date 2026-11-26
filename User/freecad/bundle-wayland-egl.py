"""Bundle Mesa's internal archives in the order recorded for the EGL test client."""
from pathlib import Path
import os
import shlex
import subprocess
import sys

build, destination = map(Path, sys.argv[1:])
command = subprocess.check_output(
    ["ninja", "-t", "commands", "src/egl/lixin-egl-check"], cwd=build, text=True
).splitlines()[-1]
archives = list(dict.fromkeys(
    arg for arg in shlex.split(command)
    if arg.endswith(".a") and not Path(arg).is_absolute() and Path(arg).name != "libGLESv2.a"
))
assert "src/egl/libEGL.a" in archives
subprocess.run(["ninja", "-j", os.environ.get("JOBS", "8"), *archives], cwd=build, check=True)
destination.parent.mkdir(parents=True, exist_ok=True)
temporary = destination.with_suffix(".tmp")
temporary.unlink(missing_ok=True)
script = [f"create {temporary}", *(f"addlib {archive}" for archive in archives), "save", "end"]
subprocess.run(["aarch64-none-elf-ar", "-M"], cwd=build, input="\n".join(script)+"\n", text=True, check=True)
# Mesa and libwayland-cursor each carry an internal anonymous-file helper.
subprocess.run(["aarch64-none-elf-objcopy", "--redefine-sym",
                "os_create_anonymous_file=mesa_os_create_anonymous_file", temporary], check=True)
temporary.replace(destination)
