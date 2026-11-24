"""Test a native session bus and its client under both released kernels."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NATIVE = runpy.run_path(str(HERE / "test-native-kernel.py"))
INSTALL = runpy.run_path(str(ROOT / "User/gcc/install.py"))["install"]
PACKAGE = ROOT / "Kernel/target/user-build/dbus-port/package"
ADDRESS = "unix:abstract=lixinos-dbus-test"


def boot(disk, release, kernel):
    launch = ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
              "--kernel", kernel, "--release", release, "--serial"]
    return NATIVE["guest"]["boot"](disk, launch, prompt=b"Kernel ")


def install_gdbus(disk, directory):
    build = ROOT / "Kernel/target/user-build"
    elf = build / "gnome-port/glib-cross/gio/gdbus"
    cross, = build.glob("arm-gnu-toolchain-*-aarch64-none-elf")
    package = directory / "gio"
    env = dict(os.environ, PATH=f"{cross / 'bin'}:{os.environ['PATH']}")
    subprocess.run(["python3", "-c", """
from pathlib import Path
import runpy
import sys

builder = runpy.run_path(sys.argv[1])["tool"]
package, elf, directory = map(Path, sys.argv[2:])
builder(package, "/bin/gdbus", elf, library="/lib/dbus", launcher_flag="-DLX_DBUS",
        build_dir=directory, deflate=True)
""", ROOT / "User/gcc/package.py", package, elf, directory], env=env, check=True)

    def add(rows):
        for file in package.rglob("*"):
            path = b"/" + file.relative_to(package).as_posix().encode()
            if path not in rows:
                rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

    edit_files(disk, add)


def check(release, gio=False):
    with tempfile.TemporaryDirectory(prefix="lixin-dbus-") as temp:
        disk = Path(temp) / "test.img"
        prepare(disk)
        with boot(disk, release, "rust") as serial:
            NATIVE["check_boot_release"](serial, b"rust")
            NATIVE["shutdown"](serial)
        INSTALL(PACKAGE, disk, "dbus")
        if gio:
            install_gdbus(disk, Path(temp))
        for kernel in ("rust", "tcc"):
            with boot(disk, release, kernel) as serial:
                NATIVE["check_boot_release"](serial, kernel.encode())
                command = NATIVE["command"]
                command(serial, "cd /")
                command(serial, "dbus-daemon --version", expected=b"D-Bus Message Bus Daemon")
                command(serial, "dbus-run-session -- dbus-send --session --print-reply --reply-timeout=5000 "
                                "--dest=org.freedesktop.DBus /org/freedesktop/DBus "
                                "org.freedesktop.DBus.ListNames",
                        expected=b'string "org.freedesktop.DBus"')
                serial.send(("dbus-daemon --nofork --config-file=/lib/dbus/session.conf"
                             f" --address={ADDRESS} --print-address=1 &\r").encode())
                ready = b"unix:abstract=lixinos-dbus-test,guid="
                if ready not in serial.until(NATIVE["shell"]):
                    serial.until(ready, timeout=15)
                    serial.until(b"\n")
                command(serial, f"dbus-send --bus={ADDRESS} --print-reply --reply-timeout=5000 "
                                "--dest=org.freedesktop.DBus /org/freedesktop/DBus "
                                "org.freedesktop.DBus.ListNames",
                        expected=b'string "org.freedesktop.DBus"')
                if gio:
                    command(serial, f"gdbus call --address {ADDRESS} --timeout 5 "
                                    "--dest org.freedesktop.DBus --object-path /org/freedesktop/DBus "
                                    "--method org.freedesktop.DBus.ListNames",
                            expected=b"['org.freedesktop.DBus',")
                NATIVE["shutdown"](serial)
            print(f"{kernel}: D-Bus authenticated and returned ListNames", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--gio", action="store_true", help="also use gdbus from the GNOME cross build")
    args = parser.parse_args()
    check(args.release, args.gio)
