"""Boot a disposable disk and exercise both independent RA2 ports."""
import argparse
import json
import shlex
from pathlib import Path
import runpy
import subprocess
import tempfile
import time
from PIL import Image

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
panes=runpy.run_path(str(HERE/"test-panes.py"))
shutdown=runpy.run_path(str(HERE/"test-native-kernel.py"))["shutdown"]

def exercise(guest, control, tmp, screenshot, retail=False, recomp_skirmish=False):
    def run(command, expected, timeout=60):
        guest.send(command.encode()+b"\r")
        output=guest.until(b"$ ",timeout=timeout)
        assert all(s.encode() in output for s in expected),output
        assert b"[exit " not in output or any("[exit " in s for s in expected),output

    run("ra2-engine --version",["Red Alert 2 engine 0.9.0"])
    run("ra2-recomp --help",["usage: ra2","--game DIR"])
    run("ra2-recomp-check",["Win32 sparse addresses verified","bound 2 imports, 0 unresolved","pe_loader_selftest: all checks passed"])
    run("ra2-recomp-check draw",["\nok\n"])
    run("ra2-recomp-check host",["lifted functions in dispatch: 1","entering 0x00401000","entry returned eax=0000002A","[exit 42]"])
    if retail:
        run("ra2-recomp --game /lib/ra2-content", ["mapped /lib/ra2-content/"])
        steps = ['require scene == "shell:Main Menu"', 'click button "Single Player"',
                 'click button Skirmish', 'require scene == lobby', 'click start',
                 'until 90 scene == game', 'require scene == game', 'wait 4', 'click unit 0:AMCV',
                 'key d', 'until 450 built(0,GACNST) == 1', 'require built(0,GACNST) == 1']
        script = shlex.quote("\\n".join(steps) + "\\n")
        run(f"printf {script} > /tmp/retail.play", [])
        run("ra2-engine --play /tmp/retail.play --out /tmp/ra2-play",
            ["play_result=PASS", "failures=0 unreached=0"], timeout=360)
    assert "QMP" in json.loads(control.readline())
    def qmp(name,**args):
        control.write(json.dumps({"execute":name,"arguments":args}).encode()+b"\n");control.flush()
        while True:
            reply=json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply,reply
                return
    qmp("qmp_capabilities")
    guest.send(b"ra2-engine --data /tmp/ra2-missing\r")
    guest.until(b"4 required file(s) missing",timeout=45)
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        path=tmp/"frame.ppm"
        qmp("screendump",filename=str(path))
        _,dims,_,pixels=path.read_bytes().split(b"\n",3)
        w,h=map(int,dims.split())
        frame=Image.frombytes("RGB",(w,h),pixels,"raw","RGB",len(pixels)//h)
        # Upstream renders three red buttons. The rightmost one exits the menu.
        buttons=[(x,y) for y in range(h//2,h,8) for x in range(0,w,8) if frame.getpixel((x,y))==(70,30,26)]
        if buttons and any(c==(255,200,90) and n>100 for n,c in frame.getcolors(w*h)):
            break
        time.sleep(.2)
    else:
        raise AssertionError("Upstream GAME FILES NEEDED screen did not appear")
    if screenshot:
        frame.save(screenshot)
    x=max(x for x,y in buttons)-18
    y=(min(y for x,y in buttons)+max(y for x,y in buttons))//2
    qmp("input-send-event",events=[{"type":"abs","data":{"axis":axis,"value":round(v*32767/(extent-1))}}
                                   for axis,v,extent in (("x",x,w),("y",y,h))])
    for down in (True,False):
        qmp("input-send-event",events=[{"type":"btn","data":{"button":"left","down":down}}])
        time.sleep(.15)
    guest.until(b"$ ",timeout=15)
    if recomp_skirmish:
        play_recompiled_skirmish(guest, qmp, recomp_skirmish)
    else:
        shutdown(guest)

def play_recompiled_skirmish(guest, qmp, edition="ra2"):
    command, exe = ("yr-recomp", "gamemd.exe") if edition == "yr" else ("ra2-recomp", "game.exe")
    guest.send(f"{command} --game /lib/ra2-content --run --debuglog\r".encode())
    guest.until(f"mapped /lib/ra2-content/{exe}:".encode(), timeout=90)
    # These are the game window's buttons in the headless Retina test display.
    x, rows = (.808, (.386, .523, .455)) if edition == "yr" else (.672, (.446, .520, .482))
    for dialog, y in zip((b"0xE2", b"0x100", b"0x102"), rows):
        guest.until(b"[dialog] open " + dialog, timeout=180)
        time.sleep(5)  # Wait for the button panel's opening animation.
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(value * 32767)}}
            for axis, value in (("x", x), ("y", y))])
        time.sleep(.4)
        for down in (True, False):
            qmp("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}])
            time.sleep(.4)
    guest.until(b"[game] Capture_Mouse()", timeout=240)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        guest.until(b"game frame ", timeout=max(1, deadline - time.monotonic()))
        if int(guest.until(b"\n")) >= 100:
            return
    raise AssertionError("Retail skirmish simulation did not advance to frame 100")

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk",type=Path,required=True)
    parser.add_argument("--kernel",choices=("rust","tcc"),required=True)
    parser.add_argument("--release",default="latest")
    parser.add_argument("--screenshot",type=Path)
    parser.add_argument("--retail",type=Path,help="import this local RA2 installation and play a skirmish")
    parser.add_argument("--recomp-skirmish", nargs="?", const="ra2", choices=("ra2","yr"),
                        help="click into a battle in Red Alert 2 or Yuri's Revenge")
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ra2-ports-") as tmp:
        disk=Path(tmp)/"test.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        port=runpy.run_path(str(ROOT/"User/games/ra2-native/install.py"))
        port["install"](disk,args.retail)
        panes["test"](lambda g,c,d:exercise(g,c,d,args.screenshot,args.retail is not None,args.recomp_skirmish),mode="--window",disk=disk,
                      launcher_args=("--kernel",args.kernel,"--release",args.release),prompt=b"$ ")
    print(f"{args.kernel}: RA2 Engine menu and mouse exit, Win32 PE, DirectDraw and translated entry verified")
    if args.retail:
        print(f"{args.kernel}: retail PE mapped and Allied construction yard deployed in skirmish")
    if args.recomp_skirmish:
        print(f"{args.kernel}: {args.recomp_skirmish} loaded a retail map and advanced past game frame 100")

if __name__=="__main__":
    main()
