"""Drive the original single race with held QEMU keys on a cloned game disk."""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from unittest.mock import patch

from PIL import Image
from audio_capture import check_samples

HERE = Path(__file__).resolve().parent
PANES = runpy.run_path(str(HERE / "test-panes.py"))
SHUTDOWN = runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"]


def exercise(guest, control, directory, screenshot):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **args):
        control.write(json.dumps(dict(execute=command, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def button(key, down):
        qmp("input-send-event", events=[dict(type="key", data=dict(
            down=down, key=dict(type="qcode", data=key)))])

    def tap(key):
        button(key, True)
        time.sleep(.12)
        button(key, False)

    def frame():
        path = directory / "screen.ppm"
        qmp("screendump", filename=str(path))
        _, dims, _, pixels = path.read_bytes().split(b"\n", 3)
        w, h = map(int, dims.split())
        return Image.frombytes("RGB", (w, h), pixels, "raw", "RGB", len(pixels)//h)

    def count(im, rect, predicate):
        # The test launcher uses the same Retina display geometry on both kernels.
        return sum(predicate(*rgb) for rgb in im.resize((850, 508)).crop(rect).getdata())

    qmp("qmp_capabilities")
    guest.send(b"nfs2 --trace\r")
    guest.until(b"movies/titleav.dct", timeout=90)
    for _ in range(30):
        try:
            guest.until(b"/art/main.qfs", timeout=1)
            break
        except TimeoutError:
            tap("esc")
    else:
        raise AssertionError("Game did not open the main menu")
    guest.until(b"gamedata/audio/pc/show2.mus", timeout=60)
    time.sleep(2)
    white = lambda r,g,b: min(r,g,b) > 210
    for _ in range(16):
        shot = frame()
        if count(shot, (200, 337, 268, 375), white) > 750:
            break
        tap("down")
        time.sleep(.8)
    else:
        raise AssertionError("RACE was not highlighted")
    if screenshot:
        shot.save(screenshot.with_name(screenshot.stem + "-menu.png"))
    tap("ret")
    guest.until(b"tracks/speeds/tr00.qal", timeout=120)
    red = lambda r,g,b: r > 80 and r > 2*g and r > 2*b
    for _ in range(100):
        if count(frame(), (315, 24, 355, 48), red) > 400:
            break
        time.sleep(.25)
    else:
        raise AssertionError("Proving Grounds racing HUD did not appear")
    button("up", True)
    guest.until(b"held navigation 1\r\n", timeout=15)
    gold = lambda r,g,b: r > 150 and g > 95 and b < 65
    for _ in range(120):
        shot = frame()
        # Two or more digits extend left of the right-aligned zero on the speedometer.
        if count(shot, (184, 51, 212, 77), gold) > 18:
            break
        time.sleep(.2)
    else:
        if screenshot:
            shot.save(screenshot)
        raise AssertionError("Speedometer stayed at a single digit with the throttle held")
    button("right", True)
    guest.until(b"held navigation 9\r\n", timeout=10)
    time.sleep(.45)
    button("right", False)
    guest.until(b"held navigation 1\r\n", timeout=10)
    if screenshot:
        frame().save(screenshot)
    button("up", False)
    guest.until(b"held navigation 0\r\n", timeout=10)
    guest.send(b"\x11")
    guest.until(b"$ ", timeout=20)
    SHUTDOWN(guest, timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True, help="installed disk with default racing options")
    parser.add_argument("--kernel", choices=("rust", "tcc"), required=True)
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nfs2-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp", "-c", args.disk.resolve(), disk], check=True)
        wave = Path(tmp) / "sound.wav"
        with patch.dict(os.environ, {"LIXINOS_AUDIO": f"wav,path={wave},out.frequency=48000",
                                     "LIXINOS_TCC_DISK": str(disk)}):
            PANES["test"](lambda g,c,d: exercise(g,c,d,args.screenshot),
                          mode="--window", disk=disk, prompt=b"$ ",
                          launcher_args=("--kernel", args.kernel, "--release", args.release))
        frames = check_samples(wave)
        print(f"{args.kernel}: menu, race, acceleration, simultaneous steering, release, exit; {frames} stereo frames")


if __name__ == "__main__":
    main()
