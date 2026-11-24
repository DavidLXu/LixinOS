"""Open the guest-built Agent window and exercise its live workbench through QMP."""
from launcher_grid import select_icon
import argparse
import gzip
import json
import os
from pathlib import Path, PurePosixPath
import runpy
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

from disk_image import snapshot_headers

HERE = Path(__file__).resolve().parent
SERIAL = runpy.run_path(str(HERE / "test-user.py"))
BRIDGE = runpy.run_path(str(HERE / "agent-bridge.py"))
DECODE = runpy.run_path(str(HERE / "export-git.py"))["decode"]
PROJECT = "/work/panel-calculator"
SHELL = SERIAL["SHELL"]
TASK = "Make R clear the calculator and show the shortcut"
PRESS = "    press(e.value);"
DRAW = "    app_text(s, 4, 16, input, 0xffff);"
BADGE = '    app_rect(s, 2, 2, 60, 10, app_rgb(245, 107, 102));\n    app_text(s, 4, 0, "R", 0xffff);'
CALCULATOR = """#include "/src/desktop/window-app.h"
#include "/src/gui/calculator-model.h"

static int event(AppWindow *s, WindowEvent e) {
    if (e.type != WINDOW_CLIENT_KEY) return 0;
""" + PRESS + """
    return 1;
}

static void draw(AppWindow *s) {
    app_rect(s, 0, 0, s->width, s->height,
             value() == 0 ? app_rgb(34, 59, 67) : app_rgb(40, 68, 86));
""" + DRAW + """
}

int main(void) { return app_run(event, draw); }
"""


def clear_test(name, key):
    return json.dumps([{"name": name, "steps": [
        {"event": ["resize", 64, 42], "pixels": [[63, 41, 8648]]},
        {"event": ["key", ord("7")], "pixels": [[63, 41, 10794]]},
        {"event": ["key", ord(key)], "pixels": [[63, 41, 8648]]},
    ]}])


def reply(instructions, data):
    assert data["workspace"] == PROJECT and data["task"] == TASK, data
    assert "workbench tool" in instructions
    actions = [
        ("write_file", ["tests", clear_test("R clears", "R")]),
        ("check_app", []),
        ("apply_patch", ["source", PRESS, "    press(e.value == 'R' ? 'c' : e.value);"]),
        ("apply_patch", ["source", DRAW, BADGE + "\n" + DRAW]),
        ("check_app", []),
        ("finish", ["The calculator keeps AC and now supports R."]),
    ]
    observations = data["observations"]
    if len(observations) == 2:
        assert observations[-1]["status"] == "failed" and "R clears step 3" in observations[-1]["detail"], observations
    if len(observations) == 5:
        assert "CHECK PASSED 2 cases" in observations[-1]["detail"], observations
    tool, args = actions[len(observations)]
    return {"tool": tool, "args": args}


def files(disk):
    with disk.open("rb") as stream:
        _, start, count, _, header = snapshot_headers(stream)[0]
        stream.seek(start + 512)
        return DECODE(stream.read(count), header[:8])


