"""Select local PNG and JPEG files through Desktop's wallpaper dialog and reboot."""
import argparse
import io
import json
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
import time

from PIL import Image
from disk_image import edit_files

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-panes.py"))
shell = harness["serial"]["SHELL"]


def fixture(format, color):
    # The center covers the display. Stretching leaves red borders at the sample points.
    image = Image.new("RGBA" if format == "PNG" else "RGB", (160, 40), "red")
    image.paste(color, (40, 0, 120, 40))
    output = io.BytesIO()
    image.save(output, format)
    return output.getvalue()


def exercise(guest, control, directory, rebooted):
    def qmp(command, **arguments):
        control.write(json.dumps(dict(execute=command, arguments=arguments)).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    screen = directory / "frame.ppm"

    def frame():
        qmp("screendump", filename=str(screen))
        _, dimensions, _, pixels = screen.read_bytes().split(b"\n", 3)
        width, height = map(int, dimensions.split())
        return Image.frombytes("RGB", (width, height), pixels, "raw", "RGB", len(pixels)//height, 1)

    image = frame()
    width, height = image.size

    def wait(x, y, expected):
        for _ in range(120):
            value = frame().getpixel((2*x, 44 + 2*y))
            if max(abs(a-b) for a,b in zip(value, expected)) <= 4:
                return
            time.sleep(.1)
        raise AssertionError(f"pixel at ({x},{y}) was {value}, expected {expected}")

    def button(x, y, name):
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=axis, value=round(point*32767/(limit-1))))
            for axis, point, limit in (("x", 2*x+1, width), ("y", 44+2*y+1, height))])
        for down in (True, False):
            qmp("input-send-event", events=[dict(type="btn", data=dict(button=name, down=down))])
            time.sleep(.08)

    def menu():
        button(250, 130, "right")
        wait(415, 140, (232,238,232))

    def pick(path):
        menu()
        button(285, 263, "left")
        time.sleep(.2)
        guest.send(path.encode() + b"\r")

    guest.until(b"$ ")
    guest.expect(b"cd /\r", shell)
    guest.send(b"desktop\r")
    if not rebooted:
        wait(90, 1, (56,37,62))
        pick("/wall-test/a picture.PNG")
        wait(90, 90, (33, 119, 92))
        wait(width//2 - 8, 90, (33, 119, 92))
        pick("/wall-test/bad.jpg")
        wait(520, (height-44)//2 - 28, (35,60,61))
        wait(90, 90, (33, 119, 92))
        # The path dialog stays open for correction after the error.
        guest.send(b"\x1b")
        time.sleep(.2)
        pick("/wall-test/photo.jpg")
    wait(90, 90, (54, 94, 192))
    wait(width//2 - 8, 90, (54, 94, 192))
    if rebooted:
        menu()
        button(280, 175, "left")
        wait(90, 1, (56,37,62))
    guest.send(b"\x1b")
    guest.until(shell)
    guest.expect(b"cat /home/lixin/.wallpaper\r", shell, b"0" if rebooted else b"/wall-test/photo.jpg")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=20) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True, help="an idle test seed with a filesystem snapshot")
    parser.add_argument("--desktop", type=Path, required=True)
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        disk = Path(temp)/"test.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        stamp = struct.pack("<II", *(2*[int(time.time())]))

        def stage(rows):
            if b"/wall-test" in rows:
                raise ValueError("test seed already contains /wall-test")
            for directory in (b"/bin", b"/home", b"/home/lixin", b"/wall-test"):
                rows.setdefault(directory, (stamp, None))
            rows.pop(b"/home/lixin/.wallpaper", None)
            for path, data in {
                b"/bin/desktop": args.desktop.read_bytes(),
                b"/wall-test/a picture.PNG": fixture("PNG", (28,164,107,128)),
                b"/wall-test/bad.jpg": b"not a picture",
                b"/wall-test/photo.jpg": fixture("JPEG", (54,94,192)),
            }.items():
                rows[path] = stamp, data

        edit_files(disk, stage)
        for rebooted in (False, True):
            harness["test"](lambda g,c,d: exercise(g,c,d,rebooted), disk=disk, mode="--window",
                            launcher_args=("--kernel","rust","--release",args.release),
                            prompt=b"LixinOS Bash (C shell)")
    print("PNG, JPEG, invalid image, persistence after reboot and preset restore passed.")


if __name__ == "__main__":
    main()
