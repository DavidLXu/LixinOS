"""Open the desktop browser, click through to example.com and inspect the framebuffer."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import time
import tempfile

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-panes.py"))


def exercise(guest, control, directory, screenshot, page_url=None, events=False):
    def qmp(name, **args):
        control.write(json.dumps(dict(execute=name, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    path = directory / "links.ppm"

    def frame():
        qmp("screendump", filename=str(path))
        _, size, _, pixels = path.read_bytes().split(b"\n", 3)
        width, height = map(int, size.split())
        stride = len(pixels) // height
        if stride != width * 3:
            pixels = b"".join(pixels[y*stride:y*stride+width*3] for y in range(height))
            path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)
        return width, height, pixels

    def click(x, y):
        width, height, _ = frame()
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=a, value=round(v*32767/(n-1))))
            for a, v, n in (("x", x*2+1, width), ("y", 44+y*2+1, height))])
        for down in (True, False):
            qmp("input-send-event", events=[dict(type="btn", data=dict(button="left", down=down))])

    def wait(check):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            width, height, pixels = frame()
            def pixel(x, y):
                i = ((44+y*2)*width+x*2)*3
                return pixels[i:i+3]
            if check(pixel):
                return width, height, pixels
            time.sleep(.1)
        raise AssertionError("Expected browser framebuffer did not appear")

    font = (here.parent / "Kernel/target/user-build/desktop.gray").read_bytes()

    def title(pixel, text, ox=94, oy=23, light=True):
        for index, ch in enumerate(text):
            glyph = (ord(ch)-32)*18*22
            for y in range(22):
                for x in range(9):
                    alpha = font[glyph+y*18+x]
                    if 16 < alpha < 235:
                        continue
                    if (pixel(ox+index*9+x, oy+y)[0] > 140) != ((alpha >= 235) == light):
                        return False
        return True

    if events:
        guest.send(f"links -g {page_url}/events.html\r".encode())
        width, _, pixels=wait(lambda pixel: title(pixel,"Waiting"))
        position=pixels.index(bytes.fromhex("185e93"),(44+96*2)*width*3)//3
        click((position%width)//2+1,(position//width-44)//2)
        wait(lambda pixel: title(pixel,"Server 42"))
        # The DOM result is painted in the body independently of the title.
        def body(pixel):
            for row in range(8):
                for col in range(4):
                    good=True
                    for i,ch in enumerate("Server 42"):
                        for y in range(22):
                            for x in range(9):
                                alpha=font[(ord(ch)-32)*18*22+y*18+x]
                                if 16<alpha<235: continue
                                if (pixel(26+9*(col+i)+x,96+22*row+y)[0]<145)!=(alpha>=235):
                                    good=False; break
                            if not good: break
                        if not good: break
                    if good: return True
            return False
        wait(body)
        if screenshot:
            subprocess.run(["sips","-s","format","png",path,"--out",screenshot],check=True)
        click(width//2-32,34)
        guest.until(harness["serial"]["SHELL"])
        guest.send(b"shutdown\r")
        assert guest.vm.wait(timeout=10)==0
        return

    if page_url:
        guest.send(f"links -g {page_url}/page.html\r".encode())
        width, _, pixels = wait(lambda pixel: title(pixel, "Guest JS"))
        def pictures(pixel):
            # Three known solid colors must cover consecutive interior pixels.
            colors=[(178,57,108),(24,124,171),(227,160,42)]
            found=set()
            for y in range(96,565,4):
                for i,color in enumerate(colors):
                    if all(all(abs(a-b)<=5 for a,b in zip(pixel(x,y),color)) for x in (40,50,60)):
                        found.add(i)
            return len(found)==3
        wait(pictures)
        font_path=here.parent/"Kernel/target/user-build/links-web/han.font"
        atlas=font_path.read_bytes()
        def chinese(p):
            # Find the exact adjacent 16x16 glyph masks in the first few body rows.
            for row in range(8):
                for col in range(4):
                    ok=True
                    for index,ch in enumerate("中文"):
                        slot=(ord(ch)-0x3000)*32
                        for y in range(16):
                            for x in range(16):
                                ink=atlas[slot+y*2+x//8] & (128>>(x%8))
                                color=p(12+14+col*9+index*18+1+x,12+84+22*row+3+y)
                                if bool(ink)!=(color==bytes.fromhex("172e32")):
                                    ok=False
                                    break
                            if not ok: break
                        if not ok: break
                    if ok: return True
            return False
        wait(chinese)
        if screenshot:
            subprocess.run(["sips","-s","format","png",path,"--out",screenshot],check=True)
        click(width//2-32,34)
        guest.until(harness["serial"]["SHELL"])
        guest.send(b"shutdown\r")
        assert guest.vm.wait(timeout=10)==0
        return

    desktop = lambda pixel: pixel(1,1).hex()=="281e2b"
    guest.send(b"desktop\r")
    wait(desktop)
    width,height,_=frame()
    bx,by=76+(width//2-76-850)//2, ((height-44)//2-540)//2
    click(38, 110)
    width, _, pixels = wait(lambda pixel: title(pixel, "Links 2.30",bx+14,by+11,False))
    # Locate the blue link underline below the toolbar.
    position = pixels.index(bytes.fromhex("185e93"), (44+96*2)*width*3)//3
    click((position % width)//2+1, (position//width-44)//2)
    wait(lambda pixel: title(pixel, "Example Domain",bx+14,by+11,False))
    if screenshot:
        subprocess.run(["sips", "-s", "format", "png", path, "--out", screenshot], check=True)
    click(bx+40,by+61)
    wait(lambda pixel: title(pixel, "Links 2.30",bx+14,by+11,False))
    click(bx+850-21,by+22)
    wait(desktop)
    guest.send(b"\x1b")
    guest.until(harness["serial"]["SHELL"])
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--pages", action="store_true", help="check native image, script and Chinese rendering")
    parser.add_argument("--events",action="store_true",help="click a button, fetch JSON and check the resulting pixels")
    args = parser.parse_args()
    if args.events:
        site=runpy.run_path(str(here/"test-links-pages.py"))["site"]
        with tempfile.TemporaryDirectory() as temp, site(Path(temp)) as url:
            harness["test"](lambda g,c,d: exercise(g,c,d,args.screenshot,url,True),mode="--window")
        print("Verified mouse click, interval cancellation, timeout, guest HTTP GET and DOM update.")
        raise SystemExit(0)
    if args.pages:
        site=runpy.run_path(str(here/"test-links-pages.py"))["site"]
        with tempfile.TemporaryDirectory() as temp, site(Path(temp)) as url:
            harness["test"](lambda g,c,d: exercise(g,c,d,args.screenshot,url),mode="--window")
        print("Verified PNG, JPEG and GIF pixels, adjacent Chinese glyphs, JavaScript title and Close.")
        raise SystemExit(0)
    harness["test"](lambda g, c, d: exercise(g, c, d, args.screenshot), mode="--window")
    print("Verified desktop launch, public HTTPS title pixels, mouse navigation, Back and Close.")
