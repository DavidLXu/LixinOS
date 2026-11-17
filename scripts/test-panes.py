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

    def left_data():
        path = directory / "data.bin"
        # Observe the left pane's writable 2 MiB block, including C globals.
        command("pmemsave", val=0x44200000, size=2 << 20, filename=str(path))
        return path.read_bytes()

    assert "QMP" in json.loads(control.readline())
    command("qmp_capabilities")
    prompt = serial["SHELL"]
    repl = serial["PYTHON"]
    guest.until(prompt)  # The startup handshake paints both shell prompts.
    guest.expect(b"python\r", repl)
    guest.expect(b"x = 42\r", repl)
    switch()
    guest.expect(b"python\r", repl)
    guest.expect(b"x = 7\r", repl)
    switch()
    guest.expect(b"x\r", repl, b"\n42\n")
    guest.expect(b"\x04", prompt)
    guest.expect(b"""echo '#include <stdio.h>' > spin.c\r""", prompt)
    guest.expect(b"""echo 'volatile unsigned n; int main(void){puts("spinning");for(;;)++n;}' >> spin.c\r""", prompt)
    guest.expect(b"tcc -run spin.c\r", b"spinning\r\n")
    switch()
    guest.expect(b"x\r", repl, b"\n7\n")
    paused = left_data()
    time.sleep(0.15)
    assert paused == left_data(), "C loop advanced in the inactive pane"
    switch()
    time.sleep(0.15)
    switch()
    guest.expect(b"x * 2\r", repl, b"\n14\n")
    assert paused != left_data(), "C loop did not resume"
    guest.expect(b"\x04", prompt)
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=5) == 0


def test():
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
                  f"PANE_TEST_SOCKET={endpoint}", here / "run-kernel.sh", "--split"]
        with serial["boot"](directory / "disk.img", launch) as guest, socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(10)
            sock.connect(str(endpoint))
            with sock.makefile("rwb") as control:
                exercise(guest, control, directory)


if __name__ == "__main__":
    test()
    print("Passed split-pane Python isolation and native C pause/resume.")
