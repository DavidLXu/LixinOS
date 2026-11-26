"""Drive the installed Code editor through the guest compositor on a disposable disk."""
import argparse
import os
from pathlib import Path
import runpy
import select
import subprocess
import tempfile
import time

from PIL import Image

here = Path(__file__).resolve().parent
mutter = runpy.run_path(str(here / "test-mutter.py"))


def exercise(guest, control, directory):
    qmp = mutter["qmp_connection"](control)
    qmp("qmp_capabilities")
    guest.until(b"$ ")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("1920\t1200\n")
    time.sleep(.4)
    filename = "code-roundtrip-" + str(os.getpid()) + ".txt"

    def command(line):
        mutter["GUEST"]["command"](guest, line, prompt=b"$ ")

    command("cd /tmp")
    command("echo hello > " + filename)

    def key(name):
        qmp("human-monitor-command", **{"command-line": f"sendkey {name} 40"})
        time.sleep(.13)

    frame = directory / "frame.ppm"
    def screen():
        qmp("screendump", filename=str(frame))
        with Image.open(frame) as image:
            return image.copy()

    def status_text(image, top):
        # Compare the filename and its dirty marker, ignoring the cursor location.
        return image.crop((0, top, image.width // 2, image.height)).tobytes()

    def open_window(name):
        guest.send(f"code {name}\r".encode())
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if select.select([guest.vm.stdout], [], [], 0)[0]:
                guest.buffer += os.read(guest.vm.stdout.fileno(), 8192)
                if b"[exit " in guest.buffer:
                    raise AssertionError(guest.buffer)
            image = screen()
            x = 3
            band = [y for y in range(image.height // 2, image.height)
                    if image.getpixel((x, y)) == (36, 56, 80)]
            if band and image.getpixel((image.width // 3, image.height // 2)) == (30, 31, 35):
                return min(band), status_text(image, min(band))
            time.sleep(.25)
        raise AssertionError(f"Editor window did not appear: {guest.buffer!r}")

    def save_and_close(name, expected, clean):
        top, original_status = clean
        def wait_status(modified):
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                image = screen()
                if (status_text(image, top) != original_status) == modified:
                    return
                time.sleep(.25)
            raise AssertionError(f"Timed out waiting for document modified={modified}")
        wait_status(True)
        key('ctrl-s')
        # A guest filesystem commit can take several seconds on a large disk.
        wait_status(False)
        key('ctrl-w')
        output = guest.until(b'$ ', timeout=180)
        guest.send(b"echo $?\r")
        result = guest.until(b"$ ")
        assert b"\n0\n" in result, (output, result)
        mutter["GUEST"]["command"](guest, "cat " + name, expected=expected, prompt=b"$ ")

    clean = open_window(filename)
    key("end")
    key("z")
    key("ctrl-z")
    key("x")
    save_and_close(filename, b'hellox\n', clean)
    clean = open_window(filename)
    key("end")
    key("y")
    save_and_close(filename, b'helloxy\n', clean)
    new_file = "new-" + filename
    clean = open_window(new_file)
    key("q")
    key("ctrl-a")
    key("ctrl-c")
    key("r")
    key("ctrl-a")
    key("ctrl-v")
    save_and_close(new_file, b'\nq', clean)
    print("Guest window, relative path, save/reopen, undo, new file and clipboard round trip passed.", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path)
    parser.add_argument("--kernel", choices=["rust", "tcc"], default="rust")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "code.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        mutter["with_window"](disk, args.release, args.kernel, exercise)
