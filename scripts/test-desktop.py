"""Read QEMU framebuffer pixels and drive the desktop via its virtio input devices."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import time

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-panes.py"))
parser = argparse.ArgumentParser()
parser.add_argument("--screenshots", type=Path)
parser.add_argument("--split", action="store_true")
args = parser.parse_args()


def exercise(guest, control, directory):
    def qmp(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    pane_width = 572 if args.split else 902
    screen_width = 2310 if args.split else 1804
    panel_width = min(pane_width-32, 600)
    left, top = (pane_width-panel_width)//2, (528-391)//2
    atlas = (here.parent / "Kernel/target/user-build/desktop.gray").read_bytes()
    letters = "".join(map(chr, range(32, 127))) + "文件夹上一级名称类型大小空目录页字节本编辑器终端"
    frame = directory / "desktop.ppm"

    def point(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pos*32767/(limit-1))}}
            for axis, pos, limit in (("x", x*2+1, screen_width), ("y", 44+y*2+1, 1100))])
    def click(x, y):
        point(x, y)
        for down in (True, False):
            qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":down}}])

    def capture():
        qmp("screendump", filename=str(frame))
        magic, dims, depth, data = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dims.split())
        assert magic == b"P6" and depth == b"255" and height == 1100
        stride = len(data)//height
        if stride != width*3:
            # Some QEMU displays pad each PPM scanline to a 4-byte boundary.
            data = b"".join(data[row*stride:row*stride+width*3] for row in range(height))
            frame.write_bytes(b"\n".join((magic, dims, depth, data)))
        def pixel(x, y):
            start = 3*((44+2*y)*width+2*x)
            return data[start:start+3]
        return pixel

    # Compare the bright and dark strokes in every glyph, not serial debug output.
    def matches(pixel, x, y, text, light=False):
        for ch in text:
            idx = letters.index(ch)*18*22
            adv = 9 if ch.isascii() else 18
            for j in range(22):
                for i in range(adv):
                    a = atlas[idx+j*18+i]
                    if 16 < a < 235:
                        continue
                    bright = pixel(x+i,y+j)[0] > 150
                    if bright != ((a >= 235) == light):
                        return False
            x += adv
        return True

    def wait(check, name=None):
        limit = time.monotonic()+5
        while time.monotonic() < limit:
            pixel = capture()
            if check(pixel):
                if name and args.screenshots:
                    args.screenshots.mkdir(parents=True, exist_ok=True)
                    subprocess.run(["sips", "-s", "format", "png", frame, "--out",
                                    args.screenshots / (name+".png")], check=True, stdout=subprocess.DEVNULL)
                return pixel
            time.sleep(.05)
        if args.screenshots:
            args.screenshots.mkdir(parents=True, exist_ok=True)
            subprocess.run(["sips", "-s", "format", "png", frame, "--out", args.screenshots / "unexpected.png"], check=True)
        raise AssertionError(f"Unexpected framebuffer at {name or 'transition'}")

    prompt = harness["serial"]["SHELL"]
    if args.split:
        guest.until(prompt)
    guest.expect(b"mkdir demo\r", prompt)
    guest.send(b"desktop\r")
    wait(lambda p: matches(p,40,69,"文件",True), "desktop")
    point(160,110)
    wait(lambda p: p(162,116) == b"\xff\xff\xff")
    click(58,45)
    def demo_row(p):
        return next((i for i in range(9) if matches(p,left+42,top+83+30*i,"demo")), None)
    root = wait(lambda p: matches(p,left+40,top+11,"/") and demo_row(p) is not None, "files")
    click(left+66,top+95+30*demo_row(root))
    wait(lambda p: matches(p,left+40,top+11,"/demo") and matches(p,left+16,top+90,"空目录"), "demo")
    click(left+panel_width-70,top+22)
    wait(lambda p: demo_row(p) is not None, "parent")
    click(left+panel_width-20,top+22)
    wait(lambda p: p(left+panel_width-110,top+4) == bytes.fromhex("26494c"))
    click(145,45)
    wait(lambda p: matches(p,(pane_width-440)//2+14,181,"文本编辑器"))
    guest.send(b"demo/note.txt\r")
    guest.until(b"^X Exit")
    guest.send(b"hello from nano\x0f")
    guest.until(b"Saved")
    wait(lambda p: matches(p,24,39,"文本编辑器",True), "editor")
    guest.send(b"\x18")
    wait(lambda p: matches(p,222,69,"终端",True))
    click(58,45)
    root = wait(lambda p: demo_row(p) is not None)
    click(left+66,top+95+30*demo_row(root))
    wait(lambda p: matches(p,left+42,top+83,"note.txt"))
    click(left+170,top+95)
    guest.until(b"hello from nano")
    guest.send(b"\x18")
    wait(lambda p: matches(p,left+42,top+83,"note.txt"))
    click(left+panel_width-20,top+22)
    wait(lambda p: matches(p,222,69,"终端",True))
    click(239,45)
    guest.until(prompt)
    wait(lambda p: matches(p,24,39,"终端",True), "terminal")
    guest.expect(b"cd /demo\r", b"lixin:/demo$ ")
    guest.send(b"cat no")
    guest.until(b"cat no")
    click(111,49)
    wait(lambda p: matches(p,382,33,"1",True))
    click(239,45)
    guest.until(prompt)
    guest.expect(b"pwd\r",prompt,b"\n/\n")
    qmp("human-monitor-command", **{"command-line": "sendkey ctrl-bracket_right 40"})
    wait(lambda p: matches(p,382,33,"2",True))
    click(304,46)
    wait(lambda p: matches(p,382,33,"1",True))
    click(342,46)
    wait(lambda p: matches(p,24,39,"终端",True))
    guest.expect(b"te.txt\r",b"lixin:/demo$ ",b"hello from nano")
    wait(lambda p: matches(p,24,39,"终端",True), "restored-terminal")
    guest.send(b"exit\r")
    wait(lambda p: matches(p,382,33,"1",True))
    click(342,46)
    wait(lambda p: matches(p,24,39,"终端",True))
    guest.send(b"exit\r")
    wait(lambda p: matches(p,222,69,"终端",True) and p(295,30)==bytes.fromhex("26494c"))
    click(58,45)
    wait(lambda p: demo_row(p) is not None)
    qmp("human-monitor-command", **{"command-line": "sendkey esc 40"})
    guest.until(prompt)
    guest.expect(b"ls -1 / | cat\r", prompt, b"\ndemo\n")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


harness["test"](exercise, "--split" if args.split else "--window")
print("Verified desktop icon, software pointer, /demo, parent, close, editor save/reopen, two Bash windows, draft and cwd restoration, and shell output.")
