"""Exercise the Vanguard simulation and the LixinOS graphical input path in QEMU."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PANES = runpy.run_path(str(HERE / "test-panes.py"))
shutdown = runpy.run_path(str(HERE / "test-native-kernel.py"))["shutdown"]


def exercise(guest, control, directory, screenshot):
    guest.send(b"ra2 --check\r")
    result = guest.until(b"$ ", timeout=120)
    assert b"RA2 verified movement, both construction yards, power plant, barracks and infantry combat" in result, result
    assert "QMP" in json.loads(control.readline())

    def qmp(command, **args):
        control.write(json.dumps({"execute": command, "arguments": args}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return

    def capture():
        path = directory / "ra2.ppm"
        qmp("screendump", filename=str(path))
        _, dimensions, _, raw = path.read_bytes().split(b"\n", 3)
        w, h = map(int, dimensions.split())
        return Image.frombytes("RGB", (w,h), raw, "raw", "RGB", len(raw)//h)

    qmp("qmp_capabilities")
    guest.send(b"ra2\r")
    guest.until(b"RA2 skirmish ready", timeout=45)
    before = capture()
    qmp("human-monitor-command", **{"command-line":"sendkey d 60"})
    deadline = time.monotonic() + 12
    # The green ownership marker stays at the foot of the rendered entity.
    # Compare the actual playfield around it to verify the deployed building.
    while time.monotonic() < deadline:
        after = capture()
        green = after.convert("RGB")
        points = [(x,y) for y in range(0,green.height,2) for x in range(0,green.width,4)
                  if green.getpixel((x,y)) == (0x57,0xbb,0x83)]
        if points:
            x,y = points[-1]
            box = (x-40,y-70,x+40,y+4)
            changed = sum(a != b for a,b in zip(before.crop(box).getdata(),after.crop(box).getdata()))
            if changed > 180:
                break
        time.sleep(.2)
    else:
        raise AssertionError("QEMU input did not change the MCV into a construction yard")
    # Locate the first production button in the guest framebuffer.
    x = after.width-25
    top = next(y for y in range(after.height) if after.getpixel((x,y)) == (0x35,0x3e,0x40))
    y = top+12
    qmp("input-send-event", events=[
        {"type":"abs","data":{"axis":axis,"value":round(value*32767/limit)}}
        for axis,value,limit in (("x",x,after.width-1),("y",y,after.height-1))])
    for down in (True,False):
        qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":down}}])
        time.sleep(.25)
    deadline = time.monotonic()+8
    while time.monotonic()<deadline:
        after=capture()
        if after.getpixel((x-1,top+2)) in ((0x71,0x58,0x3b),(0x42,0x76,0x5b)):
            break
        time.sleep(.2)
    else:
        raise AssertionError("Clicking the production sidebar did not queue a power plant")
    if screenshot:
        after.save(screenshot)
    qmp("human-monitor-command", **{"command-line":"sendkey q 60"})
    guest.until(b"$ ",timeout=15)
    shutdown(guest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--kernel", choices=("rust","tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ra2-") as tmp:
        disk = Path(tmp) / "test.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        subprocess.run(["python3",ROOT/"User/gcc/install.py","--toolchain","ra2",
                        ROOT/"Kernel/target/user-build/ra2-port/package",disk],check=True)
        PANES["test"](lambda g,c,d: exercise(g,c,d,args.screenshot),mode="--window",
                      disk=disk, launcher_args=("--kernel",args.kernel,"--release",args.release),prompt=b"$ ")
    print(f"{args.kernel}: skirmish verified, keyboard deployment, mouse production and quit verified.")


if __name__ == "__main__":
    main()
