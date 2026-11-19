"""Exercise Ctrl+Tab across two live EL0 contexts on a disposable guest disk."""
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import tempfile
import time

here = Path(__file__).resolve().parent
serial = runpy.run_path(str(here / "test-user.py"))


def exercise(guest, control, directory):
    def command(name, **arguments):
        control.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        control.flush()
        while True:
            reply = json.loads(control.readline())
            if "event" not in reply:
                assert "return" in reply, reply
                return reply["return"]

    def switch():
        command("human-monitor-command", **{"command-line": "sendkey ctrl-tab 20"})
        time.sleep(0.12)

    assert "QMP" in json.loads(control.readline())
    command("qmp_capabilities")
    prompt = serial["SHELL"]
    repl = serial["PYTHON"]
    guest.until(prompt)  # The startup handshake paints both shell prompts.
    guest.expect(b"echo 'int x = 42; // note' > color.c\r", prompt)
    guest.expect(b"kilo color.c\r", b"Ctrl-F = find")
    screenshot = directory / "kilo.ppm"
    command("screendump", filename=str(screenshot))
    pixels = screenshot.read_bytes().split(b"\n", 3)[-1]
    assert bytes.fromhex("e69a96") in pixels, "numeric literal lacks its native red foreground"
    switch()
    guest.expect(b"echo newer > color.c\r", prompt)
    switch()
    guest.expect(b"X\x13", b"file changed in other pane")
    guest.expect(b"\x11\x11\x11\x11", prompt)
    guest.expect(b"cat color.c\r", prompt, b"\nnewer\n")
    guest.expect(b"python\r", repl)
    guest.expect(b"x = 42\r", repl)
    switch()
    guest.expect(b"python\r", repl)
    guest.expect(b"x = 7\r", repl)
    switch()
    guest.expect(b"x\r", repl, b"\n42\n")
    guest.expect(b"\x04", prompt)
    guest.expect(b"""echo '#include <stdio.h>' > spin.c\r""", prompt)
    guest.expect(b"echo '#include <lixin.h>' >> spin.c\r", prompt)
    guest.expect(b"echo 'int main(void){puts(\"spinning\");for(;;){' >> spin.c\r", prompt)
    guest.expect(b"echo 'for(volatile int i=0;i<5000000;i++){}' >> spin.c\r", prompt)
    guest.expect(b"echo 'if(lx_stat(\"signal\")>=0)return lx_save_file(\"done\",\"ok\",2);}}' >> spin.c\r", prompt)
    guest.expect(b"tcc spin.c -o spin\r",prompt)
    guest.expect(b"./spin\r", b"spinning\r\n")
    switch()
    guest.expect(b"x * 2\r", repl, b"\n14\n")
    guest.expect(b"\x04", prompt)
    guest.expect(b"touch signal\r", prompt)
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        guest.send(b"cat done\r")
        if b"\nok" in guest.until(prompt): break
    else: raise AssertionError("unfocused process did not finish")
    # A network job outlives its launching shell. Keep the right-hand editor
    # open while its peer compiles on the left, then close the left terminal.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1",0))
        listener.listen()
        listener.settimeout(10)
        switch()
        guest.send(b"echo left-ready\r")
        guest.until(b"\nleft-ready\r\n")
        guest.until(prompt)
        for source in (
            "#include <lixin.h>",
            f'int main(void){{char c;int fd=lx_tcp_connect("10.0.2.2",{listener.getsockname()[1]});',
            'if(fd<0 || lx_read(fd,&c,1)!=1)return 1;',
            'return lx_save_file("socket-result",&c,1);}',
        ):
            guest.expect(f"echo '{source}' >> worker.c\r".encode(),prompt)
        guest.expect(b"tcc worker.c -o worker\r",prompt)
        guest.expect(b"./worker &\r",prompt,b"[pid ")
        with listener.accept()[0] as conn:
            switch()
            guest.expect(b"kilo draft.c\r",b"Ctrl-F = find")
            guest.send(b"working")
            switch()
            guest.send(b"tcc worker.c -o built\r")
            guest.until(prompt)
            guest.expect(b"ls -1 built\r",prompt,b"\nbuilt\n")
            guest.send(b"exit\r")
            time.sleep(.12)
            switch()
            guest.send(b" draft\x13")
            guest.until(b"bytes written on disk")
            conn.sendall(b"K")
            guest.expect(b"\x11",prompt)
            guest.expect(b"cat draft.c\r",prompt,b"working draft")
            guest.expect(b"cat socket-result\r",prompt,b"\nK")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=5) == 0


def test(exercise=exercise, mode="--split", disk=None):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        directory = Path(tmp)
        if not (qemu := shutil.which("qemu-system-aarch64")):
            raise RuntimeError("QEMU is required")
        # Use the normal split launcher, replacing only the display and UART
        # endpoints so the test can send input and observe guest output.
        wrapper = directory / "qemu-system-aarch64"
        wrapper.write_text("""#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
args[args.index('-display') + 1] = 'none'
args[args.index('-serial') + 1] = 'stdio'
os.execv(os.environ['PANE_TEST_QEMU'], [os.environ['PANE_TEST_QEMU'], *args,
         '-qmp', 'unix:' + os.environ['PANE_TEST_SOCKET'] + ',server=on,wait=off'])
""")
        wrapper.chmod(0o700)
        endpoint = directory / "qmp"
        launch = ["env", f"PATH={directory}:{os.environ['PATH']}", f"PANE_TEST_QEMU={qemu}",
                  f"PANE_TEST_SOCKET={endpoint}", here / "run-kernel.sh", mode]
        with serial["boot"](disk if disk is not None else directory / "disk.img", launch) as guest, socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10)
            sock.connect(str(endpoint))
            with sock.makefile("rwb") as control:
                exercise(guest, control, directory)


if __name__ == "__main__":
    test()
    print("Passed native Kilo colors, cross-pane save conflict, Python isolation and independent CPU progress.")
