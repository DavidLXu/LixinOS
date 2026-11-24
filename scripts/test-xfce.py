"""Boot both desktop commands on the same disk and exercise the Xfce panel."""
import argparse
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
import time
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
HARNESS = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
WIDTH, HEIGHT = 2024, 1320


def exercise(guest, control, directory):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **args):
        control.write(json.dumps({"execute": command, "arguments": args}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def click(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(v * 32767 / (limit - 1))}}
            for axis, v, limit in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))])
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])
            time.sleep(.18)

    def key(name):
        qmp("human-monitor-command", **{"command-line": "sendkey " + name + " 40"})

    frame = directory / "screen.ppm"

    def wait(test, title, timeout=100, required=True):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            _, size, _, pixels = frame.read_bytes().split(b"\n", 3)
            if size == f"{WIDTH} {HEIGHT}".encode():
                def pixel(x, y):
                    i = ((44+2*y)*WIDTH + 2*x)*3
                    return pixels[i:i+3].hex()
                result = test(pixel)
                if result:
                    return result
            time.sleep(.2)
        if not required:
            return False
        if path := os.environ.get("LIXINOS_UI_SNAPSHOT"):
            Path(path).write_bytes(frame.read_bytes())
        try:
            guest.until(b"__xfce_timeout__", timeout=.5)
        except TimeoutError as error:
            print("Guest console", error, flush=True)
        raise AssertionError("Timed out waiting for " + title)

    qmp("qmp_capabilities")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{WIDTH}\t{HEIGHT}\n")
    wait(lambda p: True, "resized console")
    guest.until(b"$ ", timeout=90)
    guest.expect(b"cd /xfce-check\r", b"$ ")
    guest.send(b"startxfce4\r")
    def home(p):
        return p(2, 52) != "384d66" and p(520, 8) == "203850"

    def shot(name):
        if folder := os.environ.get("LIXINOS_XFCE_TEST_FRAMES"):
            qmp("screendump", filename=str(Path(folder) / (name + ".ppm")))

    wait(home, "Xfce application launcher")
    time.sleep(1)
    shot("home")
    click(134, 19)  # Files
    wait(lambda p: p(2, 52) == "384d66" and len({p(x, y) for x in range(14, 460, 7)
         for y in range(100, 265, 5)}) > 30, "Thunar file listing")
    time.sleep(1)
    shot("files")
    def header(p):
        return tuple(p(x, y) for x in range(14, 132, 2) for y in range(48, 64, 2))
    file_header = wait(lambda p: header(p), "Thunar title")
    click(211, 212)
    time.sleep(.25)
    key("ret")
    wait(lambda p: p(2, 52) == "384d66" and header(p) != file_header
         and p(260, 390) != "e6edf4", "Mousepad opened the selected document")
    time.sleep(1)
    shot("editor")
    # Wait for the edit to reach the guest before asking it to save. On a
    # multi-gigabyte work disk the first GTK document can take seconds to load.
    click(112, 130)
    wait(lambda p: any(p(x, 123) != "f8fcf8" for x in range(66, 72)),
         "cursor at the end of the document's first line")
    key("x")
    wait(lambda p: len({x for x in range(67, 77) for y in range(128, 136)
                        if max(bytes.fromhex(p(x, y))) < 140}) >= 4,
         "appended x rendered by Mousepad")
    key("ctrl-s")
    time.sleep(2)
    click(47, 19)
    wait(home, "Applications with documents still open")
    click(212, 19)
    wait(lambda p: p(2, 52) == "384d66" and header(p) == file_header, "switched back to Thunar")
    click(312, 19)
    wait(lambda p: p(2, 52) == "384d66" and header(p) != file_header, "Mousepad after closing Thunar")
    time.sleep(.45)
    click(312, 19)
    if not wait(home, "closed application windows", timeout=12, required=False):
        # The GTK port can leave a saved Mousepad process alive. Exercise the
        # explicit second close and verify the file independently in the shell.
        click(312, 19)
        wait(home, "closed application windows")
    click(420, 19)
    guest.until(b"$ ", timeout=60)
    guest.expect(b"cat 'edit me.txt'\r", b"$ ", b"\noriginalx\n")
    guest.send(b"desktop\r")
    wait(lambda p: p(2, 2) == "281e2b", "original desktop on the same disk")
    key("esc")
    guest.until(b"$ ", timeout=60)
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=90) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--kernel", choices=["rust","tcc"], required=True)
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    boot = HARNESS["serial"]["boot"]
    def selected(disk, command):
        return boot(disk, command + ["--kernel", args.kernel, "--release", args.release],
                    prompt=b"LixinOS Bash (C shell)")
    with tempfile.TemporaryDirectory(prefix="xfce-test-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp","-c", args.disk, disk], check=True)
        root = HERE.parent
        installer = runpy.run_path(str(root / "User/gcc/install.py"))
        install = installer["package_edit"](root / "Kernel/target/user-build/xfce-port/package", "xfce")

        def fixture(rows):
            install(rows)
            if b"/xfce-check" in rows:
                raise ValueError("Test disk already contains /xfce-check")
            stamp = struct.pack("<II", *([int(time.time())] * 2))
            rows[b"/xfce-check"] = (stamp, None)
            rows[b"/xfce-check/edit me.txt"] = (stamp, b"original\n")

        installer["edit_files"](disk, fixture)
        with RESIZE(), patch.dict(HARNESS["serial"], {"boot":selected}):
            HARNESS["test"](exercise, mode="--window", disk=disk)
    print(f"{args.kernel}: Xfce application launch, switch, close, logout and original desktop passed")


if __name__ == "__main__":
    main()
