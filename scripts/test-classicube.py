"""Exercise upstream ClassiCube's local player and block edits in a QEMU guest."""
import argparse
import json
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

from disk_image import require_local

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PANES = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
PACKAGE = ROOT / "Kernel/target/user-build/classicube-port/package"


def exercise(guest, control, directory, screenshot):
    assert "QMP" in json.loads(control.readline())

    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    def move(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(value * 32767)}}
            for axis, value in (("x", x), ("y", y))])

    def click(button):
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": button, "down": down}}])
            time.sleep(.09)

    def line(prefix):
        guest.until(prefix, timeout=35)
        return guest.until(b"\n").strip()

    def snapshot(output=None):
        frame = directory / "world.ppm"
        deadline = time.monotonic() + 20
        while True:
            qmp("screendump", filename=str(frame))
            magic, size, depth, pixels = frame.read_bytes().split(b"\n", 3)
            width, height = map(int, size.split())
            stride = len(pixels) // height
            # ramfb aligns each scanline; PPM image readers expect packed RGB.
            rows = [pixels[y*stride:y*stride+width*3] for y in range(height)]
            if output:
                break
            # A flat grass map has blue sky above and green terrain below the horizon.
            sky = rows[height//4][(width//2)*3:][:3]
            ground = rows[height*3//4]
            greens = sum(g > r and g > b for r, g, b in
                         (ground[x:x+3] for x in range(0, width*3-3, 12)))
            if sky[2] > sky[0] and greens > width//16:
                break
            if time.monotonic() >= deadline:
                raise AssertionError(f"Flat world missing from framebuffer: sky={sky.hex()} green samples={greens}")
            time.sleep(.2)
        if output:
            frame.write_bytes(b"\n".join((magic, size, depth, b"".join(rows))))
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", output], check=True)

    qmp("qmp_capabilities")
    guest.send(b"classicube --flat --trace\r")
    initial = line(b"Player ")
    time.sleep(1)
    snapshot()
    for _ in range(9):
        qmp("human-monitor-command", **{"command-line": "sendkey w 40"})
        time.sleep(.11)
    moved = line(b"Player ")
    assert initial.split()[::2] != moved.split()[::2], (initial, moved)
    move(.5, .25)
    time.sleep(.15)
    move(.5, .63)
    time.sleep(.4)
    click("left")
    removed = line(b"Block ")
    assert re.fullmatch(rb"\d+ \d+ \d+: [1-9]\d* -> 0", removed), removed
    click("right")
    placed = line(b"Block ")
    assert re.fullmatch(rb"\d+ \d+ \d+: 0 -> [1-9]\d*", placed), placed
    assert removed.split(b":")[0] == placed.split(b":")[0], (removed, placed)
    qmp("human-monitor-command", **{"command-line": "sendkey ctrl-q 60"})
    guest.until(b"$ ", timeout=12)
    guest.expect(b"echo game-closed\r", b"$ ", b"\ngame-closed\n")
    if screenshot:
        guest.send(b"classicube --trace\r")
        line(b"Player ")
        time.sleep(4)  # Let the terrain mesh around the spawn point finish drawing.
        snapshot(screenshot)
        qmp("human-monitor-command", **{"command-line": "sendkey ctrl-q 60"})
        guest.until(b"$ ", timeout=15)
    runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"](guest)
    print("Player", initial, "->", moved, "removed", removed, "placed", placed, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    require_local(args.disk, args.disk.stat())
    with tempfile.TemporaryDirectory(prefix="classicube-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        subprocess.run(["python3", ROOT / "User/gcc/install.py", "--toolchain", "classicube",
                        PACKAGE, disk], check=True)
        boot = PANES["serial"]["boot"]
        def release_boot(disk, command):
            return boot(disk, [*command, "--kernel", args.kernel, "--release", args.release], prompt=b"$ ")

        with patch.dict(PANES["serial"], {"boot": release_boot}), RESIZE():
            PANES["test"](lambda g, c, d: exercise(g, c, d, args.screenshot), mode="--window", disk=disk)


if __name__ == "__main__":
    main()
