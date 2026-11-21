"""Exercise concurrent desktop terminal tabs through QEMU tablet and UART."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import time

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-panes.py"))


def exercise(guest, control, directory, split):
    assert "QMP" in json.loads(control.readline())

    def qmp(name, **args):
        control.write(json.dumps(dict(execute=name, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            result = json.loads(control.readline())
            if "event" not in result:
                assert "return" in result, result
                return result["return"]

    qmp("qmp_capabilities")
    shell = harness["serial"]["SHELL"]
    if split:
        guest.until(shell)
    for line in (
        "#include <lixin.h>",
        "#include <stdio.h>",
        "long lx_console_call(long, long);",
        "int main(int argc, char **argv) {",
        "  if (argc != 5) return 1;",
        '  printf("%s-ready\\n", argv[1]);',
        "  while (lx_stat(argv[2]) < 0) lx_console_call(43, 10);",
        "  puts(argv[3]);",
        '  return lx_save_file(argv[4], "done\\n", 5);',
        "}",
    ):
        guest.expect(f"echo '{line}' >> tab-worker.c\r".encode(), shell)
    guest.expect(b"tcc tab-worker.c -o /tab-worker\r", shell)

    font = directory / "font.gray"
    subprocess.run(["swift", str(here / "console-font.swift"), str(font)], check=True)
    atlas = font.read_bytes()
    frame = directory / "tabs.ppm"
    cw, ch = 22, 44
    width, height = (3014 if split else 2354), 1408
    pane_w = (width - cw) // 4 if split else width // 2
    pane_h = (height - ch) // 2
    popup_cols = min(pane_w // 11 - 8, 76)
    popup_w = popup_cols * 11 + 12
    wx, wy = (pane_w - popup_w) // 2, (pane_h - (21 * 22 + 50)) // 2

    def click(x, y):
        axes = [(axis, round(pixel * 32767 / (bound - 1))) for axis, pixel, bound in
                (("x", 2 * x + 1, width), ("y", ch + 2 * y + 1, height))]
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=axis, value=value)) for axis, value in axes])
        for down in (True, False):
            qmp("input-send-event", events=[dict(type="btn", data=dict(button="left", down=down))])

    def capture():
        qmp("screendump", filename=str(frame))
        magic, size, depth, pixels = frame.read_bytes().split(b"\n", 3)
        assert (magic, size, depth) == (b"P6", f"{width} {height}".encode(), b"255")
        return pixels, len(pixels) // height

    def matches(pixels, stride, text):
        samples = []
        for col, char in enumerate(text):
            glyph = atlas[(ord(char) - 32) * cw * ch: (ord(char) - 31) * cw * ch]
            white = [i for i, a in enumerate(glyph) if a >= 245]
            black = [i for i, a in enumerate(glyph) if a == 0]
            samples.extend((i // cw, col * cw + i % cw, True) for i in white[::13])
            samples.extend((i // cw, col * cw + i % cw, False) for i in black[::97])

        def match_at(x, y):
            for row, col, ink in samples:
                index = (y + row) * stride + (x + col) * 3
                if (pixels[index] > 190) != ink:
                    return False
            return True

        left = 2 * (wx + 6)
        return any(match_at(left + cw * col, ch + 2 * (wy + 44) + ch * row)
                   for row in range(21) for col in range(popup_cols - len(text) + 1))

    def wait_tab(index):
        end = time.monotonic() + 6
        while time.monotonic() < end:
            pixels, stride = capture()
            point = (ch + 2 * (wy + 9)) * stride + (2 * (wx + 165 + 30 * index)) * 3
            if pixels[point:point + 3] == bytes.fromhex("395772"):
                return
            time.sleep(0.04)
        raise AssertionError(f"tab {index + 1} did not become active")

    guest.send(b"desktop\r")
    time.sleep(0.3)
    dock_step = min(82, (pane_h - 48) // 8)
    click(38, 8 + 3 * dock_step + 20)
    guest.until(shell)
    wait_tab(0)
    guest.send(b"/tab-worker first /first-go First-finished /first-done\r")
    guest.until(b"\nfirst-ready")

    click(wx + 164 + 30 + 9, wy + 21)
    guest.until(shell)
    wait_tab(1)
    guest.send(b"/tab-worker second /second-go Second-finished /second-done\r")
    guest.until(b"\nsecond-ready")
    qmp("human-monitor-command", **{"command-line": "sendkey ctrl-c 40"})
    guest.until(shell)
    guest.expect(b"touch /first-go\r", shell)

    end = time.monotonic() + 6
    while time.monotonic() < end:
        guest.send(b"cat /first-done\r")
        response = guest.until(shell)
        assert b"First-finished\n" not in response, response
        if b"\ndone\n" in response:
            break
        time.sleep(0.04)
    else:
        raise AssertionError("tab 1 did not finish after tab 2 received Ctrl+C")
    assert not matches(*capture(), "First-finished"), "hidden output reached tab 2"

    click(wx + 164 + 9, wy + 21)
    wait_tab(0)
    assert matches(*capture(), "First-finished"), "hidden output was lost"
    click(wx + popup_w - 21, wy + 22)
    end = time.monotonic() + 6
    while matches(*capture(), "First-finished"):
        assert time.monotonic() < end, "closing tab 1 did not show tab 2"
        time.sleep(.04)
    wait_tab(0)
    guest.expect(b"echo second-still-open\r", shell, b"\nsecond-still-open\n")
    click(wx + popup_w - 21, wy + 22)
    time.sleep(0.15)
    guest.send(b"\x1b")
    guest.until(shell)
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--split", action="store_true")
    args = parser.parse_args()
    harness["test"](lambda guest, control, directory: exercise(guest, control, directory, args.split),
                    "--split" if args.split else "--window")
    print("Verified live terminal tabs, output isolation, Ctrl+C routing and close.")
