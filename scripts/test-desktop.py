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
parser.add_argument("--guest-build", action="store_true")
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
    screen_width = 3014 if args.split else 2354
    screen_height = 1408
    pane_width = (screen_width-22)//4 if args.split else screen_width//2
    pane_height = (screen_height-44)//2
    panel_width = min(pane_width-32, 600)
    left, top = (pane_width-panel_width)//2, (pane_height-391)//2
    window_width = min(pane_width//11-8,76)*11+12
    wx, wy = (pane_width-window_width)//2,(pane_height-(21*22+44+6))//2
    atlas = (here.parent / "Kernel/target/user-build/desktop.gray").read_bytes()
    letters = "".join(map(chr, range(32, 127))) + "文件夹上一级名称类型大小空目录页字节本编辑器终端"
    frame = directory / "desktop.ppm"

    def point(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pos*32767/(limit-1))}}
            for axis, pos, limit in (("x", x*2+1, screen_width), ("y", 44+y*2+1, screen_height))])
    def click(x, y):
        point(x, y)
        for down in (True, False):
            qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":down}}])

    def drag(x, y, dx, dy):
        point(x,y)
        qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":True}}])
        point(x+dx,y+dy)
        qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":False}}])

    def capture():
        qmp("screendump", filename=str(frame))
        magic, dims, depth, data = frame.read_bytes().split(b"\n", 3)
        width, height = map(int, dims.split())
        assert magic == b"P6" and depth == b"255" and (width, height) == (screen_width, screen_height)
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

    def check_scrollback(x, y):
        point(x+300, y+150)
        def wheel(button):
            for down in (True, False):
                qmp("input-send-event", events=[{"type":"btn","data":{"button":button,"down":down}}])
        def top_rows(pixel):
            return b"".join(pixel(x+i,y+j) for j in range(0,66,2) for i in range(0,240,2))
        for n in range(19):
            guest.expect(f"echo scroll-{n}\r".encode(), prompt)
        before = top_rows(capture())
        for n in range(19,22):
            guest.expect(f"echo scroll-{n}\r".encode(), prompt)
        after = top_rows(wait(lambda p: top_rows(p) != before))
        wheel("wheel-up")
        wheel("wheel-up")
        wait(lambda p: top_rows(p) == before)
        wheel("wheel-down")
        wheel("wheel-down")
        wait(lambda p: top_rows(p) == after)

    prompt = harness["serial"]["SHELL"]
    if args.split:
        guest.until(prompt)
    check_scrollback(0,0)
    guest.expect(b"mkdir demo\r", prompt)
    if args.guest_build:
        guest.expect(b"tcc /src/tinycc/bootstrap.c -o /bin/tcc-new\r", prompt)
        guest.expect(b"/bin/tcc-new -I/src/tinycc/sdk /src/desktop/desktop.c -o /bin/desktop\r", prompt)
        guest.expect(b"echo $?\r", prompt, b"\n0\n")
    guest.send(b"desktop\r")
    wait(lambda p: matches(p,40,69,"文件",True), "desktop")
    point(160,110)
    wait(lambda p: p(162,116) == b"\xff\xff\xff")
    click(58,45)
    def demo_row(p):
        return next((i for i in range(9) if matches(p,left+42,top+83+30*i,"demo")), None)
    root = wait(lambda p: matches(p,left+40,top+11,"/") and demo_row(p) is not None, "files")
    drag(left+150,top+20,26,47)
    left, top = left+26, top+47
    root = wait(lambda p: matches(p,left+40,top+11,"/") and demo_row(p) is not None)
    # Drag past the top-left limit, then back; controls must follow the title.
    drag(left+150,top+20,1-(left+150),1-(top+20))
    wait(lambda p: matches(p,40,11,"/"))
    drag(150,20,left,top)
    root = wait(lambda p: matches(p,left+40,top+11,"/") and demo_row(p) is not None)
    click(left+66,top+95+30*demo_row(root))
    wait(lambda p: matches(p,left+40,top+11,"/demo") and matches(p,left+16,top+90,"空目录"), "demo")
    click(left+panel_width-70,top+22)
    wait(lambda p: demo_row(p) is not None, "parent")
    click(left+panel_width-20,top+22)
    wait(lambda p: p(left+panel_width-110,top+4) == bytes.fromhex("26494c"))
    click(145,45)
    dialog_x = (pane_width-440)//2
    wait(lambda p: matches(p,dialog_x+14,183,"文本编辑器"))
    drag(dialog_x+120,192,-38,27)
    wait(lambda p: matches(p,dialog_x-38+14,210,"文本编辑器"))
    click(dialog_x-38+419,172+27+22)
    wait(lambda p: p(dialog_x+130,220)==bytes.fromhex("26494c"))
    click(145,45)
    wait(lambda p: matches(p,dialog_x-38+14,210,"文本编辑器"))
    guest.send(b"demo/note.txt\r")
    guest.until(b"^Q Exit")
    guest.send(b"hello from nano")
    guest.until(b"hello from nano")
    wait(lambda p: matches(p,wx+14,wy+11,"文本编辑器"))
    drag(wx+130,wy+20,-24,37)
    ex,ey=wx-24,wy+37
    wait(lambda p: matches(p,ex+14,ey+11,"文本编辑器"),"editor")
    # Work in document coordinates after moving the window. Select backwards
    # across a newline, copy it, cut it, then paste the exact range twice.
    guest.send(b"\rxyz")
    guest.until(b"xyz")
    body_x, body_y = ex+6+11+2, ey+44+22+3
    drag(body_x+2*11, body_y+22, 4*11, -22)
    guest.until(b"Ln 1  Col 7")
    click(body_x+10*11, ey+44+20*22+4)  # Copy in the editor toolbar
    guest.until(b"Copied")
    guest.send(b"\x18")
    guest.until(b"hello z")
    guest.send(b"\x16\x16")
    guest.until(b"xyfrom nano")
    # Move vertically and replace the duplicated passage with one character.
    guest.send(b"\x1b[A")
    guest.until(b"Ln 2  Col 3")
    drag(body_x+2*11, body_y+22, 0, 22)
    guest.until(b"Ln 3  Col 3")
    guest.send(b"!")
    guest.until(b"xy!z")
    click(ex+window_width-21,ey+22)
    guest.until(b"Save changes?")
    guest.send(b"\x1b")
    guest.until(b"^Q Exit")
    click(ex+window_width-21,ey+22)
    guest.until(b"Save changes?")
    guest.send(b"y")
    wait(lambda p: not matches(p,ex+14,ey+11,"文本编辑器"))
    click(58,45)
    root = wait(lambda p: demo_row(p) is not None)
    click(left+66,top+95+30*demo_row(root))
    wait(lambda p: matches(p,left+42,top+83,"note.txt"))
    click(left+170,top+95)
    guest.until(b"hello from nano")
    # Copy in the reopened document, click a different line and paste using
    # the toolbar. Saving and reopening through the shell checks exact bytes.
    bx, by = wx+6+11+2, wy+44+22+3
    drag(bx, by, 5*11, 0)
    guest.until(b"Ln 1  Col 6")
    guest.send(b"\x03")
    guest.until(b"Copied")
    click(bx+4*11,by+22)
    guest.until(b"Ln 2  Col 5")
    click(bx+27*11,wy+44+20*22+4)
    guest.until(b"xy!zhello")
    guest.send(b"\x13")
    guest.until(b"Saved")
    guest.send(b"\x11")
    wait(lambda p: matches(p,left+42,top+83,"note.txt") and not matches(p,wx+14,wy+11,"文本编辑器"))
    click(left+panel_width-20,top+22)
    desktop = wait(lambda p: matches(p,222,69,"终端",True) and
                   p(left+panel_width-110,top+4) == bytes.fromhex("26494c"))
    click(239,45)
    guest.until(prompt)
    wait(lambda p: matches(p,wx+14,wy+11,"终端"), "terminal")
    check_scrollback(wx+6,wy+44)
    guest.expect(b"cd /demo\r", b"lixin:/demo$ ")
    guest.send(b"cat no")
    guest.until(b"cat no")
    point(wx+130,wy+20)
    before = wait(lambda p: matches(p,wx+14,wy+11,"终端"))
    # One continuous grab, reversing direction across both axes. Sample the
    # old footprint and the translated title, border and body at each stop.
    samples = [(x,y) for y in range(0,512,7) for x in range(0,window_width,7)
               if not (115 <= x <= 155 and 5 <= y <= 50)]
    qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":True}}])
    for dx,dy in ((1,0),(-1,0),(0,-1),(-18,-23),(23,40)):
        tx,ty=wx+dx,wy+dy
        point(tx+130,ty+20)
        def moved(p):
            if not matches(p,tx+14,ty+11,"终端"):
                return False
            for x,y in samples:
                if p(tx+x,ty+y) != before(wx+x,wy+y):
                    return False
                if not (tx <= wx+x < tx+window_width and ty <= wy+y < ty+512):
                    if p(wx+x,wy+y) != desktop(wx+x,wy+y):
                        return False
            return True
        wait(moved, f"terminal-drag-{dx}-{dy}")
    qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":False}}])
    click(tx+window_width-54,ty+22)
    wait(lambda p: matches(p,382,33,"1",True))
    click(239,45)
    guest.until(prompt)
    guest.expect(b"pwd\r",prompt,b"\n/\n")
    qmp("human-monitor-command", **{"command-line": "sendkey ctrl-bracket_right 40"})
    wait(lambda p: matches(p,382,33,"2",True))
    click(304,46)
    wait(lambda p: matches(p,382,33,"1",True))
    click(342,46)
    wait(lambda p: matches(p,tx+14,ty+11,"终端"))
    guest.expect(b"te.txt\r",b"lixin:/demo$ ",b"hello from nano\nxy!zhello")
    wait(lambda p: matches(p,tx+14,ty+11,"终端"), "restored-terminal")
    click(tx+window_width-21,ty+22)
    wait(lambda p: matches(p,382,33,"1",True))
    click(342,46)
    wait(lambda p: matches(p,wx+14,wy+11,"终端"))
    guest.send(b"python\r")
    guest.until(b">>> ")
    click(wx+window_width-21,wy+22)
    wait(lambda p: matches(p,222,69,"终端",True) and p(295,30)==bytes.fromhex("26494c"))
    click(58,45)
    wait(lambda p: demo_row(p) is not None)
    qmp("human-monitor-command", **{"command-line": "sendkey esc 40"})
    guest.until(prompt)
    guest.expect(b"ls -1 / | cat\r", prompt, b"\ndemo\n")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


harness["test"](exercise, "--split" if args.split else "--window")
print("Verified window dragging and close controls, editor save/cancel, file browsing, Bash minimize/restore and closing a running REPL.")
