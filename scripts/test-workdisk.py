"""Exercise daily-disk recovery, job control, and login configuration on a disposable copy."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import time

here = Path(__file__).resolve().parent
guest = runpy.run_path(str(here / "test-user.py"))
home = b"lixin:/home/lixin$ "


def check(seed, release):
    with tempfile.TemporaryDirectory() as temp:
        env = dict(os.environ, LIXINOS_WORKDIR=temp)

        def workdisk(*args):
            return subprocess.check_output([sys.executable, here / "workdisk.py", *args],
                                           text=True, env=env).strip()

        image = Path(workdisk("init", seed))
        def boot(kernel):
            return guest["boot"](image, [here / "run-kernel.sh", "--serial", "--kernel", kernel,
                                         "--release", release], prompt=home)

        def shutdown(s):
            s.send(b"shutdown\r")
            assert s.vm.wait(timeout=20) == 0

        def interrupt(s, cmd):
            s.send(cmd + b"\r")
            time.sleep(.25)
            s.expect(b"\x03", home)
            s.expect(b"echo $?\r", home, b"\n130\n")

        with boot("rust") as s:
            s.expect(b"echo 'print(3*17)' > recover-me.py\r", home)
            s.expect(b"echo 'int main(void){for(;;){}return 0;}' > spinner.c\r", home)
            s.expect(b"tcc spinner.c -o bin/spinner\r", home)
            s.expect(b"echo 'export LX_PROFILE=confirmed' >> .profile\r", home)
            shutdown(s)
        backup = workdisk("backup")
        for kernel in ("tcc", "rust"):
            with boot(kernel) as s:
                s.expect(b"echo $HOME $LX_PROFILE\r", home, b"\n/home/lixin confirmed\n")
                s.expect(b"python3 recover-me.py\r", home, b"\n51\n")
                s.expect(b"export SESSION_MARK=1234\r", home)
                s.expect(b"bash -c 'echo $SESSION_MARK' | cat\r", home, b"\n1234\n")
                s.expect(b"python3 -c 'import os; print(os.environ.get(\"SESSION_MARK\"))'\r",
                         home, b"\n1234\n")
                s.expect(b"sleep 120 &\r", home, b"[pid ")
                for job in (b"spinner", b"sleep 40", b"spinner | cat", b"cat"):
                    interrupt(s, job)
                s.send(b"ps\r")
                assert b"background" in s.until(home)
                s.expect(b"echo discard-me\x03", home, b"^C")
                s.expect(b"cd bin\r", b"lixin:/home/lixin/bin$ ")
                s.expect(b"cd\r", home)
                if kernel == "tcc":
                    s.expect(b"rm recover-me.py\r", home)
                    s.expect(b"echo retained > later.txt\r", home)
                else:
                    s.expect(b"cat later.txt\r", home, b"\nretained\n")
                shutdown(s)
            if kernel == "tcc":
                workdisk("restore", backup, "/home/lixin/recover-me.py")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seed", type=Path, help="idle installed disk to clone")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    check(args.seed.resolve(), args.release)
    print("Passed single-file recovery, interruption, and persistent login settings on both kernels.")
