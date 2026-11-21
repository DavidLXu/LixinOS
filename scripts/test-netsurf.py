"""Exercise the installed NetSurf desktop with real QEMU input and framebuffer pixels."""
import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
import threading
import time
import zlib

here = Path(__file__).resolve().parent
panes = runpy.run_path(str(here / "test-panes.py"))


def png(width=32, height=24, row=None):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress((b"\0" + (row or bytes.fromhex("ad337e") * width)) * height))
            + chunk(b"IEND", b""))


class Pages(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/undisplayed.png":
            self.server.hidden_requested = True
        pages = {
            "/": ("text/html; charset=utf-8", '<!doctype html><meta charset="utf-8"><title>A中国，大陆</title><link rel="stylesheet" href="/style.css"><div></div><img src="/sample.png"><img src="/sample.jpg"><p><a href="/two">Next</a></p>'.encode()),
            "/style.css": ("text/css", b"body{margin:0;height:1500px;background:#e2ebc7} div{width:50%;height:50px;background:#169a73}"),
            "/sample.jpg": ("image/jpeg", self.server.jpeg),
            "/sample.png": ("image/png", png()),
            "/two": ("text/html; charset=utf-8", ("<!doctype html><title>Next page</title>"
                "<body style='background:#973452;margin:0'><div style='background:white;color:black;width:140px'>"
                "<p style='margin:0;font-size:16px'>国Wié</p><p style='margin:0;font-size:32px'>国<b>Wié</b></p></div>").encode()),
        }
        if self.path in ("/js", "/expand.js"):
            name, mime = (("js-demo.html", "text/html") if self.path == "/js" else
                          ("expand.js", "text/javascript"))
            pages[self.path] = (mime + "; charset=utf-8", (here.parent / "User/netsurf" / name).read_bytes())
        if self.path == "/survey-logo.svg":
            pages[self.path] = ("image/svg+xml", b"""<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20" viewBox="0 0 40 20">
<defs><linearGradient id="g"><stop stop-color="#e04020"/><stop offset="1" stop-color="#2040e0"/></linearGradient></defs>
<path fill="url(#g)" d="M0 0h40v20H0z"/></svg>""")
        if self.path == "/survey":
            words = ("إختبار", "परीक्षा", "δοκιμή", "טעסט", "பரிட்சை", "테스트")
            page = """<!doctype html><meta charset=utf-8><style>
html{background:light-dark(#ededed,#101010)}body{margin:0;color:#345678}
img,svg{display:block}p{font-size:24px;line-height:40px;margin:0}i{display:inline-block;width:4px;height:12px;background:#cc2244}
</style><img src="/survey-logo.svg"><svg width="32" height="20" viewBox="0 0 32 20">
<g transform="translate(3 0)"><path fill="currentColor" d="M0 0h19v20H0z"/></g></svg>"""
            pages[self.path] = ("text/html; charset=utf-8", (page + "".join(
                "<p>" + "".join(f"&#{ord(c)};" for c in word) + "<i></i></p>" for word in words)).encode())
        if self.path == "/navigation-layout":
            pages[self.path] = ("text/html", b"""<!doctype html><style>
body{margin:0;background:white}
nav{display:flex;width:300px;justify-content:space-between}
nav i{display:block;width:20px;height:10px;background:currentColor}
nav{color:#694928}.outer{display:contents;color:#2378ab}.inner{display:inherit}
main{display:grid;width:210px;column-gap:10px;justify-content:center}
main:has(:nth-child(2)){grid-template-columns:min-content min-content}
.padded{--space:4px;--edge:1px;padding-inline:calc(var(--space) - var(--edge));
padding-block:calc(var(--space) - var(--edge))}
main b{width:34px;height:6px;background:#bc3456}
main b+b{width:54px;background:#258861}
aside{height:8px;background:#654321}aside:has(:nth-child(2)){background:#748599}
</style><nav><i></i><div class=outer><div class=inner><i></i><i></i></div></div><i></i></nav>
<main><b class=padded></b><!-- index ignores comments --><b class=padded></b></main>
<aside><span></span></aside><aside><span><i></i><i></i></span></aside>""")
        if self.path == "/link-borders":
            pages[self.path] = ("text/html", b"""<!doctype html><style>
body{margin:0;background:#eee}a{display:block;width:78px;height:22px;text-align:center}
.action{background:var(--fill);border:1px solid;border-color:var(--edge);
color:var(--ink);border-radius:var(--radius)}
.action{--fill:rgb(0,113,227);--edge:transparent;--ink:#fff;--radius:980px}
.secondary{--fill:transparent;--ink:rgb(0,102,204);--edge:var(--ink)}
</style><a class=action href=/two>A</a><a class="action secondary" href=/two>B</a>
<a href=/survey>Plain</a>""")
        if self.path == "/paint":
            pages[self.path] = ("text/html", b"""<!doctype html><link rel=stylesheet href=/paint.css>
<div hidden><img src=/undisplayed.png>Hidden content</div><section id=theme><i></i><i class=override></i></section>
<i></i><div class=cover></div><div class=contain></div><div class=sprite></div><div class=zero>Fallback</div><footer><img src=/sample.png width=10 height=9></footer>""")
        if self.path == "/paint.css":
            pages[self.path] = ("text/css", b""":root{--base:#936025;--logo:url(/sample.png)}
#theme{--accent:var(--base)}@media(max-width:20px){#theme{--accent:#abcdef}}
body{margin:0;background:white}i{display:block;width:20px;height:12px;background-color:#29477b;background-color:var(--accent)}
.override{background-color:#356a4a}
.cover,.contain{width:80px;height:24px}
.cover{background:#154643 url(/sample.png) center / cover no-repeat}
.contain{background:#154643 center no-repeat;background-image:var(--logo);background-size:contain}
footer{display:flex}.zero{height:12px;font-size:0;color:black;background:white}
.sprite{width:16px;height:10px;background-image:linear-gradient(transparent,transparent),url(/sample.png)}""")
        if self.path == "/column":
            pages[self.path] = ("text/html", b"""<!doctype html><style>
body{margin:0;background:white}main{display:flex;flex-direction:column;width:120px}
b{width:200px;height:5px;background:#833456}i{height:12px;background:#328469}
em{height:8px;max-width:65px;background:#4567aa}
</style><main><b></b><i></i><em></em></main>""")
        if self.path == "/rem":
            pages[self.path] = ("text/html", b"""<!doctype html><style>
html{font-size:12px;background:white}body{margin:0;font-size:1.5rem}
main{width:10em;height:1em;background:#396a43}
div{font-size:3rem;width:2em;height:1em;background:#8c3946}
section{font-size:16px;line-height:1.5em}article{font-size:28px;background:#a45a31}
section:last-of-type{line-height:1.5}section:last-of-type article{background:#3456a8}
</style><main></main><div></div><section><article>x<br>x</article></section>
<section><article>x<br>x</article></section>""")
        if self.path == "/tracks":
            pages[self.path] = ("text/html", b"""<!doctype html><style>
body{margin:0;background:white}main{display:grid;width:310px;height:100px;gap:10px;
grid-template-columns:minmax(30px,1fr) 2fr;grid-template-rows:1fr 2fr}
i{background:#555555}i+i{background:#775533}i+i+i{background:#009977;grid-column:1 / span 2}
</style><main><i></i><i></i><i></i></main>""")
        if self.path == "/hero":
            pages[self.path] = ("text/html", b"""<!doctype html><link rel=stylesheet href=/hero.css>
<main><div class=copy><b></b></div><div class=art><picture>
<source data-empty media="(min-width:0px)" srcset="data:image/gif;base64,R0lGODlhAQABAHAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==">
<img src="/banner.png"></picture></div></main>""")
        if self.path == "/banner.png":
            row = b"\xff\xff\xff"*180 + bytes.fromhex("8b389e")*40 + b"\xff\xff\xff"*180
            pages[self.path] = ("image/png", png(400,80,row))
        if self.path == "/hero.css":
            pages[self.path] = ("text/css", b""":root{--hero-height:60px}
@media(min-width:400px){:root{--hero-height:80px}}body{margin:0;background:#eeeeee}
main{display:flex;justify-content:center;position:relative;overflow:hidden;width:200px;height:var(--hero-height)}
.copy{position:relative;width:16px}.copy b{display:block;position:relative;z-index:2;height:12px;background:#c09941}
.art{position:absolute;bottom:0;width:100%;height:var(--hero-height)}
picture{display:block;height:100%}img{position:absolute;height:100%;width:auto;bottom:0;
inset-inline-start:50%;transform:translateX(-50%)}""")
        if self.path == "/layout":
            pages[self.path] = ("text/html; charset=utf-8", """<!doctype html><meta charset=utf-8>
<link rel=stylesheet href=/layout.css><body><main><div></div><div></div><div></div></main>
<picture><source media="(max-width: 50px)" srcset="/missing"><source type="image/png" media="(min-width: 200px)" srcset="/sample.png 1x, /missing 2x"><img src="/missing"></picture>
<section><p>W</p><p>W</p><p>W</p></section>
<article><aside>中日，文</aside><aside>中<br>日，<br>文</aside></article>
""".encode())
        if self.path == "/layout.css":
            pages[self.path] = ("text/css", b"""body{margin:0;background:white;color:black}
main{display:grid;width:232px;grid-template-columns:repeat(2,1fr);gap:7px 12px}
main div{height:20px;background:#e04030}main div+div{background:#2040e0}main div+div+div{background:#309060}
picture,img{display:block}section{height:65px}p{float:left;width:75px;margin:0;line-height:60px;font-size:13px}
p+p{font-size:21px}p+p+p{font-size:42px}aside{float:left;width:34px;margin-right:36px;font-size:16px;line-height:22px}article{clear:both}""")
        if self.path == "/go":
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()
            return
        if self.path not in pages:
            self.send_error(404)
            return
        mime, data = pages[self.path]
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.path == "/sample.jpg":
            # Keep a transfer pending when the user types a new URL after Back.
            time.sleep(.25)
        self.wfile.write(data)
    def log_message(self, *args):
        pass


def exercise(guest, control, directory, url, screenshot):
    def qmp(command, **args):
        control.write(json.dumps(dict(execute=command, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    width, height = 2354, 1408
    frame = directory / "screen.ppm"

    def wait(label, check):
        end = time.monotonic() + 25
        while time.monotonic() < end:
            qmp("screendump", filename=str(frame))
            header, size, depth, data = frame.read_bytes().split(b"\n", 3)
            assert header == b"P6" and size == f"{width} {height}".encode() and depth == b"255"
            stride = len(data) // height
            def pixel(x, y):
                start = (44 + 2*y)*stride + x*6
                return data[start:start+3].hex()
            if check(pixel):
                if label == "home" and screenshot:
                    frame.write_bytes(b"\n".join((header, size, depth, b"".join(
                        data[y*stride:y*stride+3*width] for y in range(height)))))
                    subprocess.run(["sips", "-s", "format", "png", frame, "--out", screenshot],
                                   check=True, stdout=subprocess.DEVNULL)
                print(label, flush=True)
                return pixel
            time.sleep(.1)
        raise AssertionError(f"NetSurf framebuffer did not show {label}")

    def click(x, y, button="left"):
        qmp("input-send-event", events=[
            dict(type="abs", data=dict(axis=a, value=round(v*32767/(n-1))))
            for a, v, n in (("x", 2*x+1, width), ("y", 44+2*y+1, height))])
        for down in (True, False):
            qmp("input-send-event", events=[dict(type="btn", data=dict(button=button, down=down))])

    prompt = panes["serial"]["SHELL"]
    guest.expect(b"netsurf --version\r", prompt, b"NetSurf 3.11")
    guest.send(b"desktop\r")
    wait("dock", lambda p: p(40,22) != p(600,22) and p(600,22) == p(820,22))
    click(38,110)
    wait("home", lambda p: p(870,330) == "edf4ef")
    guest.send(b"\x0c"+url.encode()+b"/go\r")
    def rendered(page):
        jpeg = bytes.fromhex(page(248,207))
        return (page(220,162) == "169a73" and page(780,162) == "e2ebc7" and
                page(216,207) == "ad337e" and all(abs(a-b) < 4 for a,b in zip(jpeg, bytes.fromhex("ad337e"))))
    wait("CSS, PNG and JPEG over HTTP", rendered)
    fonts = here.parent / "Kernel/target/user-build/netsurf-port/package/lib/netsurf/pingfang"
    def chinese_title(pixel):
        # The title uses the measured Latin advance followed by five 18px CJK cells.
        latin = (fonts.parent / "latin/0-00.gray").read_bytes()
        x, y = 215 + latin[ord("A")], 82 + 3
        for char in "中国，大陆":
            code = ord(char)
            data = (fonts / f"{code >> 8:02x}.gray").read_bytes()
            start = 32 + 640 * (code & 255)
            for row in range(16):
                for col in range(16):
                    pair = data[start + row*8 + col//2]
                    alpha = ((pair >> 4) if col % 2 == 0 else (pair & 15)) * 17
                    red = (0xe8 * (256-alpha) + 0x23 * alpha) >> 8
                    if int(pixel(x+col, y+row)[:2], 16) != red:
                        return False
            x += 18
        return True
    wait("Chinese window title", chinese_title)
    click(997,93)  # maximize, changing the width of the 50% green rectangle
    wait("maximized layout", lambda p: p(600,87) == "169a73" and p(720,87) == "e2ebc7")
    click(400,210,"wheel-down")
    wait("scrolled", lambda p: p(450,87) == "e2ebc7")
    click(400,210,"wheel-up")
    page = wait("scrolled back", lambda p: p(450,87) == "169a73")
    link = next((x,y) for y in range(140,240) for x in range(76,130)
                if page(x,y) == "0000ff")
    click(*link)
    wait("second page", lambda p: p(450,140) == "973452")
    def chinese_body(pixel):
        code = ord("国")
        data = (fonts / f"{code >> 8:02x}.gray").read_bytes()
        for scale in (1, 2):
            size = 16 * scale
            start = 32 + 640 * (code & 255) + (128 if scale == 2 else 0)
            mask = [n * 17 for byte in data[start:start + size*size//2]
                    for n in (byte >> 4, byte & 15)]
            # Match the raster's gray edge pixels too, allowing for integer rounding in libnsfb.
            def matches(x, y):
                return all(abs(int(pixel(x+i%size, y+i//size)[:2],16) - (255-a)) <= 2
                           for i,a in enumerate(mask))
            position = next(((x,y) for x in range(76,82) for y in range(72,145) if matches(x,y)), None)
            if position is None:
                return False
            x, y = position
            x += size
            style = 0 if scale == 1 else 2
            latin = (fonts.parent / "latin" / f"{style}-00.gray").read_bytes()
            for char in "Wié":
                index = ord(char)
                advance = latin[256*(scale-1) + index]
                start = 512 + 800*index + (160 if scale == 2 else 0)
                stride = 20*scale
                for row in range(size):
                    for col in range(advance):
                        i = row*stride + 2*scale + col
                        byte = latin[start + i//2]
                        alpha = 17*((byte >> 4) if i%2 == 0 else byte & 15)
                        if abs(int(pixel(x+col,y+row)[:2],16) - (255-alpha)) > 2:
                            return False
                x += advance
        return True
    wait("Mixed Chinese and proportional Latin at 16px and 32px", chinese_body)
    click(87,58)
    wait("back", lambda p: p(450,87) == "169a73")
    guest.send(b"\x0c"+url.encode()+b"/layout\r")
    def grid_origin(p):
        return next(((x,y) for y in range(72,105) for x in range(75,84)
                     if p(x,y)=="e04030"),None)
    def grid_ok(p):
        start = grid_origin(p)
        if not start: return False
        x,y=start
        return (p(x+109,y)=="e04030" and p(x+110,y)=="ffffff" and
                p(x+122,y)=="2040e0" and p(x,y+24)=="ffffff" and p(x,y+27)=="309060")
    screen=wait("CSS Grid columns, auto placement and gaps",grid_ok)
    x,y=grid_origin(screen)
    wait("picture media and srcset selection",lambda p: p(x+5,y+50)=="ad337e")
    def text_layout(p):
        widths=[]
        for i in range(3):
            ink=[a for a in range(65) for b in range(72,132) if int(p(x+75*i+a,y+b)[:2],16)<128]
            if not ink: return False
            widths.append(max(ink)-min(ink)+1)
        return (widths[0]<widths[1]<widths[2] and widths[2]>=30 and
                all(p(x+a,y+b)==p(x+70+a,y+b) for a in range(34) for b in range(137,199)) and
                any(p(x+a,y+b)!="ffffff" for a in range(34) for b in range(137,154)))
    wait("13px, 21px and 42px outlines and Chinese punctuation wrapping",text_layout)
    guest.send(b"\x0c"+url.encode()+b"/rem\r")
    wait("descendant rem font sizes resolve against the document root",
         lambda p: p(x+179,y+17)=="396a43" and p(x+180,y+17)=="ffffff"
         and p(x+71,y+53)=="8c3946" and p(x+72,y+53)=="ffffff"
         and p(x+71,y+54)=="a45a31")
    wait("inherited em line height stays fixed and a unitless multiplier scales",
         lambda p: p(x+200,y+101)=="a45a31" and p(x+200,y+102)=="3456a8"
         and p(x+200,y+185)=="3456a8" and p(x+200,y+186)=="ffffff")
    guest.send(b"\x0c"+url.encode()+b"/column\r")
    wait("oversized flex item leaves stretched siblings within the column",
         lambda p: p(x+199,y+4)=="833456" and p(x+119,y+8)=="328469"
         and p(x+120,y+8)=="ffffff" and p(x+64,y+20)=="4567aa"
         and p(x+65,y+20)=="ffffff")
    guest.send(b"\x0c"+url.encode()+b"/tracks\r")
    wait("minmax columns, fractional rows and a spanning item",
         lambda p: p(x+99,y+29)=="555555" and p(x+100,y+29)=="ffffff"
         and p(x+110,y+29)=="775533" and p(x+300,y+31)=="ffffff"
         and p(x+5,y+40)=="009977" and p(x+309,y+99)=="009977")
    guest.send(b"\x0c"+url.encode()+b"/hero\r")
    wait("center of oversized responsive image at the media-selected height",
         lambda p: p(x+100,y+79)=="8b389e" and p(x+79,y+79)=="ffffff"
         and p(x+120,y+79)=="ffffff" and p(x+100,y+80)=="eeeeee" and p(x+100,y+5)=="c09941")
    guest.send(b"\x0c"+url.encode()+b"/navigation-layout\r")
    wait("nested display:contents participates in one flex row and inherits color",
         lambda p: [p(x+dx,y+4) for dx in (2,95,190,282)] ==
         ["694928","2378ab","2378ab","694928"] and p(x+50,y+4)=="ffffff")
    wait("conditional two-column grid centers intrinsic tracks and matches nested children",
         lambda p: p(x+49,y+14)=="ffffff" and p(x+50,y+14)=="bc3456"
         and p(x+89,y+14)=="bc3456" and p(x+90,y+14)=="ffffff"
         and p(x+100,y+14)=="258861" and p(x+160,y+14)=="ffffff"
         and p(x+4,y+25)=="654321" and p(x+4,y+34)=="748599")
    guest.send(b"\x0c"+url.encode()+b"/link-borders\r")
    wait("link fills, scoped outline override, curved corners and plain links",
         lambda p: p(x,y)=="eeeeee" and p(x+58,y)=="0071e3"
         and p(x+8,y+11)=="0071e3" and p(x,y+24)=="eeeeee"
         and p(x+14,y+24)=="0066cc" and p(x+8,y+35)=="eeeeee"
         and p(x,y+35)=="0066cc" and p(x,y+50)=="eeeeee"
         and any(p(x+a,y+b)=="0000ff" for a in range(15,65) for b in range(49,68)))
    guest.send(b"\x0c"+url.encode()+b"/paint\r")
    wait("scoped theme aliases, source order, hidden content and background images",
         lambda p: p(x+2,y+5)=="936025" and p(x+2,y+17)=="356a4a"
         and p(x+2,y+29)=="29477b" and p(x+79,y+47)=="ad337e"
         and p(x+23,y+70)=="154643" and p(x+24,y+70)=="ad337e"
         and p(x+55,y+70)=="ad337e" and p(x+56,y+70)=="154643"
         and p(x+2,y+88)=="ad337e"
         and all(p(x+a,y+b)=="ffffff" for a in range(35) for b in range(94,106))
         and p(x+9,y+109)=="ad337e" and p(x+10,y+109)=="ffffff")
    guest.send(b"\x0c"+url.encode()+b"/survey\r")
    def survey_art(p):
        return (p(x+90,y+10)=="ededed" and int(p(x+2,y+10)[:2],16)>190 and
                int(p(x+37,y+10)[-2:],16)>190 and p(x+8,y+30)=="345678"
                and p(x+30,y+30)=="ededed")
    wait("external SVG gradient, inline SVG currentColor and light scheme",survey_art)
    def multilingual(p):
        # Widths at 24px from hb-shape for the packaged fonts, then three 24px Hangul cells.
        for row,width in enumerate((53,53,79,59,102,72)):
            top=y+40+40*row
            if not any(p(x+width+1,b)=="cc2244" for b in range(top,top+40)):
                return False
            if not any(p(x+a,b)=="345678" for a in range(width) for b in range(top,top+40)):
                return False
        return True
    wait("Arabic, Indic, Greek, Hebrew and Korean ink and advances",multilingual)
    for address in (url + "/js", "file:///lib/netsurf/js-demo.html"):
        guest.send(b"\x0c"+address.encode()+b"\r")
        wait("article before JavaScript click", lambda p: p(x+110,y+12)=="354f80"
             and p(x+150,y+32)=="267951")
        for count in (1, 2):
            click(x+48, y+16)
            top = y+32+38*(count-1)
            wait(f"JavaScript appends Chinese paragraph {count} and moves footer",
                 lambda p: p(x+150,top)=="efe3a1" and p(x+150,top+38)=="267951"
                 and any(int(p(x+a,top+b)[0:2],16)<80 for a in range(15) for b in range(7,31)))
    guest.send(b"\x0c"+url.encode()+b"/\r")
    wait("restored first page",lambda p: p(450,87)=="169a73")
    click(38,275)
    guest.until(prompt)
    guest.expect(b"echo netsurf-terminal\r", prompt, b"\nnetsurf-terminal\n")
    guest.send(b"exit\r")
    wait("browser after terminal exit", lambda p: p(450,87) == "169a73")
    click(1155,22)
    click(38,110)
    wait("reopened browser", lambda p: p(870,330) == "edf4ef")
    guest.send(b"\x1b\x1b")
    guest.until(prompt)
    guest.send(b"q\r")
    assert guest.vm.wait(timeout=15) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path)
    parser.add_argument("--kernel", choices=("tcc","rust"), required=True)
    parser.add_argument("--release", default="kernel-2026-11-20.5")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    os.environ["LIXINOS_TEST_SEED"] = str(args.disk.resolve(strict=True))
    boot = panes["serial"]["boot"]
    panes["serial"]["boot"] = lambda disk, cmd: boot(disk, [*cmd, "--kernel", args.kernel, "--release", args.release])
    with ThreadingHTTPServer(("127.0.0.1", 0), Pages) as server:
        with tempfile.TemporaryDirectory() as temporary:
            sample = Path(temporary) / "sample.png"
            sample.write_bytes(png())
            jpeg = sample.with_suffix(".jpg")
            subprocess.run(["sips", "-s", "format", "jpeg", sample, "--out", jpeg],
                           check=True, stdout=subprocess.DEVNULL)
            server.jpeg = jpeg.read_bytes()
        server.hidden_requested = False
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://10.0.2.2:{server.server_port}"
            panes["test"](lambda g,c,d: exercise(g,c,d,url,args.screenshot), "--window")
            assert not server.hidden_requested, "hidden image was fetched before layout"
        finally:
            server.shutdown()
