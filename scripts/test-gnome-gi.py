"""Check the GNOME GLib build can invoke GI and dispatch Wayland events."""
import argparse
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GUEST = runpy.run_path(str(HERE / "test-native-kernel.py"))
BUILD = ROOT / "Kernel/target/user-build"
PORT = ROOT / "User/gnome"


def boot(disk, release, kernel):
    command = ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
               "--kernel", kernel, "--release", release, "--serial"]
    return GUEST["guest"]["boot"](disk, command, prompt=b"Kernel ")


def check(release):
    gnome = BUILD / "gnome-port"
    cross, = BUILD.glob("arm-gnu-toolchain-*-aarch64-none-elf")
    dirs = [gnome / "sysroot", BUILD / "gtk3-port/sysroot", BUILD / "wayland-port/sdk"]
    env = dict(os.environ, PATH=f"{cross / 'bin'}:{os.environ['PATH']}", PKG_CONFIG_PATH="",
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(d / "lib/pkgconfig") for d in dirs))
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    with tempfile.TemporaryDirectory(prefix="lixin-gi-") as temp:
        temp = Path(temp)
        disk = temp / "gi.img"
        package = temp / "package"
        data = package / "lib/gnome"
        data.mkdir(parents=True)
        subprocess.run(["gi-compile-repository", PORT / "tests/LixinProbe-1.0.gir",
                        "-o", data / "LixinProbe-1.0.typelib"], check=True)
        for name, files, deps in (
            ("gi-probe", ["tests/gi-check.c"], ["girepository-2.0"]),
            ("wayland-loop-probe", ["tests/wayland-loop.c", "wayland-source.c"],
             ["glib-2.0", "wayland-server"]),
        ):
            flags = subprocess.check_output(["pkg-config", "--static", "--cflags", "--libs", *deps],
                                            env=env, text=True)
            elf = temp / name
            subprocess.run([gnome / "cc", "-Os", "-I", PORT, *(PORT / f for f in files),
                            "-Wl,--start-group", *shlex.split(flags), "-Wl,--end-group",
                            "-o", elf], env=env, check=True)
            pack(package, "/bin/" + name, elf, library="/lib/gnome", build_dir=temp,
                 launcher_flag="-DLX_DBUS", deflate=True)
        prepare(disk)
        with boot(disk, release, "rust") as serial:
            GUEST["check_boot_release"](serial, b"rust")
            GUEST["shutdown"](serial)

        def install(rows):
            for file in package.rglob("*"):
                path = b"/" + file.relative_to(package).as_posix().encode()
                if path not in rows:
                    rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

        edit_files(disk, install)
        for kernel in ("rust", "tcc"):
            with boot(disk, release, kernel) as serial:
                GUEST["check_boot_release"](serial, kernel.encode())
                GUEST["command"](serial, "gi-probe",
                                 expected=b"GI typelib resolved and invoked a static symbol")
                GUEST["command"](serial, "wayland-loop-probe",
                                 expected=b"GLib dispatched Wayland idle, pipe, and timer events")
                GUEST["shutdown"](serial)
            print(f"{kernel}: GI and the GLib Wayland event source passed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    check(parser.parse_args().release)
