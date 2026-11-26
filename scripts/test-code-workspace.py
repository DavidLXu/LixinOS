"""Exercise Code's project, IME, concurrent edits and native build through QEMU."""
import argparse
import io
import os
from pathlib import Path
import re
import runpy
import socket
import subprocess
import tempfile
import time
from unittest.mock import patch
from PIL import Image

MUTTER = runpy.run_path(str(Path(__file__).with_name("test-mutter.py")))


def exercise(guest, control, directory):
    delay = 2.5 if os.environ.get("CODE_TEST_KERNEL") == "tcc" else 1
    def pause(seconds):
        time.sleep(seconds*delay)
    qmp = MUTTER["qmp_connection"](control)
    qmp("qmp_capabilities")
    guest.until(b"$ ")
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text("1920\t1200\n")
    project = "/tmp/code-suite"
    def show(cmd):
        guest.send(cmd.encode() + b"\r")
        return guest.until(b"$ ", timeout=180)
    def command(cmd, expected=b""):
        MUTTER["GUEST"]["command"](guest, cmd, expected=expected, prompt=b"$ ")
    command("mkdir " + project)
    command("cd " + project)
    command("echo 'int main(void){return 0;}' > main.c")
    command("echo draft > notes.txt")
    command("echo marker > sample.txt")
    command("echo changed > replacement")
    command("clip paste --mac > long.txt")
    command("clip copy < long.txt")
    command("clip paste > other.txt")
    command("cmp long.txt other.txt")
    command("clip copy --mac < other.txt")
    assert Path(os.environ["CLIPBOARD_TEST_FILE"]).read_bytes() == PAYLOAD
    print("Mac bridge and guest clipboard transferred", len(PAYLOAD), "bytes", flush=True)
    frame = directory / "code.ppm"
    def key(name):
        qmp("human-monitor-command", **{"command-line": f"sendkey {name} 35"})
        pause(.18)
    def text(value):
        for letter in value: key({'.':'dot',' ':'spc'}.get(letter,letter))
    def image():
        qmp("screendump",filename=str(frame))
        with Image.open(frame) as f: return f.copy()
    def until_window():
        guest.until(b"LixinOS compositor renderer:",timeout=180)
        deadline = time.monotonic()+150
        while time.monotonic()<deadline:
            screen=image()
            row = screen.crop((0, screen.height//2, screen.width, screen.height//2+1))
            has_gutter = (35,38,44) in row.getdata()
            if has_gutter and any(screen.getpixel((5,y)) == (36,56,80) for y in range(screen.height//2,screen.height)):
                pause(.75)
                return
            pause(.3)
        raise AssertionError("Code window missing")
    def capture(label):
        if os.environ.get("CODE_TEST_FRAMES"):
            image().save(Path(os.environ["CODE_TEST_FRAMES"]) / (label + ".png"))
    def wait_screen(pattern, timeout=40):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            png = io.BytesIO()
            image().save(png,format="PNG")
            words = subprocess.check_output(["tesseract","stdin","stdout","--psm","11"],
                        input=png.getvalue(),stderr=subprocess.DEVNULL).decode(errors="replace")
            if re.search(pattern,words,re.I): return
            time.sleep(.3)
        raise AssertionError((pattern,words[-500:]))
    def click_editor():
        w,h=image().size
        for down in (True,False):
            qmp("input-send-event",events=[
                {"type":"abs","data":{"axis":"x","value":round(w*.65*32767/(w-1))}},
                {"type":"abs","data":{"axis":"y","value":round(h*.45*32767/(h-1))}},
                {"type":"btn","data":{"button":"left","down":down}}])
            pause(.15)
    def quick(name, search="ctrl-p"):
        key(search)
        wait_screen(r"matches|searching|enter text to search|filter by")
        text(name)
        wait_screen(r"1\s+match(?:es)?\b")
        capture("find-"+name); key("ret"); pause(.6); capture("opened-"+name)
    def close():
        key("ctrl-w"); pause(.4)
    def shell():
        guest.until(b"$ ",timeout=180)
    guest.send(b"code .\r"); until_window()
    quick("notes")
    key("end"); key("f7"); text("nihao"); capture("pinyin"); key("1"); key("f7"); key("ctrl-s")
    pause(1); capture("saved-notes")
    close()
    quick("marker","f6")
    key("end"); text("x"); key("ctrl-s"); pause(1); close()
    quick("main.c")
    key("f5"); wait_screen(r"Built",timeout=60); capture("build")
    close(); close(); shell()
    command("cat notes.txt","draft你好".encode())
    command("cat sample.txt",b"markerx")
    command("./main.lxe")
    print("Project tree launch, tabs, quick open, pinyin, search navigation and F5 build passed",flush=True)

    command("echo different | clip copy")
    guest.send(b"code long.txt\r"); until_window()
    key("ctrl-a"); pause(.4); capture("selected-long"); key("ctrl-c"); pause(1); capture("long-copy"); close(); shell()
    command("clip paste > from-code")
    diff = show("cmp long.txt from-code")
    if b"differ" in diff or b"[exit" in diff:
        raise AssertionError((diff, show("wc -c long.txt from-code"), show("wc -l from-code")))

    print("Code copied a long UTF-8 selection into the shared guest clipboard",flush=True)

    guest.send(b"code from-mac.txt\r"); until_window()
    click_editor(); key("f9"); wait_screen(r"421"); capture("mac-paste")
    key("ctrl-s"); wait_screen(r"saved");
    Path(os.environ["CLIPBOARD_TEST_FILE"]).write_bytes(b"reset")
    key("ctrl-a"); key("f8"); pause(1); capture("mac-copy")
    deadline = time.monotonic()+30
    while time.monotonic()<deadline and Path(os.environ["CLIPBOARD_TEST_FILE"]).read_bytes() != PAYLOAD:
        time.sleep(.2)
    host = Path(os.environ["CLIPBOARD_TEST_FILE"]).read_bytes()
    assert host == PAYLOAD, ("Code GUI copy to Mac",len(host),host[:32])
    close(); shell()
    command("cmp from-mac.txt long.txt")
    print("Code GUI copied and pasted 9,660 bytes via the Mac actions",flush=True)

    def start(name):
        guest.send(("code " + name + "\r").encode()); until_window()
    def save():
        key("ctrl-s"); pause(1.2)

    command("echo 'if ok:' > indent.py")
    start("indent.py")
    # The Python keyword has the highlighter's foreground and the gutter
    # has a separate background. Capture before the cursor moves.
    colors = image().convert("RGB").getcolors(1000000)
    assert colors and all(any(pixel == wanted for _,pixel in colors)
                          for wanted in [(35,38,44),(166,202,255)])
    key("end"); key("ret"); text("pass"); save(); close(); shell()
    command("cat indent.py", b"if ok:\n    pass")
    start("indent.py"); key("ctrl-h"); pause(.5); text("pass")
    key("tab"); text("done")
    for _ in range(3): key("tab")
    key("spc"); pause(.3); key("esc"); save(); close(); shell()
    command("cat indent.py", b"if ok:\n    done")
    print("Gutter, syntax color, auto-indent and Replace All passed",flush=True)

    command("echo 'int main(void) {' > bad.c")
    command("echo '  int n = 0;' >> bad.c")
    command("echo '  return missing;' >> bad.c")
    command("echo '}' >> bad.c")
    command("cp main.lxe bad.lxe")
    start("bad.c"); key("f5"); wait_screen(r"Build failed",timeout=60); capture("diagnostic")
    key("ret"); pause(.5); key("home"); key("slash"); key("slash")
    save(); close(); shell()
    command("cat bad.c", b"//  return missing;")
    command("cmp main.lxe bad.lxe")
    print("TCC failure navigated to its source line and preserved the previous build",flush=True)

    with socket.socket() as listener:
        listener.bind(("127.0.0.1",0)); listener.listen(); listener.settimeout(30)
        command("echo '#include <lixin.h>' > writer.c")
        command("echo 'int main(void){char c; char *args[]={\"mv\",\"aaaa-replacement\",\"watched.txt\"};' >> writer.c")
        command(f"echo 'int fd=lx_tcp_connect(\"10.0.2.2\",{listener.getsockname()[1]});' >> writer.c")
        command("echo 'if(fd<0 || lx_read(fd,&c,1)!=1)return 1;' >> writer.c")
        command("echo 'c=lx_spawn(\"/bin/mv\",3,args); lx_write(fd,&c,1); return c;}' >> writer.c")
        command("tcc writer.c -o writer")
        for dirty in (False, True):
            command("echo original > watched.txt")
            command("echo replaced > aaaa-replacement")
            command("./writer &")
            with listener.accept()[0] as worker:
                worker.settimeout(45)
                start("."); quick("watched")
                tree_before = image().crop((0,155,280,300)).tobytes()
                if dirty: key("end"); key("x")
                worker.sendall(b"w"); assert worker.recv(1)==bytes(1), "guest rename failed"
                pause(3.5)
                capture("watch-dirty" if dirty else "watch-clean")
                if not dirty:
                    assert image().crop((0,155,280,300)).tobytes() != tree_before, "project tree did not refresh"
                if dirty: key("ret"); pause(.5)
                key("ctrl-a"); key("ctrl-c"); pause(1)
                if dirty: key("ctrl-z")
                close(); close(); shell()
            command("cat watched.txt", b"replaced")
            command("clip paste", b"originalx" if dirty else b"replaced")
    print("Rename-replace reloaded clean text and preserved dirty text",flush=True)


PAYLOAD = ("你好，LixinOS\t12345\n"*420).encode()
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk",required=True,type=Path)
    parser.add_argument("--kernel",choices=["rust","tcc"],default="rust")
    parser.add_argument("--release",default="latest")
    args=parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        directory=Path(tmp); disk=directory/"disk.img"
        subprocess.run(["cp","-c",args.disk,disk],check=True)
        clip=directory/"host-clipboard"; clip.write_bytes(PAYLOAD)
        for name, cmd in [("pbpaste",'cat "$CLIPBOARD_TEST_FILE"'),("pbcopy",'cat > "$CLIPBOARD_TEST_FILE"')]:
            stub=directory/name;stub.write_text("#!/bin/sh\n"+cmd+"\n");stub.chmod(0o700)
        with patch.dict(os.environ, {"CODE_TEST_KERNEL":args.kernel,"CLIPBOARD_TEST_FILE":str(clip),
                                     "PATH":str(directory)+":"+os.environ["PATH"]}):
            os.environ.pop("LIXINOS_MAC_CLIPBOARD", None)
            MUTTER["with_window"](disk,args.release,args.kernel,exercise)
