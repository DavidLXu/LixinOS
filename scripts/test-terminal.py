"""Verify xterm rendering and screen restoration through the QEMU framebuffer."""
import json
from pathlib import Path
import runpy
import subprocess

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))


def exercise(guest, control, directory):
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
        path = directory / "terminal.ppm"
        qmp("screendump", filename=str(path))
        _, size, _, pixels = path.read_bytes().split(b"\n", 3)
        _, height = map(int, size.split())
        stride = len(pixels) // height

        def cells(row, column, width, start=0, end=44):
            return b"".join(
                pixels[y * stride + column * 66:y * stride + (column + width) * 66]
                for y in range(44 + 44 * row + start, 44 + 44 * row + end)
            )
        return cells

    qmp("qmp_capabilities")
    shell = harness["serial"]["SHELL"]
    program = [
        "#include <stdio.h>",
        "int main(void){",
        r'printf("\033[2J\033[HORIGINAL\033[2;20Hbaseline"); getchar();',
        r'printf("\033[?1049h\033[?25l\033]2;title\007\033[2;5H\033(0lqxk\033(B");',
        r'printf("\033[3;1HXXXX\033[3;3H\033[1K\033[2d\033[9G\342\226\210");',
        r'printf("\033[5;1H+-|+# ready"); getchar();',
        r'printf("\033[?1049l\033[?25h"); getchar(); return 0;',
        "}",
    ]
    for line in program:
        guest.expect(f"echo '{line}' >> screen.c\r".encode(), shell)
    guest.expect(b"tcc screen.c -o screen\r", shell)
    guest.send(b"./screen\r")
    guest.until(b"baseline")
    original = frame()(0, 0, 8)
    # Compare guest pixels with the shared 22x44 CoreText masks. Duplicated
    # 2x2 pixels still pass the positioning tests below, but blur thin strokes.
    atlas = directory / "menlo.gray"
    subprocess.run(["swift", Path(__file__).with_name("console-font.swift"), atlas], check=True)
    masks = atlas.read_bytes()
    bg, fg = bytes.fromhex("171b20"), bytes.fromhex("dadbd5")
    palette = [bytes((f*a + b*(255-a))//255 for b, f in zip(bg,fg)) for a in range(256)]
    reference = b"".join(palette[masks[((c-32)*44+y)*22+x]]
                         for y in range(44) for c in b"ORIGINAL" for x in range(22))
    assert original == reference, "terminal glyphs differ from the native-resolution Menlo atlas"
    guest.send(b"\r")
    guest.until(b"+-|+# ready")
    alternate = frame()
    assert alternate(0, 0, 8) != original, "main screen stayed visible"
    assert alternate(1, 4, 5) == alternate(4, 0, 5), "cursor positioning or line graphics shifted"
    assert alternate(2, 0, 3) == alternate(2, 4, 3), "erase-to-start did not clear the row"
    assert alternate(4, 11, 1, 41, 44) == alternate(4, 12, 1, 41, 44), "cursor stays visible"
    guest.send(b"\r")
    guest.until(b"\x1b[?1049l\x1b[?25h")
    assert frame()(0, 0, 8) == original, "leaving the alternate screen lost the main screen"
    guest.expect(b"\r", shell)
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    harness["test"](exercise, mode="--window")
    print("Verified xterm positioning, line graphics, cursor hiding and screen restoration.")
