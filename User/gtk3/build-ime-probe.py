"""Build the GTK input integration fixture into an isolated package."""
import os
from pathlib import Path
import runpy
import shlex
import subprocess


def build(work):
    root = Path(__file__).resolve().parents[2]
    cache = root / "Kernel/target/user-build"
    gtk = cache / "gtk3-port"
    gnome = cache / "gnome-port"
    cross = next(cache.glob("arm-gnu-toolchain-*-aarch64-none-elf"))
    env = dict(os.environ, PKG_CONFIG_PATH="", PKG_CONFIG_LIBDIR=str(gtk / "sysroot/lib/pkgconfig"),
               PATH=str(cross/"bin")+os.pathsep+os.environ["PATH"])

    def flags(option):
        output = subprocess.check_output(["pkg-config","--static",option,"gtk+-3.0"],env=env,text=True)
        return [flag for flag in shlex.split(output) if flag != "-pthread"]

    binary = work / "gtk-ime-check.elf"
    subprocess.run([gtk/"cc","-Os","-ffunction-sections","-fdata-sections",
                    "-I"+str(cache/"cpython-port/musl-1.2.5/src/process"),
                    "-I"+str(gnome/"ibus-cross/src"),"-I"+str(gnome/"ibus-1.5.35/src"),
                    *flags("--cflags"),root/"User/gtk3/ime-probe.c",root/"User/rust/spawn.c",
                    "-Wl,--start-group",*flags("--libs"),"-Wl,--end-group","-o",binary],
                   env=env,check=True)
    package = work / "ime-test-package"
    runpy.run_path(str(root/"User/gcc/package.py"))["tool"](
        package,"/bin/gtk-ime-check",binary,library="/lib/gtk-ime-check",build_dir=work,
        launcher_flag="-DLX_DBUS",deflate=True)
    return package
