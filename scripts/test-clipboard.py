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

    def pointer(x, y, state=None, button="left"):
        events = [
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (bound - 1))}}
            for axis, pixel, bound in (("x", x, width), ("y", y, height))
        ]
        if state is not None:
            events.append({"type": "btn", "data": {"button": button, "down": state}})
        qmp("input-send-event", events=events)

    def menu(x, y, item):
        pointer(x, y, True, "right")
        pointer(x, y, False, "right")
        pointer(x + 27, y + 17 + 44 * item, True)
        pointer(x + 27, y + 17 + 44 * item, False)

    clipboard = Path(os.environ["CLIPBOARD_TEST_FILE"])
    pointer(11, 110, True)
    pointer(209, 110)
    pointer(209, 110, False)
    menu(280, 154, 0)
    # Clipboard bridge delivery is asynchronous.
    def copied(expected):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and clipboard.read_bytes() != expected:
            time.sleep(0.04)
        assert clipboard.read_bytes() == expected, clipboard.read_bytes()

    copied(b"guest-copy")

    clipboard.write_bytes(b"echo pasted\x03 from Mac\r\n")
    menu(280, 154, 1)
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
    guest.send(b"tcc /src/gui/text-editor.c -o /editor-test\r")
    output = guest.until(shell)
    assert b"error:" not in output, output
    guest.expect(b"echo 'select me' > mouse.txt\r", shell)
    guest.send(b"/editor-test mouse.txt\r")
    time.sleep(0.45)
    # Select "select" in the editor, then paste two lines over it.
    pointer(57, 237, True)
    pointer(165, 237)
    pointer(165, 237, False)
    menu(320, 153, 0)
    copied(b"select")
    clipboard.write_bytes(b"first\nsecond")
    menu(320, 153, 1)
    time.sleep(0.2)
    guest.send(b"\x13\x1b")
    guest.until(shell)
    guest.expect(b"cat mouse.txt\r", shell, b"\nfirst\nsecond me\n")
    guest.send(b"desktop\r")
    time.sleep(0.2)
    pane_width = (width - 22) // 4 if split else width // 2
    pane_height = (height - 44) // 2
    dock_step = min(82, (pane_height - 48) // 8)

    def click(x, y):
        pointer(x, y, True)
        pointer(x, y, False)

    def physical(x, y):
        return 2 * x + 1, 45 + 2 * y

    click(*physical(38, 8 + 4 * dock_step + 20))
    time.sleep(0.15)
    guest.send(b"mouse-nano.txt")
    time.sleep(0.15)
    dialog_x = 76 + (pane_width - 76 - 440) // 2
    dialog_y = (pane_height - 120) // 2
    pointer(*physical(dialog_x + 17, dialog_y + 57), True)
    pointer(*physical(dialog_x + 240, dialog_y + 57))
    pointer(*physical(dialog_x + 240, dialog_y + 57), False)
    menu(320, 153, 0)
    copied(b"/mouse-nano.txt")
    guest.send(b"\r")
    guest.until(b"^Q Exit")
    guest.send(b"mouse")
    guest.until(b"Ln 1  Col 6")

    popup_w = min(pane_width // 11 - 8, 76) * 11 + 12
    popup_x = (pane_width - popup_w) // 2
    popup_y = (pane_height - (21 * 22 + 50)) // 2
    text_x = popup_x + 6 + 11 + 2
    text_y = popup_y + 44 + 22 + 3
    pointer(*physical(text_x, text_y), True)
    pointer(*physical(text_x + 5 * 11, text_y))
    pointer(*physical(text_x + 5 * 11, text_y), False)
    menu(320, 153, 0)
    copied(b"mouse")
    clipboard.write_bytes(b"replacement")
    menu(320, 153, 1)
    guest.until(b"replacement")
    guest.send(b"\x13")
    guest.until(b"Saved")
    guest.send(b"\x11")
    time.sleep(0.15)
    click(*physical(38, 8 + 3 * dock_step + 20))
    guest.until(shell)
    clipboard.write_bytes(b"echo popup paste")
    menu(320, 153, 1)
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
    print("Verified right-click clipboard in the shell, desktop filename field, Nano and Text Editor.")
