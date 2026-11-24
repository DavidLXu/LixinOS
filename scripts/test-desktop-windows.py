"""Build a guest window client and exercise independent instances through QMP."""
from launcher_grid import installed_apps, select_icon
import json
from pathlib import Path
import runpy
import time

harness = runpy.run_path(str(Path(__file__).with_name("test-panes.py")))
WIDTH, HEIGHT = 2354, 1408
WORK_WIDTH, WORK_HEIGHT = WIDTH // 2, (HEIGHT - 44) // 2


def exercise(guest, control, directory):
    prompt = harness["serial"]["SHELL"]
    guest.send(b"tcc /src/desktop/counter.c -o /bin/counter.win\r")
    output = guest.until(prompt, timeout=30)
    assert b"error" not in output, output
    guest.expect(b"ls /bin/counter.win\r", prompt, b"counter.win")
    guest.send(b"tcc /src/desktop/stopwatch.c -o /bin/my-watch.win2\r")
    assert b"error" not in guest.until(prompt, timeout=30)
    guest.expect(b"echo '#include <stdio.h>' > frozen.c\r", prompt)
    guest.expect(b"echo '#include <lixin.h>' >> frozen.c\r", prompt)
    guest.expect(b"""echo 'int main(void){char p[32]; int n=snprintf(p,32,"%d",lx_pid()); lx_save_file("/frozen.pid",p,n); for (;;) {} }' >> frozen.c\r""", prompt)
    guest.expect(b"tcc frozen.c -o /bin/frozen.win\r", prompt)
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **args):
        control.write(json.dumps({"execute": command, "arguments": args}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def point(x, y):
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pixel * 32767 / (extent - 1))}}
            for axis, pixel, extent in (("x", 2*x+1, WIDTH), ("y", 44+2*y+1, HEIGHT))])

    def button(down):
        qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])

    def click(x, y):
        point(x, y)
        button(True)
        button(False)

    frame = directory / "desktop.ppm"

    def wait(check, description):
        stop = time.monotonic() + 8
        while time.monotonic() < stop:
            qmp("screendump", filename=str(frame))
            _, dims, _, data = frame.read_bytes().split(b"\n", 3)
            assert dims == f"{WIDTH} {HEIGHT}".encode()
            stride = len(data) // HEIGHT

            def pixel(x, y):
                offset = (44+2*y)*stride + 6*x
                return data[offset:offset+3].hex()

            if check(pixel):
                return
            time.sleep(.05)
        raise AssertionError(f"Desktop did not show {description}: {guest.buffer[-1000:]!r}")

    guest.send(b"ls -1 /bin\r")
    apps = installed_apps(guest.until(prompt))

    def open_menu():
        click(36, 8+7*min(82, (WORK_HEIGHT-48)//8)+19)
        wait(lambda p: p(82, 6) == "203f51", "Applications menu")

    def new_app(name="counter.win"):
        open_menu()
        select_icon(click,qmp,WORK_WIDTH,WORK_HEIGHT,apps.index(name.encode())+6,len(apps)+6)

    left = 76 + (WORK_WIDTH-76-360)//2
    top = (WORK_HEIGHT-246)//2
    def counter(slot, value, down=False):
        x, y = left + slot*28, top + slot*28 + 44
        mask = (63, 6, 91, 79, 102, 109, 125, 7, 127, 111)[value]
        segments = ((78,19),(88,26),(88,42),(78,53),(69,42),(69,26),(78,36))
        def matches(p):
            return (p(x+6, y+12) == ("104450" if down else "082c38") and
                    all((p(x+(sx*360+127)//128, y+(sy*202+71)//72) == "f8f4f0") == bool(mask & (1<<i))
                        for i, (sx,sy) in enumerate(segments)))
        wait(matches, f"counter {slot+1} showing {value}, pressed={down}")

    qmp("qmp_capabilities")
    guest.send(b"desktop\r")
    wait(lambda p: p(2, 2) == "281e2b", "dock")
    new_app()
    counter(0, 0)
    guest.send(b"+++")
    counter(0, 3)
    new_app("frozen.win")
    wait(lambda p: p(left+28+360-28, top+28+13) == "f1d6cc", "unresponsive window")
    click(left+28+360-19, top+28+22)
    counter(0, 3)
    time.sleep(.4)  # Allow the closed client to exit and its slot to be reaped.
    new_app()
    counter(1, 0)
    point(left+100, top+130)
    button(True)
    counter(1, 1, down=True)
    point(left-25, top+130)
    button(False)
    counter(1, 1)
    click(left+28+360-85, top+28+22)
    counter(0, 3)
    open_menu()
    click(WORK_WIDTH-118, 30)
    wait(lambda p: p(410, 8) == "233c3d", "Windows tab")
    click(150, 92)
    counter(1, 1)
    guest.send(b"\x1d")
    counter(0, 3)
    guest.send(b"\x1d")
    counter(1, 1)
    new_app()
    counter(2, 0)
    new_app()
    counter(3, 0)
    new_app()
    wait(lambda p: p(10, WORK_HEIGHT-16) == "233c3d", "window limit feedback")
    for slot in (2, 1):
        guest.send(b"\x1b")
        counter(slot, 1 if slot == 1 else 0)
    guest.send(b"\x1b")
    counter(0, 3)
    guest.send(b"\x1b")
    wait(lambda p: p(left+6, top+56) != "082c38", "closed client")
    # Reflow moves the fixed-size play control to the new right edge.
    def color(value):
        return f"{((value & 0xf800)<<8) | ((value & 0x7e0)<<5) | ((value & 31)<<3):06x}"
    back, play, pause = map(color, (0x1127, 0x568f, 0xf4e8))
    def face(x, y, width, running=False):
        expected = pause if running else play
        wait(lambda p: p(x+width-44, y+14) == expected and p(x+width-47, y+14) == back,
             f"native-pixel control at width {width}")
    def digit_signature(p, x, y, w, h, digit=6):
        scale = min((w-24)//27, (h-58)//5)
        xx = x+(w-27*scale)//2+4*digit*scale
        yy = y+40+(h-40-5*scale)//2
        return tuple(p(xx+i*scale+scale//2, yy+j*scale+scale//2) for j in range(5) for i in range(3))

    new_app("stopwatch.win2")
    body = top+44
    face(left, body, 360)
    point(left+356, top+242)
    button(True)
    point(left+476, top+302)
    button(False)
    face(left, body, 480)
    click(left+480-24, body+18)
    point(80, 120)
    face(left, body, 480, True)
    original = []
    def ticking(p):
        glyph = digit_signature(p, left, body, 480, 262)
        if not original: original.append(glyph)
        return original[0] != glyph
    wait(ticking, "timer-driven change with no input")
    guest.send(b" ")
    face(left, body, 480)
    guest.send(b"r")
    click(left+480-52, top+22)
    face(76, 44, WORK_WIDTH-76)
    click(WORK_WIDTH-52, 22)
    face(left, body, 480)
    guest.send(b"r ")
    face(left, body, 480, True)
    click(left+480-86, top+22)
    wait(lambda p: p(left+20, body+12) != back, "minimized stopwatch")
    time.sleep(1.2)
    open_menu()
    click(WORK_WIDTH-118, 30)
    click(150, 60)
    face(left, body, 480, True)
    zero = tuple(color(0x2249 if mark == "0" else 0xe77a) for mark in "111101101101111")
    wait(lambda p: digit_signature(p, left, body, 480, 262, 4) != zero,
         "seconds catch up after restore")
    new_app()   # A ticking window stays behind a legacy window.
    counter(1, 0)
    time.sleep(.3)
    counter(1, 0)
    guest.send(b"\x1b")
    face(left, body, 480, True)
    guest.send(b"\x1b")
    wait(lambda p: p(left+20, body+12) != back, "closed stopwatch")
    new_app("my-watch.win2")
    face(left, body, 360)
    guest.send(b"\x1b")
    wait(lambda p: p(left+20, body+12) != back, "closed guest-built adaptive client")
    guest.expect(b"\x1b", prompt)
    guest.send(b"cat /frozen.pid\r")
    pid = guest.until(prompt).split(b"\n", 1)[1].split(b"\x1b", 1)[0].strip()
    assert pid.isdigit(), pid
    guest.send(b"ps\r")
    assert pid not in [row.split()[0] for row in guest.until(prompt).splitlines() if row.strip()]
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    harness["test"](exercise, mode="--window")
    print("Verified legacy windows, adaptive resize, timers, partial redraw and stacking.")
