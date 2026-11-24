"""Boot upstream Mutter with a virtual output on both LixinOS kernels."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import time
from unittest.mock import patch

from disk_image import edit_files, prepare

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DBUS = runpy.run_path(str(HERE / "test-dbus.py"))
GUEST = DBUS["NATIVE"]
BUILD = ROOT / "Kernel/target/user-build/gnome-port"
WAYLAND = BUILD.parent / "wayland-port/sdk"
PORT = ROOT / "User/gnome"


def build_client(temporary):
    source = BUILD / "sysroot/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    header = temporary / "xdg-shell-client.h"
    protocol = temporary / "xdg-shell-protocol.c"
    scanner = WAYLAND / "bin/wayland-scanner"
    subprocess.run([scanner, "client-header", source, header], check=True)
    subprocess.run([scanner, "private-code", source, protocol], check=True)
    elf = temporary / "mutter-client"
    subprocess.run([BUILD / "cc", "-Os", "-I", temporary, "-I", WAYLAND / "include",
                    PORT / "tests/mutter-client.c", protocol, WAYLAND / "lib/libwayland-client.a",
                    "-lffi", "-L", BUILD / "sysroot/lib", "-o", elf], check=True)
    return elf


def install(disk, package, temporary):
    data = package / "lib/gnome"
    schemas = data / "schemas"
    schemas.mkdir(parents=True)
    for directory in (BUILD / "sysroot/share/glib-2.0/schemas", BUILD / "mutter-cross/data"):
        for file in directory.glob("*.xml"):
            if file.name.endswith((".gschema.xml", ".enums.xml")):
                shutil.copy2(file, schemas / file.name)
    subprocess.run(["glib-compile-schemas", "--strict", schemas], check=True)
    shutil.copytree(BUILD / "xkb-data/share/xkeyboard-config-2", data / "xkb")
    pack = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
    pack(package, "/bin/mutter", BUILD / "mutter-cross/src/mutter", library="/lib/gnome",
         build_dir=temporary, launcher_flag="-DLX_DBUS", deflate=True)
    pack(package, "/bin/mutter-client", build_client(temporary), library="/lib/gnome",
         build_dir=temporary, launcher_flag="-DLX_DBUS", deflate=True)

    def add(rows):
        for file in package.rglob("*"):
            path = b"/" + file.relative_to(package).as_posix().encode()
            if path not in rows:
                rows[path] = (bytes(8), file.read_bytes() if file.is_file() else None)

    DBUS["INSTALL"](DBUS["PACKAGE"], disk, "dbus")
    edit_files(disk, add)


def session(serial, runtime):
    GUEST["command"](serial, f"mkdir {runtime}")
    GUEST["command"](serial, f"export HOME=/ PATH=/bin XDG_RUNTIME_DIR={runtime} "
                      "GSETTINGS_SCHEMA_DIR=/lib/gnome/schemas GSETTINGS_BACKEND=memory")


def qmp_connection(control):
    assert "QMP" in json.loads(control.readline())

    def command(name, /, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            response = json.loads(control.readline())
            if "event" not in response:
                assert "return" in response, response
                return

    return command


def with_window(disk, release, kernel, exercise):
    harness = runpy.run_path(str(HERE / "test-panes.py"))
    resize = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
    boot = harness["serial"]["boot"]

    def selected(disk, command, prompt):
        return boot(disk, command + ["--kernel", kernel, "--release", release],
                    prompt=prompt)

    with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk)}), resize():
        with patch.dict(harness["serial"], {"boot": selected}):
            harness["test"](exercise, mode="--window", disk=disk, prompt=b"LixinOS Bash (C shell)")


def updated_client_visible(screen):
    _, dimensions, _, pixels = screen.split(b"\n", 3)
    width, height = map(int, dimensions.split())
    # Some QEMU Cocoa framebuffer sizes produce four-byte-aligned PPM rows.
    pitch, remainder = divmod(len(pixels), height)
    if remainder or pitch not in (3 * width, (3 * width + 3) & ~3):
        return False
    # The window canvas doubles logical pixels on the QEMU framebuffer.
    green, orange, purple, blue = (bytes.fromhex(c) * 2 for c in
                                    ("38b694", "ec713d", "b935ce", "42a9ed"))
    normal = green * 64 + orange * 64
    top_edit = green * 7 + purple * 13 + green * 44 + orange * 64
    bottom_edit = green * 64 + orange * 61 + blue * 3
    anchor = pixels.find(top_edit)
    if anchor < 11 * 2 * pitch:
        return False
    origin = anchor - 11 * 2 * pitch
    for y in range(96 * 2):
        expected = top_edit if 22 <= y < 40 else bottom_edit if y >= 182 else normal
        if not pixels.startswith(expected, origin + y * pitch):
            return False
    return True


def check_graphics(disk, release, kernel):
    def exercise(guest, control, directory):
        qmp = qmp_connection(control)
        qmp("qmp_capabilities")
        Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("2024\t1320\n")
        guest.until(b"$ ")
        session(guest, f"/tmp/lx-mutter-graphics-{kernel}")
        address = "unix:abstract=lx-mutter-graphics"
        GUEST["command"](guest, f"export DBUS_SESSION_BUS_ADDRESS={address}")
        GUEST["command"](guest, "dbus-daemon --nofork --config-file=/lib/dbus/session.conf "
                          f"--address={address} &")
        GUEST["command"](guest, "mutter-client wayland-lx-test hold &")
        guest.send(b"mutter --lixinos-display --wayland-display wayland-lx-test\r")
        frame = directory / "mutter.ppm"
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            if updated_client_visible(frame.read_bytes()):
                guest.until(b"Mutter painted the xdg-shell client frame", timeout=6)
                print(f"{kernel}: both damaged rectangles and all surrounding client pixels matched", flush=True)
                return
            time.sleep(.1)
        raise AssertionError(f"The Mutter client colors did not reach the QEMU display: {guest.buffer!r}")

    with_window(disk, release, kernel, exercise)


def check(release, kernels, graphics):
    with tempfile.TemporaryDirectory(prefix="lixin-mutter-") as temp:
        temp = Path(temp)
        disk = temp / "mutter.img"
        prepare(disk)
        with DBUS["boot"](disk, release, "rust") as serial:
            GUEST["check_boot_release"](serial, b"rust")
            GUEST["shutdown"](serial)
        install(disk, temp / "package", temp)
        for kernel in kernels:
            with DBUS["boot"](disk, release, kernel) as serial:
                GUEST["check_boot_release"](serial, kernel.encode())
                GUEST["command"](serial, "mutter --version", expected=b"mutter 49.0")
                session(serial, f"/tmp/lx-mutter-{kernel}")
                GUEST["command"](serial, "dbus-run-session -- /bin/mutter --virtual-monitor 320x240 "
                                  "--wayland-display wayland-lx-test &")
                GUEST["command"](serial, "mutter-client wayland-lx-test",
                                  expected=b"Mutter painted the xdg-shell client frame")
                print(f"{kernel}: Mutter composited the Wayland client", flush=True)
            if graphics:
                check_graphics(disk, release, kernel)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc"), action="append")
    parser.add_argument("--graphics", action="store_true")
    options = parser.parse_args()
    check(options.release, options.kernel or ("rust", "tcc"), options.graphics)