def wait_for(check, label, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = check()
            if value:
                return value
        except (OSError, ValueError, IndexError):
            pass  # An observation can overlap guest publication of a disk snapshot.
        time.sleep(.08)
    raise AssertionError(f"Timed out waiting for {label}")


def exercise(guest, control, folder, disk):
    def qmp(name, **args):
        control.write(json.dumps(dict(execute=name, arguments=args)).encode() + b"\n")
        control.flush()
        while True:
            answer = json.loads(control.readline())
            if "event" not in answer:
                assert "return" in answer, answer
                return

    def picture():
        path = folder / "desktop.ppm"
        qmp("screendump", filename=str(path))
        if os.environ.get("LIXINOS_UI_SNAPSHOT"):
            shutil.copyfile(path, os.environ["LIXINOS_UI_SNAPSHOT"])
        _, size, _, pixels = path.read_bytes().split(b"\n", 3)
        w, h = map(int, size.split())
        return w, h, pixels

    def pixel(x,y):
        w,h,p = picture()
        offset = (44+2*y)*(len(p)//h) + 6*x
        return p[offset:offset+3].hex()

    def click(x,y):
        qmp("input-send-event", events=[
            {"type":"abs","data":{"axis": axis,"value": round(pos*32767/(size-1))}}
            for axis,pos,size in (("x",2*x+1,2354),("y",44+2*y+1,1408))])
        time.sleep(.14)
        qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":True}}])
        time.sleep(.09)
        qmp("input-send-event", events=[{"type":"btn","data":{"button":"left","down":False}}])
        time.sleep(.09)

    def command(text, prompt, marker=b""):
        guest.send(text.encode() + b"\r")
        output = guest.until(prompt, timeout=35)
        assert marker in output, output

    command(f"agent desktop new {PROJECT}", SHELL, b"Created")
    prompt = f"lixin:{PROJECT}$ ".encode()
    command(f"cd {PROJECT}", prompt)
    for name, content in (("app.c", CALCULATOR), ("tests.json", clear_test("AC clears", "c"))):
        for index, line in enumerate(content.splitlines()):
            redirect = ">>" if index else ">"
            command(f"printf '%s\\n' {shlex.quote(line)} {redirect} {name}", prompt)
    command("grep calculator-model.h app.c", prompt, b'#include "/src/gui/calculator-model.h"')
    command("python3 -I /src/agent/desktop.py check", prompt, b"CHECK PASSED 1")
    command("python3 -I /src/agent/desktop.py publish", prompt, b"ACTIVE")
    installed = PurePosixPath("/bin/panel-calculator.win2")
    baseline = files(disk)[installed]
    assert "QMP" in json.loads(control.readline())
    qmp("qmp_capabilities")
    guest.send(b"desktop\r")
    wait_for(lambda: pixel(2,2) == "281e2b", "desktop dock")
    click(36,8+7*min(82,(682-48)//8)+19)
    wait_for(lambda: pixel(82,6) == "203f51", "Apps menu")
    select_icon(click,qmp,1177,682,6,7)
    time.sleep(.45)
    picture()
    app = PurePosixPath("/bin/agent.win3")
    assert wait_for(lambda: files(disk).get(app), "TCC package")[:4] == b"LXE1"
    left, top = 76 + (1177-76-720)//2, (682-480)//2 + 44
    wait_for(lambda: pixel(left+5,top+7) == "102030", "native Agent title")
    guest.send(b"\x7f"*5 + PROJECT.encode() + b"\r")
    time.sleep(.9)
    receipt = PurePosixPath(PROJECT+"/.desktop/candidate")
    old = files(disk)[receipt]
    guest.send(TASK.encode()+b"\r")
    try:
        wait_for(lambda: files(disk).get(receipt) not in (None, old), "GUI Agent candidate", 120)
        wait_for(lambda: pixel(left+507,top+41) == "204c50", "enabled Preview button", 45)
    except AssertionError as error:
        journals = [data.decode(errors="replace")[-6000:] for path, data in files(disk).items()
                    if str(path.parent) == PROJECT and path.name.startswith(".agent-trace-")]
        picture()
        raise AssertionError(f"{error}. Guest journals: {journals}") from error
    assert files(disk)[installed] == baseline
    click(left+540,top+49)
    wait_for(lambda: picture()[2].count(bytes.fromhex("f06860")) > 1000, "candidate preview")
    click(left+540,top+49)
    content_height = 480-44
    click(left+260+208,top+content_height-85)
    assert files(disk)[installed] == baseline
    click(left+260+208,top+content_height-85)
    wait_for(lambda: files(disk)[installed] != baseline, "explicit publication")
    wait_for(lambda: pixel(left+560,top+content_height-93) == "204c50", "enabled Rollback button", 45)
    changed = files(disk)[installed]
    click(left+580,top+content_height-85)
    assert files(disk)[installed] == changed
    click(left+580,top+content_height-85)
    wait_for(lambda: files(disk)[installed] == baseline, "explicit rollback")
    print("Verified the GUI calculator task, failed test and repair, TCC build, preview, publish, and rollback.")


def test(kernel, release):
    qemu = shutil.which("qemu-system-aarch64")
    with tempfile.TemporaryDirectory(prefix="lixin-agent-ui-",dir="/tmp") as temp:
        folder = Path(temp); disk = folder / "disk.img"; endpoint = folder / "qmp"
        if kernel == "tcc":
            ref = f"refs/tags/{release}:kernel-artifacts/tcc-system.img.gz"
            with subprocess.Popen(["git", "show", ref], cwd=HERE.parent, stdout=subprocess.PIPE) as packed:
                with gzip.GzipFile(fileobj=packed.stdout) as source, disk.open("wb") as out:
                    shutil.copyfileobj(source,out)
                assert packed.wait() == 0
        wrapper = folder / "qemu-system-aarch64"
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join(
            [sys.executable, str(Path(__file__).resolve()), "--fixture", qemu, str(endpoint)]) + ' "$@"\n')
        wrapper.chmod(0o700)
        launch = ["env", "LIXINOS_GPU=ramfb", f"PATH={folder}:{os.environ['PATH']}", HERE / "run-kernel.sh",
                  "--kernel", kernel, "--window", *(["--release", release] if release else ["--source"])]
        with SERIAL["boot"](disk, launch) as guest, socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10)
            sock.connect(str(endpoint))
            with sock.makefile("rwb") as control:
                exercise(guest, control, folder, disk)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
        qemu, endpoint, *args = sys.argv[2:]
        args[args.index("-display")+1] = "none"
        args[args.index("-serial")+1] = "stdio"
        sys.exit(BRIDGE["launch"]([qemu,*args,"-qmp",f"unix:{endpoint},server=on,wait=off"], reply))
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", choices=("rust","tcc"), default="rust")
    parser.add_argument("--release")
    args = parser.parse_args()
    if args.kernel == "tcc" and not args.release:
        parser.error("--kernel tcc requires --release TAG")
    test(args.kernel,args.release)
