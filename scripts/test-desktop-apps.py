"""Exercise native LXE1 desktop builds, input, saving and reboot with QEMU."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))
WIDTH, HEIGHT = 2354, 1408
CW, CH = WIDTH // 2, (HEIGHT - 44) // 2
CX, CY = (CW - 376) // 2, (CH - 378) // 2


def exercise(guest, control, directory, install, screenshots):
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **args):
        control.write(json.dumps({"execute": command, "arguments": args}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    qmp("qmp_capabilities")
    prompt = harness["serial"]["SHELL"]
    if install:
        # LXE1 with a single legacy entry returning 37, plus zero-initialized data coverage.
        for source in (
            '#include <lixin.h>',
            'static volatile char zeroed[131072];',
            'static const unsigned fixture[]={0x3145584c,0,8,0,0x528004a0,0xd65f03c0};',
            'static const unsigned retired[]={0x3245584c,0,8,0,0,0,0x528004a0,0xd65f03c0};',
            'int main(void){if(zeroed[0] || zeroed[131071])return 1;',
            'if(lx_save_file("/bin/legacy-one",fixture,sizeof fixture))return 1;',
            'return lx_save_file("/bin/retired-format",retired,sizeof retired)!=0;}',
        ):
            guest.expect(f"echo '{source}' >> layout.c\r".encode(), prompt)
        guest.expect(b"tcc layout.c -o /bin/layout\r", prompt)
        guest.expect(b"/bin/layout\r", prompt)
        guest.expect(b"echo $?\r", prompt, b"\n0\n")
        for name in ("calculator", "text-editor"):
            guest.send(f"tcc /src/gui/{name}.c -o /bin/{name}\r".encode())
            output = guest.until(prompt)
            assert b"error:" not in output and b"failed" not in output, output
            guest.expect(f"ls /bin/{name}\r".encode(), prompt, name.encode())

        guest.expect(b"tcc /src/gui/calculator.c -o /00-calculator.app\r", prompt)

    guest.send(b"/bin/retired-format\r")
    assert b"[exit 37]" not in guest.until(prompt)
    guest.expect(b"echo $?\r", prompt, b"\n1\n")
    guest.expect(b"/bin/legacy-one\r", prompt)
    guest.expect(b"echo $?\r", prompt, b"\n37\n")

    def click(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (extent - 1))}}
            for axis, pixel, extent in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))])
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])

    frame = directory / "app.ppm"
    atlas = (Path(__file__).resolve().parent.parent / "Kernel/target/user-build/desktop.gray").read_bytes()

    def wait(color_x, color_y, color, text=None, name=None):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            magic, dims, depth, rgb = frame.read_bytes().split(b"\n", 3)
            assert dims == f"{WIDTH} {HEIGHT}".encode(), dims
            stride = len(rgb) // HEIGHT

            def pixel(x, y):
                offset = (44 + 2*y)*stride + 6*x
                return rgb[offset:offset+3]

            if pixel(color_x, color_y).hex() == color:
                matched = True
                if text:
                    x, y, label, light, ink = text
                    for i, ch in enumerate(label):
                        glyph = (ord(ch)-32)*18*22
                        for row in range(22):
                            for col in range(9):
                                alpha = atlas[glyph+row*18+col]
                                if alpha >= 235 or alpha == 0:
                                    bright = pixel(x+i*9+col, y+row)[0] > ink
                                    if bright != ((alpha >= 235) == light):
                                        matched = False
                if matched:
                    if name and screenshots:
                        if stride != WIDTH * 3:
                            packed = b"".join(rgb[y*stride:y*stride+WIDTH*3] for y in range(HEIGHT))
                            frame.write_bytes(b"\n".join((magic, dims, depth, packed)))
                        screenshots.mkdir(parents=True, exist_ok=True)
                        subprocess.run(["sips", "-s", "format", "png", frame, "--out",
                                        screenshots / name], check=True, stdout=subprocess.DEVNULL)
                    return
            time.sleep(0.04)
        if screenshots:
            subprocess.run(["sips", "-s", "format", "png", frame, "--out",
                            screenshots / "unexpected.png"], check=False, stdout=subprocess.DEVNULL)
        raise AssertionError((color, text, pixel(color_x, color_y).hex()))

    def display(label, screenshot=None):
        if install:
            wait(cx+20,cy+55,"293c3b",(cx+25,cy+59,label,True,150),screenshot)
            return
        wait(CX+20, CY+55, "223b43", (CX+27, CY+61, label, True, 150), screenshot)

    cx,cy=76+(CW-76-376)//2,(CH-378)//2
    guest.send(b"desktop\r")
    wait(1, 1, "281e2b")
    if install:
        click(38,192)
    else:
        click(38,35)
        left, top = 76+(CW-76-600)//2, (CH - 391)//2
        wait(left + 6, top + 52, "f9f9f4")
        click(left + 72, top + 95)
    display("0")
    for i in (16, 17, 9, 15, 13, 18):
        if install:
            click(cx+40+i%4*87,cy+120+i//4*54)
        else:
            click(CX+40+i%4*87, CY+132+i//4*49)
        time.sleep(0.07)
    display("2.5", "calculator.png" if install else None)
    guest.send(b"c1/0=")
    display("Cannot divide by zero")
    guest.send(b"c")
    display("0")
    if install:
        click(cx+355,cy+22)
    else:
        click(CX+40+3*87, CY+132+4*49)
    wait(1, 1, "281e2b")

    if not install:
        click(left + 580, top + 20)
    click(38,438)
    wait(4, 5, "233b43")
    if install:
        guest.send(b"Hello LXE1\rSecond line\x1b[D!")
        wait(20, 84, "fcfcf7", (28, CH-32, "Modified.", False, 130))
        click(CW-165, 31)
        guest.send(b"\x13")
        wait(20, 84, "fcfcf7", (28, CH-32, "Saved.", False, 130), "editor.png")
    wait(20, 84, "fcfcf7", (46, 91, "llo LXE1", False, 130),
         None if install else "editor-reboot.png")
    click(CW-60, 31)
    wait(1, 1, "281e2b")
    guest.expect(b"\x1b", prompt)
    guest.expect(b"cat /notes.txt\r", prompt, b"Hello LXE1\nSecond lin!e")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        disk = Path(folder) / "disk.img"
        for install in (True, False):
            harness["test"](lambda guest, control, directory: exercise(
                guest, control, directory, install, args.screenshots), mode="--window", disk=disk)
            print("Built and exercised both apps." if install else "Verified both apps and saved text after reboot.")
