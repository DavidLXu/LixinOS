"""Exercise the graphical clipboard through QEMU keyboard and tablet input."""
import argparse
import json
import os
from pathlib import Path
import runpy
import tempfile
import time

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-panes.py"))


def exercise(guest, control, directory):
    assert "QMP" in json.loads(control.readline())

    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            response = json.loads(control.readline())
            if "event" not in response:
                assert "return" in response, response
                return response["return"]

    def hotkey(letter):
        qmp("human-monitor-command", **{"command-line": f"sendkey ctrl-shift-{letter} 40"})

    qmp("qmp_capabilities")
    shell = harness["serial"]["SHELL"]
    if split:
        guest.until(shell)
    guest.expect(b"clear\r", shell)
    guest.expect(b"echo guest-copy\r", shell, b"\nguest-copy\n")
    screenshot = directory / "clipboard.ppm"
    qmp("screendump", filename=str(screenshot))
    _, dimensions, _, _ = screenshot.read_bytes().split(b"\n", 3)
    width, height = (int(n) for n in dimensions.split())

    def pointer(x, y, state=None):
        events = [
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (bound - 1))}}
            for axis, pixel, bound in (("x", x, width), ("y", y, height))
        ]
        if state is not None:
            events.append({"type": "btn", "data": {"button": "left", "down": state}})
        qmp("input-send-event", events=events)

    clipboard = Path(os.environ["CLIPBOARD_TEST_FILE"])
    pointer(11, 110, True)
    pointer(209, 110)
    pointer(209, 110, False)
    hotkey("c")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and clipboard.read_bytes() != b"guest-copy":
        time.sleep(0.04)
    assert clipboard.read_bytes() == b"guest-copy", clipboard.read_bytes()

    clipboard.write_bytes(b"echo pasted\x03 from Mac\r\n")
    hotkey("v")
    guest.until(b"echo pasted from Mac ")
    try:
        guest.until(shell, timeout=0.15)
    except TimeoutError:
        pass
    else:
        raise AssertionError("pasted text submitted the shell command")
    guest.expect(b"\r", shell, b"\npasted from Mac\n")

    guest.expect(b"nano clipboard.txt\r", b"^U Paste line")
    clipboard.write_bytes(b"first\r\nsecond")
    hotkey("v")
    guest.until(b"second")
    guest.expect(b"\x0f", b"Saved")
    guest.expect(b"\x18", shell)
    guest.expect(b"cat clipboard.txt\r", shell, b"\nfirst\nsecond")
    guest.expect(b"kilo clipboard.c\r", b"Ctrl-F = find")
    clipboard.write_bytes(b"hello\nworld")
    hotkey("v")
    guest.until(b"world")
    guest.expect(b"\x13", b"bytes written on disk")
    guest.expect(b"\x11", shell)
    guest.expect(b"cat clipboard.c\r", shell, b"\nhello\nworld")
    guest.send(b"desktop\r")
    time.sleep(0.2)
    pointer(479, 135, True)
    pointer(479, 135, False)
    guest.until(shell)
    clipboard.write_bytes(b"echo popup paste")
    hotkey("v")
    guest.until(b"echo popup paste")
    guest.expect(b"\r", shell, b"\npopup paste\n")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--split", action="store_true")
    split = parser.parse_args().split
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        clipboard = directory / "clipboard"
        clipboard.write_bytes(b"")
        for name, command in (
            ("pbcopy", 'cat > "$CLIPBOARD_TEST_FILE"'),
            ("pbpaste", 'cat "$CLIPBOARD_TEST_FILE"'),
        ):
            stub = directory / name
            stub.write_text("#!/bin/sh\n" + command + "\n")
            stub.chmod(0o700)
        os.environ["CLIPBOARD_TEST_FILE"] = str(clipboard)
        os.environ["PATH"] = f"{directory}:{os.environ['PATH']}"
        harness["test"](exercise, mode="--split" if split else "--window")
    print("Verified guest-to-Mac copy, shell paste and multiline editor paste.")
