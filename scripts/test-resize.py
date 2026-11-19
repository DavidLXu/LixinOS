"""Check that host window dimensions reflow the guest's fixed-size terminal cells."""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import time

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))


def exercise(guest, control, directory, split):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **arguments):
        control.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    def frame():
        path = directory / "resize.ppm"
        qmp("screendump", filename=str(path))
        _, size, _, pixels = path.read_bytes().split(b"\n", 3)
        width, height = map(int, size.split())
        stride = len(pixels) // height

        def cell(row, col):
            return b"".join(
                pixels[y * stride + col * 66:y * stride + (col + 1) * 66]
                for y in range(44 * (row + 1), 44 * (row + 2))
            )
        return (width, height), cell

    def resize(width, height):
        source = Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"])
        source.write_text(f"{width}\t{height}\n")
        end = time.monotonic() + 8
        while time.monotonic() < end:
            size, cell = frame()
            if size == (width, height):
                return cell
            time.sleep(0.04)
        raise AssertionError((size, (width, height)))

    def clock(width, cell):
        last = width // 22 - 1
        length = 8 if width >= 26 * 22 else 5
        glyphs = [cell(-1, col) for col in range(last - length, last)]
        ink = bytes.fromhex("d4e3ff")
        assert all(ink in glyph for glyph in glyphs), "clock is missing or cropped"
        assert cell(-1, last) == bytes.fromhex("273038") * (22 * 44), "clock has no right margin"
        if length == 8:
            assert glyphs[2] == glyphs[5], "clock must use HH:MM:SS"
        return glyphs

    qmp("qmp_capabilities")
    shell = harness["serial"]["SHELL"]
    if split:
        guest.until(shell)
    for line in (
        "#include <stdio.h>",
        "#include <lixin.h>",
        "int main(void){unsigned long d=lx_geometry();",
        r'printf("%lu cols, %lu rows\n",d&65535,d>>16&65535);return 0;}',
    ):
        guest.expect(f"echo '{line}' >> geometry.c\r".encode(), shell)
    guest.expect(b"tcc geometry.c -o geometry\r", shell)
    line = "a" * 65 + "X" + "z" * 20
    for source in (
        "#include <stdio.h>",
        r'int main(void){printf("\033[2J\033[H' + line + '");getchar();return 0;}',
    ):
        guest.expect(f"echo '{source}' >> layout.c\r".encode(), shell)
    guest.expect(b"tcc layout.c -o layout\r", shell)
    guest.send(b"./layout\r")
    guest.until(line.encode())
    (initial_width, _), initial = frame()
    glyph = initial(0, 65)
    assert glyph != initial(0, 64), "reference glyph was not drawn"
    initial_clock = clock(initial_width, initial)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        _, cells = frame()
        if clock(initial_width, cells)[6:] != initial_clock[6:]:
            break
        time.sleep(0.08)
    else:
        raise AssertionError("clock stopped while waiting for keyboard input")

    width = 1760 if split else 1320
    columns = (width // 22 - int(split)) // (2 if split else 1)
    smaller = resize(width, 880)
    clock(width, smaller)
    assert smaller(65 // columns, 65 % columns) == glyph, "resize scaled or clipped the glyph"
    width = 3322 if split else 2200
    columns = (width // 22 - int(split)) // (2 if split else 1)
    larger = resize(width, 1100)
    assert larger(0, 65) == glyph, "soft-wrapped text did not rejoin"
    guest.expect(b"\r", shell)
    guest.expect(b"./geometry\r", shell, f"{columns} cols, 24 rows".encode())

    def nano_cells(width, height):
        cols = (width // 22 - int(split)) // (2 if split else 1)
        rows = height // 44 - 1
        title = bytes.fromhex("89c9ca")
        footer = bytes.fromhex("565e6b")
        end = time.monotonic() + 8
        while time.monotonic() < end:
            size, cells = frame()
            if (size == (width, height)
                    and cells(0, cols - 1) == title * (22 * 44)
                    and cells(rows - 1, 0) == footer * (22 * 44)
                    and cells(rows - 1, cols - 1).startswith(footer * 22)):
                return cells
            time.sleep(0.04)
        raise AssertionError(f"Nano title or shortcuts missing at {width} × {height}")

    guest.send(b"nano resize-note.txt\r")
    guest.until(b"^U Paste line")
    guest.send(b"W")
    guest.until(b"Ln 1  Col 2")
    initial_nano = nano_cells(width, 1100)
    nano_glyph = initial_nano(1, 1)
    assert nano_glyph != initial_nano(1, 0), "Nano body glyph missing"
    for smaller_width, height in ((1760 if split else 1320, 792), (550 if split else 264, 264)):
        resize(smaller_width, height)
        cells = nano_cells(smaller_width, height)
        clock(smaller_width, cells)
        assert cells(1, 1) == nano_glyph, "Nano resized its font"
    guest.send(b"\x18")
    guest.until(b"Save? Y/N")
    resize(width, 1100)
    guest.until(b"Save changes? Y=yes  N=discard  Esc=cancel")
    guest.expect(b"y", shell)
    guest.expect(b"cat resize-note.txt\r", shell, b"\nW")

    project = b"/header-project"
    agent_prompt = b"\x1b[96magent> \x1b[0m"
    amber = bytes.fromhex("ffdf9e") * (22 * 44)
    green = bytes.fromhex("bde5ba") * (22 * 44)

    def agent_header(expected_size, badge):
        cols = (expected_size[0] // 22 - int(split)) // (2 if split else 1)
        end = time.monotonic() + 5
        while time.monotonic() < end:
            size, cells = frame()
            if size == expected_size and cells(0, cols - 1) == badge:
                assert cells(1, cols - 1) == bytes.fromhex("20272e") * (22 * 44)
                return cells(0, 1)
            time.sleep(0.04)
        raise AssertionError(f"Agent header failed to reach the right edge at {expected_size}")

    guest.expect(b"mkdir " + project + b"\r", shell)
    guest.expect(b"agent chat " + project + b"\r", agent_prompt, b"Bypass auto-approves")
    title_glyph = agent_header((width, 1100), amber)
    assert title_glyph != bytes.fromhex("273038") * (22 * 44), "Agent title is missing"

    guest.send(b"\r\n\r\n/help\r")
    help_text = guest.until(agent_prompt)
    assert b"Recall inputs" in help_text, help_text
    for _ in range(4):
        guest.expect(b"/help\r", agent_prompt, b"Recall inputs")
    assert agent_header((width, 1100), amber) == title_glyph

    narrow = (1769 if split else 1327, 792)
    resize(*narrow)
    assert agent_header(narrow, amber) == title_glyph
    guest.expect(b"/bypass\r", agent_prompt, b"Bypass off")
    assert agent_header(narrow, green) == title_glyph

    guest.send(b"/he")
    guest.until(b"/he")
    resize(width, 1100)
    guest.expect(b"lp\r", agent_prompt, b"Recall inputs")
    assert agent_header((width, 1100), green) == title_glyph
    guest.expect(b"/exit\r", shell)
    _, cells = frame()
    cols = (width // 22 - int(split)) // (2 if split else 1)
    assert cells(0, cols - 1) != green, "Agent header remained after returning to the shell"

    if not split:
        ssh = runpy.run_path(str(Path(__file__).with_name("test-ssh-out.py")))
        key = directory / "host-key"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key], check=True)
        password = "resize-fixture"
        with ssh["fixture"](key, 0, password) as port:
            guest.expect(f"ssh -p {port} guest@10.0.2.2\r".encode(), b"Trust this host (yes/no)? ")
            guest.expect(b"yes\r", b"password: ")
            guest.expect(password.encode() + b"\r", b"remote$ ", b"PTY xterm 100x24")
            resize(1980, 1056)
            guest.until(b"PTY resized to 90x23")
            guest.expect(b"exit\r", shell)

    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--split", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        swift = shutil.which("swift")
        assert swift
        source = directory / "size"
        source.write_text("")
        wrapper = directory / "swift"
        wrapper.write_text("""#!/usr/bin/env python3
import os, sys, time
from pathlib import Path
if len(sys.argv) < 2 or not sys.argv[1].endswith('/window-size.swift'):
    swift = os.environ['LIXINOS_TEST_SWIFT']
    os.execv(swift, [swift, *sys.argv[1:]])
size = Path(os.environ['LIXINOS_TEST_WINDOW_SIZE'])
previous = ''
while True:
    value = size.read_text()
    if value.endswith('\\n') and value != previous:
        print(value, end='', flush=True)
        previous = value
    time.sleep(0.03)
""")
        wrapper.chmod(0o700)
        os.environ["LIXINOS_TEST_SWIFT"] = swift
        os.environ["LIXINOS_TEST_WINDOW_SIZE"] = str(source)
        os.environ["PATH"] = f"{directory}:{os.environ['PATH']}"
        harness["test"](lambda guest, control, directory: exercise(guest, control, directory, args.split),
                        mode="--split" if args.split else "--window")
    print("Verified the live clock, resize, Nano layout and save, pinned Agent header, and identical 22×44 glyphs.")
