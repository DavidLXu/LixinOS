"""Run the native Wayland client and server on both LixinOS kernels."""
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
NATIVE = runpy.run_path(str(HERE / "test-native-kernel.py"))
PACKAGE_EDIT = runpy.run_path(str(ROOT / "User/gcc/install.py"))["package_edit"]
BUILD = ROOT / "Kernel/target/user-build"


def install(disk, with_xfce):
    def packages(rows):
        PACKAGE_EDIT(BUILD / "wayland-port/package", "wayland")(rows)
        if with_xfce:
            for name in ("gtk3", "xfce"):
                if f"/lib/{name}/installed.json".encode() not in rows:
                    PACKAGE_EDIT(BUILD / f"{name}-port/package", name)(rows)

    generation = edit_files(disk, packages)
    print(f"{disk}: installed test packages, generation {generation}", flush=True)


def boot(disk, release, kernel):
    launch = ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
              "--kernel", kernel, "--release", release, "--serial"]
    return NATIVE["guest"]["boot"](disk, launch, prompt=b"Kernel ")


def check_graphics(disk, release, kernel, with_xfce):
    harness = runpy.run_path(str(HERE / "test-panes.py"))
    resize = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
    serial_boot = harness["serial"]["boot"]

    def selected(disk, command, prompt):
        return serial_boot(disk, command + ["--kernel", kernel, "--release", release],
                           prompt=prompt)

    def exercise(guest, control, directory):
        assert "QMP" in json.loads(control.readline())

        def qmp(command, **arguments):
            control.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
            control.flush()
            while True:
                response = json.loads(control.readline())
                if "event" not in response:
                    assert "return" in response, response
                    return

        def wait_frame(check):
            frame = directory / "frame.ppm"
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                qmp("screendump", filename=str(frame))
                _, dims, _, pixels = frame.read_bytes().split(b"\n", 3)
                if dims == b"2024 1320" and check(pixels):
                    return
                time.sleep(.1)
            raise AssertionError("Expected pixels did not appear in the QEMU framebuffer")

        def color_at(pixels, x, y):
            offset = ((44 + 2 * y) * 2024 + 2 * x) * 3
            return pixels[offset:offset + 3].hex()

        qmp("qmp_capabilities")
        Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("2024\t1320\n")
        wait_frame(lambda _: True)
        guest.until(b"$ ")
        guest.send(b"wayland-demo\r")
        colors = [bytes.fromhex(color) * 128 for color in ("42bba1", "3984d2", "eed796", "c482ad")]
        wait_frame(lambda pixels: all(color in pixels for color in colors))
        qmp("human-monitor-command", **{"command-line": "sendkey esc 40"})
        guest.until(b"Wayland shared buffer and timer passed")
        guest.until(b"$ ")

        if with_xfce:
            guest.send(b"startxfce4\r")
            wait_frame(lambda pixels: color_at(pixels, 520, 8) == "203850")
            time.sleep(.6)
            qmp("input-send-event", events=[
                {"type": "abs", "data": {"axis": axis, "value": round(pos * 32767 / limit)}}
                for axis, pos, limit in (("x", 841, 2023), ("y", 83, 1319))])
            time.sleep(.08)
            for pressed in (True, False):
                qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": pressed}}])
                time.sleep(.18)
            guest.until(b"$ ")

        guest.send(b"desktop\r")
        wait_frame(lambda pixels: color_at(pixels, 2, 2) == "281e2b")
        qmp("human-monitor-command", **{"command-line": "sendkey esc 40"})
        guest.until(b"$ ")
        NATIVE["shutdown"](guest)

    with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk)}), resize():
        with patch.dict(harness["serial"], {"boot": selected}):
            harness["test"](exercise, mode="--window", disk=disk, prompt=b"LixinOS Bash (C shell)")


def check(release, seed=None, graphics=False, with_xfce=False):
    with tempfile.TemporaryDirectory(prefix="lixin-wayland-") as temp:
        disk = Path(temp) / "test.img"
        if seed is None:
            prepare(disk)
            with boot(disk, release, "rust") as serial:
                NATIVE["check_boot_release"](serial, b"rust")
                NATIVE["shutdown"](serial)
        elif subprocess.run(["cp", "-c", seed, disk], capture_output=True).returncode:
            shutil.copyfile(seed, disk)
        install(disk, with_xfce)
        for kernel in ("rust", "tcc"):
            with boot(disk, release, kernel) as serial:
                NATIVE["check_boot_release"](serial, kernel.encode())
                serial.expect(b"cd /\r", NATIVE["shell"])
                for _ in range(2):
                    NATIVE["command"](serial, "wayland-demo --headless",
                                      expected=b"\nWayland shared buffer and timer passed\n")
                NATIVE["shutdown"](serial)
            print(f"{kernel}: Wayland roundtrip and timer passed twice", flush=True)
            if graphics:
                check_graphics(disk, release, kernel, with_xfce)
                sessions = "Xfce and Desktop" if with_xfce else "Desktop"
                print(f"{kernel}: displayed the Wayland client frame and switched to {sessions}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--disk", type=Path, help="optional seed disk copied before testing")
    parser.add_argument("--graphics", action="store_true", help="check the QEMU framebuffer and Desktop")
    parser.add_argument("--with-xfce", action="store_true", help="also launch Xfce on the graphics test disk")
    args = parser.parse_args()
    check(args.release, args.disk, args.graphics or args.with_xfce, args.with_xfce)
