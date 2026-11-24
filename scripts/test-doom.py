"""Check Freedoom's framebuffer and recorded input on a disposable LixinOS disk."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

from disk_image import disk_entries

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PANES = runpy.run_path(str(HERE / "test-panes.py"))
SHUTDOWN = runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"]
PACKAGE = ROOT / "Kernel/target/user-build/doom-port/package"


def exercise(guest, control, directory, screenshot):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **arguments):
        control.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    qmp("qmp_capabilities")
    guest.send(b"doom -warp 1 -nomonsters -record /doom-test\r")
    guest.until(b"ST_Init", timeout=45)
    frame = directory / "doom.ppm"
    deadline = time.monotonic() + 40
    previous = None
    while time.monotonic() < deadline:
        qmp("screendump", filename=str(frame))
        magic, dimensions, depth, pixels = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        stride = len(pixels) // height
        center = {
            pixels[y*stride+x*3:y*stride+x*3+3]
            for y in range(height//3, height*2//3, 7)
            for x in range(width//3, width*2//3, 7)
        }
        # Wait until the opening melt has uncovered a stable playfield.
        if center == previous and sum(max(color) - min(color) > 24 for color in center) > 24:
            break
        previous = center
        time.sleep(.45)
    else:
        raise AssertionError("Freedoom did not draw its first map")
    if screenshot:
        rows = b"".join(pixels[y*stride:y*stride+width*3] for y in range(height))
        frame.write_bytes(b"\n".join((magic, dimensions, depth, rows)))
        subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot], check=True)

    for key in ("w", "w", "w", "right", "right", "f", "f"):
        qmp("human-monitor-command", **{"command-line": f"sendkey {key} 60"})
        time.sleep(.16)
    qmp("human-monitor-command", **{"command-line": "sendkey ctrl-q 60"})
    guest.until(b"recorded", timeout=30)
    guest.until(b"$ ", timeout=20)
    SHUTDOWN(guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    boot = PANES["serial"]["boot"]

    def release_boot(disk, command):
        return boot(disk, [*command, "--kernel", args.kernel, "--release", args.release], prompt=b"$ ")

    with tempfile.TemporaryDirectory(prefix="doom-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        subprocess.run(["python3", ROOT / "User/gcc/install.py", "--toolchain", "doom",
                        PACKAGE, disk], check=True)
        with patch.dict(PANES["serial"], {"boot": release_boot}):
            PANES["test"](lambda g, c, d: exercise(g, c, d, args.screenshot), mode="--window", disk=disk)
        rows, _ = disk_entries(disk)
        demo = bytes(rows["/doom-test.lmp"][1])
        assert demo[0] == 109 and demo[-1] == 0x80 and (len(demo) - 14) % 4 == 0
        commands = [demo[i:i+4] for i in range(13, len(demo) - 1, 4)]
        assert any(0 < command[0] < 128 for command in commands), "missing forward movement"
        assert any(command[2] for command in commands), "missing turn"
        assert any(command[3] & 1 for command in commands), "missing attack"
    print(f"{args.kernel}: Freedoom rendered MAP01 and recorded movement, turning and firing.")


if __name__ == "__main__":
    main()
