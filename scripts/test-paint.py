"""Exercise the graphical paint application using QEMU absolute pointer events."""
import argparse
import json
from pathlib import Path
import runpy
import time
import xml.etree.ElementTree as ET

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))


def exercise(guest, control, directory, guest_build=False):
    def qmp(name, **args):
        control.write(json.dumps({"execute": name, "arguments": args}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    def send(*events):
        qmp("input-send-event", events=list(events))
        time.sleep(.035)

    screen_width, screen_height = 3014, 1408
    pane_width = (screen_width-22)//2

    def point(x, y):
        send(*({"type": "abs", "data": {"axis": axis, "value": round(pos*32767/(limit-1))}}
               for axis, pos, limit in (("x", x, screen_width), ("y", y, screen_height))))

    def button(down, name="left"):
        send({"type": "btn", "data": {"button": name, "down": down}})

    def click(x, y):
        point(x, y)
        button(True)
        button(False)

    def drag(ax, ay, bx, by):
        point(ax, ay)
        button(True)
        point(bx, by)
        button(False)

    def screen():
        image = directory / "paint.ppm"
        qmp("screendump", filename=str(image))
        return image.read_bytes().split(b"\n",3)[-1]

    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    prompt = harness["serial"]["SHELL"]
    guest.until(prompt)
    time.sleep(.25)
    if guest_build:
        guest.expect(b"mkdir /libexec\r",prompt)
        guest.expect(b"tcc /src/tinycc/bootstrap.c -o /tcc-new\r",prompt)
        guest.expect(b"/tcc-new -I/src/tinycc/sdk /src/foreground/runtime.c /src/paint/main.c /src/paint/raster.c /src/paint/document.c -o /libexec/paint\r",prompt)
        guest.expect(b"echo $?\r",prompt,b"\n0\n")
    old = screen()
    guest.expect(b"paint sketch.svg\r", b"Wheel: width")
    time.sleep(.15)
    drag(100,400,150,470)
    drag(230,400,280,470)
    click(11*22,64)  # Arrow button
    click(5*22,110)  # Red swatch
    button(True,"wheel-up")
    button(False,"wheel-up")
    drag(350,420,530,420)
    click(17*22,64)  # Curve
    drag(160,620,580,620)
    click(350,460)
    click(23*22,64)  # Rectangle
    drag(660,400,780,480)
    click(28*22,64)  # Ellipse
    drag(810,400,960,480)
    point(1000,700)
    guest.expect(b"\x13", b"Saved SVG")
    pixels = screen()
    assert bytes.fromhex("cb4c50") in pixels
    stride = len(pixels)//screen_height  # QEMU pads each captured scanline.
    for y in range(44,screen_height):
        right = slice(y*stride+pane_width*3,(y+1)*stride)
        assert pixels[right] == old[right], f"neighbor changed on row {y}"
    guest.expect(b"q", prompt)
    guest.send(b"cat sketch.svg\r")
    result = guest.until(prompt)
    data = result[result.index(b"<svg"):result.index(b"</svg>")+6]
    svg = ET.fromstring(data)
    groups = svg.findall("{*}g")
    assert len(groups)==6, data
    assert groups[2].get("stroke")=="#cb4c50" and groups[2].get("stroke-width")=="5", data
    assert groups[2].find("{*}polygon") is not None and "Q" in groups[3][0].get("d")
    assert groups[4][0].tag.endswith("rect") and groups[5][0].tag.endswith("ellipse")
    guest.expect(b"paint sketch.svg\r", b"Wheel: width")
    time.sleep(.15)
    assert bytes.fromhex("cb4c50") in screen()
    guest.expect(b"zq", b"Unsaved")
    guest.expect(b"q", prompt)
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10)==0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--guest-build", action="store_true")
    guest_build = parser.parse_args().guest_build
    harness["test"](lambda *args: exercise(*args, guest_build))
    print("Passed mouse drawing, SVG round trip, wheel width and pane clipping.")
