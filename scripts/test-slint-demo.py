"""Verify the guest Slint settings panel through QEMU mouse events and pixels."""
import argparse
import json
import os
import re
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

here = Path(__file__).resolve().parent
root = here.parent
harness = runpy.run_path(str(here / "test-panes.py"))
native = runpy.run_path(str(here / "test-native-kernel.py"))
resize_source = runpy.run_path(str(here / "test-resize.py"))["resize_source"]
serial = harness["serial"]
prompt = serial["SHELL"]
package = root / "Kernel/target/user-build/slint-package"


def exercise(guest, control, directory, screenshot):
    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    frame = directory / "slint.ppm"

    def snapshot():
        qmp("screendump", filename=str(frame))
        _, dimensions, _, pixels = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        stride = len(pixels) // height

        def pixel(x, y):
            offset = (44 + 2*y)*stride + 6*x
            return pixels[offset:offset+3]

        return width, height, pixel

    def panel(width, height):
        view_w, view_h = width//22*11, (height//44 - 1)*22
        card_w = min(480, view_w - 28)
        return (view_w-card_w)//2, max(0, (view_h-254)//2), card_w

    def click(width, height, x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pos*32767/(bound-1))}}
            for axis, pos, bound in (("x", 2*x+1, width), ("y", 44+2*y+1, height))
        ])
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])

    guest.send(b"slint-demo --stats\r")
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        width, height, pixel = snapshot()
        if pixel(8, 8) == bytes.fromhex("121b29"):
            break
        time.sleep(.03)
    else:
        raise AssertionError(f"Settings panel not visible. Serial buffer: {guest.buffer!r}")

    def switch():
        w, h, pixel = snapshot()
        x, y, card_w = panel(w, h)
        left, mid = x + card_w - 92, y + 153
        knob = [dx for dx in range(52) if pixel(left+dx, mid) == bytes.fromhex("f4f8fd")]
        return (sum(knob)/len(knob) if knob else -1), pixel(left+8, mid)

    x, y, card_w = panel(width, height)
    before, _ = switch()
    assert 12 <= before <= 18, before
    click(width, height, x + card_w - 64, y + 150)
    samples = []
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        pos, color = switch()
        samples.append(pos)
        if pos >= 35 and color == bytes.fromhex("32bf9b"):
            break
    else:
        raise AssertionError(f"Switch never reached the on state: {samples}")
    assert any(19 <= pos <= 32 for pos in samples), f"No animation frame: {samples}"
    def resize(width, height):
        source = Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"])
        source.write_text(f"{width}\t{height}\n")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            w, h, pixel = snapshot()
            if (w, h) == (width, height) and pixel(8, 8) == bytes.fromhex("121b29"):
                x, y, card_w = panel(w, h)
                if pixel(x+14, y+94) == bytes.fromhex("1d2a3b"):
                    return x, y, card_w
            time.sleep(.04)
        raise AssertionError(f"Slint did not redraw at {width} x {height}; last frame {w} x {h}")

    x, y, card_w = resize(880, 880)
    assert switch()[0] >= 35, "resize lost the on state"
    click(880, 880, x + card_w - 64, y + 150)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        pos, _ = switch()
        if 12 <= pos <= 18:
            break
    else:
        raise AssertionError("Switch could not be clicked at its resized position")

    width, height = 1760, 1056
    x, y, card_w = resize(width, height)
    if screenshot:
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot],
                       check=True, stdout=subprocess.DEVNULL)
    click(width, height, x+card_w-41, y+41)
    output = guest.until(prompt)
    counts = re.search(rb"slint frames: full=(\d+) partial=(\d+) largest_partial=(\d+)", output)
    assert counts, output
    full, partial, pixels = map(int, counts.groups())
    assert full >= 3 and partial >= 2 and 0 < pixels < 30_000, counts.groups()
    print(f"full={full}, partial={partial}, largest partial frame={pixels} logical pixels")
    guest.expect(b"echo console-restored\r", prompt, b"\nconsole-restored\n")
    native["shutdown"](guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    args = parser.parse_args()
    installer = runpy.run_path(str(root / "User/gcc/install.py"))["install"]
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        for kind in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
            disk = directory / f"{kind}.img"
            launch = [here / "run-kernel.sh", "--kernel", "rust", "--serial"]
            with serial["boot"](disk, launch) as guest:
                if kind == "tcc":
                    native["command"](guest, "tcc /src/tinycc/bootstrap.c -o /bin/tcc")
                    native["command"](guest, "tcc --bootloader @/src/kernel/loader-sources -o /boot/loader.elf")
                    native["command"](guest, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
                native["shutdown"](guest)
            installer(package, disk, "slint-demo")
            if kind == "tcc":
                loader = directory / "loader.elf"
                loader.write_bytes(native["read_file"](disk, "/boot/loader.elf"))
            screenshot = args.screenshot
            if screenshot and args.kernel == "both":
                screenshot = screenshot.with_name(f"{screenshot.stem}-{kind}{screenshot.suffix}")
            with patch.dict(os.environ), resize_source():
                os.environ.pop("LIXINOS_KERNEL", None)
                if kind == "tcc":
                    os.environ["LIXINOS_KERNEL"] = str(loader)
                harness["test"](lambda guest, control, tmp: exercise(guest, control, tmp, screenshot),
                                mode="--window", disk=disk)
            print(f"{kind}: verified Slint click, animation pixels, resize, partial frames and return to shell.")


if __name__ == "__main__":
    main()
