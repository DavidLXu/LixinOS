"""End-to-end foreground C and MicroPython tests through the guest serial port."""
from contextlib import contextmanager
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SHELL = b"lixin:/$ "
PYTHON = b">>> "


class Serial:
    def __init__(self, vm):
        self.vm = vm
        self.buffer = b""

    def send(self, data):
        self.vm.stdin.write(data)
        self.vm.stdin.flush()

    def until(self, marker, timeout=10):
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            wait = max(0, deadline - time.monotonic())
            if not select.select([self.vm.stdout], [], [], wait)[0]:
                raise TimeoutError(self.buffer[-1024:])
            chunk = os.read(self.vm.stdout.fileno(), 4096)
            if not chunk:
                raise EOFError(self.buffer[-1024:])
            self.buffer += chunk
        data, _, self.buffer = self.buffer.partition(marker)
        return data.replace(b"\r", b"")

    def expect(self, command, prompt, output=b""):
        self.send(command)
        received = self.until(prompt)
        if output not in received:
            raise AssertionError(received)


@contextmanager
def boot(disk, command=None, prompt=SHELL):
    env = dict(os.environ, LIXINOS_DISK=str(disk), DEEPSEEK_API_KEY="")
    command = command or [ROOT / "scripts/run-kernel.sh", "--serial"]
    with subprocess.Popen(command, env=env,
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE) as vm:
        try:
            serial = Serial(vm)
            serial.until(prompt, timeout=600)
            yield serial
        finally:
            vm.terminate()
            vm.wait(timeout=5)


def check_ls(s):
    def listing(command):
        s.send(command + b"\r")
        echoed, body = s.until(SHELL).split(b"\n", 1)
        assert echoed == command, echoed
        return body

    def rows(command):
        plain = listing(command).replace(b"\x1b[36m", b"").replace(b"\x1b[39m", b"")
        return [line.split() for line in plain.splitlines()]

    s.expect(b"mkdir ls-test\r", SHELL)
    s.expect(b"mkdir ls-test/sub\r", SHELL)
    s.expect(b"touch ls-test/empty\r", SHELL)
    s.expect(b"echo abc > ls-test/.secret\r", SHELL)
    # Each append contributes 383 ASCII bytes plus echo's newline.
    for _ in range(4):
        s.expect(b"echo " + b"x" * 383 + b" >> ls-test/data\r", SHELL)
    default = listing(b"ls ls-test")
    assert default == listing(b"ls -lh ls-test")
    assert b"1.5 KiB  data\n" in default and b"\x1b[36msub\x1b[39m\n" in default
    assert listing(b"ls -1 ls-test") == b"data\nempty\n\x1b[36msub\x1b[39m\n"
    plain = default.replace(b"\x1b[36m", b"").replace(b"\x1b[39m", b"")
    assert listing(b"ls ls-test | cat") == plain
    assert listing(b"ls -1 ls-test | cat") == b"data\nempty\nsub\n"
    s.expect(b"ls ls-test > ls-output\r", SHELL)
    assert listing(b"cat ls-output") == plain
    assert rows(b"ls -l ls-test") == [
        [b"-", b"1536", b"data"], [b"-", b"0", b"empty"], [b"d", b"-", b"sub"]]
    human = [[b"-", b"1.5", b"KiB", b"data"], [b"-", b"0", b"B", b"empty"], [b"d", b"-", b"sub"]]
    assert rows(b"ls -lh ls-test") == human
    full = [[b"d", b"-", b"."], [b"d", b"-", b".."], [b"-", b"4", b"B", b".secret"]] + human
    assert rows(b"ls -a ls-test") == full
    assert rows(b"ls -lah ls-test") == full
    assert rows(b"ls -h -a -l ls-test") == full
    assert rows(b"ls -l ls-test/.secret") == [[b"-", b"4", b"ls-test/.secret"]]


def rebuild_ls(s):
    # Edit the installed source in the guest, then install the guest-built LXE1.
    s.expect(b"nano /src/ls/ls.c\r", b"^U Paste line")
    s.expect(b"#define main original_ls\r\x0f", b"Saved")
    s.expect(b"\x18", SHELL)
    s.expect(b"echo '#undef main' >> /src/ls/ls.c\r", SHELL)
    s.expect(b"""echo 'int main(int n,char **v){text("guest-built ls"); putchar(10); return original_ls(n,v);}' >> /src/ls/ls.c\r""", SHELL)
    s.expect(b"tcc /src/ls/ls.c -o /bin/ls\r", SHELL)
    s.expect(b"ls -lah /bin > listing.txt\r", SHELL)
    s.expect(b"cat listing.txt\r", SHELL, b"\nguest-built ls\n")
    s.expect(b"ls -lah ls-test | grep .secret\r", SHELL, b"4 B  .secret\n")

    s.expect(b"echo '#include <stdio.h>' > magic.c\r", SHELL)
    s.expect(b"""echo 'int main(int n,char **v){char h[5]={0}; FILE *f=fopen(v[1],"rb"); if(!f || fread(h,1,4,f)!=4)return 1; puts(h); return fclose(f);}' >> magic.c\r""", SHELL)
    s.expect(b"tcc magic.c -o magic\r", SHELL)
    s.expect(b"tcc -run magic.c /bin/ls\r", SHELL, b"\nLXE1\n")
    s.expect(b"./magic /bin/ls\r", SHELL, b"\nLXE1\n")

    # Compile a second utility that reads stdin or a file relative to its cwd.
    lines = [
        b"#include <stdio.h>", b"#include <lixin.h>",
        b"int main(int n,char **v){ int c,fd,k; char buf[4];",
        b'if(n==1){while((c=getchar())!=EOF)putchar(c); return 0;}',
        b'fd=lx_open(v[1]); if(fd<0){fprintf(stderr,"cannot open\\n");return 7;}',
        b'while((k=lx_read(fd,buf,sizeof buf))>0)for(int i=0;i<k;i++)putchar(buf[i]);',
        b'return lx_close(fd);}',
    ]
    for i, line in enumerate(lines):
        s.expect(b"echo '" + line + (b"' >" if i == 0 else b"' >>") + b" read.c\r", SHELL)
    s.expect(b"tcc read.c -o /bin/readfile\r", SHELL)
    s.expect(b"cd ls-test\r", b"lixin:/ls-test$ ")
    prompt = b"lixin:/ls-test$ "
    s.expect(b"readfile .secret | readfile\r", prompt, b"\nabc\n")
    s.expect(b"readfile\rhi\x04", prompt, b"\nhi")
    s.expect(b"readfile < .secret\r", prompt, b"\nabc\n")
    s.expect(b"readfile missing > errtest\r", prompt, b"\ncannot open\n")
    s.expect(b"echo $?\r", prompt, b"\n7\n")
    s.expect(b"ls -l errtest\r", prompt, b"0  errtest\n")
    s.expect(b"cd /\r", SHELL)


def test_file_limit():
    with tempfile.TemporaryDirectory() as temp:
        disk = Path(temp) / "large.img"
        with boot(disk) as s:
            # echo writes two bytes; doubling nineteen times reaches exactly 1 MiB.
            s.expect(b"echo x > large\r", SHELL)
            for _ in range(19):
                s.expect(b"cat large >> large\r", SHELL)
            s.expect(b"ls -l large\r", SHELL, b"1048576  large")
            s.expect(b"echo >> large\r", SHELL, b"file too large")
            s.expect(b"cat large > copy1\r", SHELL)
            s.expect(b"cat large > copy2\r", SHELL)
            s.expect(b"cat large > copy3\r", SHELL)
            s.expect(b"echo quit\r", SHELL, b"\nquit\n")
            s.expect(b"q\x7fecho still-running\r", SHELL, b"\nstill-running\n")
            s.send(b"q\r")
            assert s.vm.wait(timeout=10) == 0
        with boot(disk) as s:
            for name in (b"large", b"copy1", b"copy2", b"copy3"):
                s.expect(b"cat " + name + b" | wc -l\r", SHELL, b"\n524288\n")
            s.expect(b"ls -l large\r", SHELL, b"1048576  large")


def test():
    with tempfile.TemporaryDirectory() as temp:
        disk = Path(temp) / "test.img"
        with boot(disk) as s:
            s.expect(b"\x1b[Aecho first\r", SHELL, b"\nfirst\n")
            s.expect(b"echo second\r", SHELL, b"\nsecond\n")
            s.expect(b"\x1b[A\x1b[A!\r", SHELL, b"\nfirst!\n")
            s.expect(b"echo draft\x1bOA\x1bOB!\r", SHELL, b"\ndraft!\n")
            s.expect(b"ec\tcompleted\r", SHELL, b"\ncompleted\n")
            s.expect(b"mkdir tabdir\r", SHELL)
            s.expect(b"echo works > 'tabdir/my file'\r", SHELL)
            s.expect(b"cat tabd\tmy\t\r", SHELL, b"\nworks\n")
            s.expect(b"cat 'tabdir/my\t\r", SHELL, b"\nworks\n")
            s.expect(b"p\t\t", SHELL, b"pwd  python")
            s.expect(b"w\t\r", SHELL, b"\n/\n")
            s.expect(b"echo '#include <stdio.h>' > hello.c\r", SHELL)
            s.expect(b"""echo 'int main(void){ puts("Hello from TinyCC"); return 6*7; }' >> hello.c\r""", SHELL)
            s.expect(b"tcc -run hello.c\r", SHELL, b"\nHello from TinyCC\n[exit 42]\n")
            s.expect(b"echo 'int main( {' > bad.c\r", SHELL)
            s.expect(b"tcc -run bad.c\r", SHELL, b"error:")
            s.expect(b"echo '#include <stdio.h>' > sum.c\r", SHELL)
            s.expect(b"echo 'int base=3; int twice(int x){return 2*x;}' >> sum.c\r", SHELL)
            s.expect(b"""echo 'int main(void){int n=base; for(int i=0;i<4;i++) n=twice(n); printf("sum=%d%c",n,10); return n;}' >> sum.c\r""", SHELL)
            s.expect(b"tcc -run sum.c\r", SHELL, b"sum=48\n[exit 48]")
            s.expect(b"tcc hello.c -o hello\r", SHELL)
            s.expect(b"./hello\r", SHELL, b"Hello from TinyCC\n[exit 42]")
            s.expect(b"tcc bad.c -o hello\r", SHELL, b"error:")
            s.expect(b"echo $PATH\r", SHELL, b"\n/bin\n")
            s.expect(b"tcc hello.c -o /bin/hello\r", SHELL)
            s.expect(b"mkdir work\r", SHELL)
            s.expect(b"/bin/cd work\r", b"lixin:/work$ ")
            s.expect(b"hello\r", b"lixin:/work$ ", b"Hello from TinyCC\n[exit 42]")
            s.expect(b"/bin/ls -1 /bin\r", b"lixin:/work$ ", b"\nagent\nagent-run\nbash\ncat\ncd\n")
            s.expect(b"/bin/echo check | /bin/grep check > log\r", b"lixin:/work$ ")
            s.expect(b"/bin/cat log\r", b"lixin:/work$ ", b"\ncheck\n")
            s.expect(b"/bin/tcc /hello.c -o /work/pwd\r", b"lixin:/work$ ")
            s.expect(b"PATH=/work\r", b"lixin:/work$ ")
            s.expect(b"hello\r", b"lixin:/work$ ", b"unknown command")
            s.expect(b"ls\r", b"lixin:/work$ ", b"unknown command")
            s.expect(b"pwd\r", b"lixin:/work$ ", b"Hello from TinyCC\n[exit 42]")
            s.expect(b"/bin/pwd\r", b"lixin:/work$ ", b"\n/work\n")
            s.expect(b"PATH=.:/bin\r", b"lixin:/work$ ")
            s.expect(b"hell\t\r", b"lixin:/work$ ", b"Hello from TinyCC\n[exit 42]")
            s.expect(b"cd /\r", SHELL)
            s.expect(b"rm hello.c\r", SHELL)
            s.expect(b"tcc sum.c -o sum\r", SHELL)
            s.expect(b"./sum\r", SHELL, b"sum=48\n[exit 48]")
            check_ls(s)
            rebuild_ls(s)
            s.expect(b"cecho\r", b"(one line)> ")
            s.expect(b"C in EL0\r", SHELL, b"C in EL0\n")
            s.expect(b"tcc /src/cecho/cecho.c -o /bin/cecho\r", SHELL)
            s.expect(b"./magic /bin/cecho\r", SHELL, b"\nLXE1\n")
            s.expect(b"echo rebuilt | cecho\r", SHELL, b"(one line)> rebuilt\n")
            s.expect(b"echo saved > note\r", SHELL)
            s.expect(b"python\r", PYTHON, b"MicroPython v1.25.0")
            s.expect(b"x = 6\r", PYTHON)
            s.expect(b"x * 7\r", PYTHON, b"\n42\n")
            s.expect(b"for i in range(3):\r    print(i)\r\r", PYTHON, b"\n0\n1\n2\n")
            s.expect(b"1 / 0\r", PYTHON, b"ZeroDivisionError")
            s.expect(b"print(input('name? '))\rAda\r", PYTHON, b"\nAda\n")
            s.expect(b"for i in range(4000):\r    a = [i] * 64\r\r", PYTHON)
            s.expect(b"x + a[0]\r", PYTHON, b"\n4005\n")
            s.expect(b"\x04", SHELL)
            s.expect(b"cat note\r", SHELL, b"\nsaved\n")
            s.expect(b"python\r", PYTHON, b"MicroPython")
            s.expect(b"x\r", PYTHON, b"NameError")
            s.expect(b"\x04", SHELL)
            s.expect(b"echo 'for i in range(3):' > hello.py\r", SHELL)
            s.expect(b"""echo '    print("Hello", i)' >> hello.py\r""", SHELL)
            output = b"\nHello 0\nHello 1\nHello 2\n"
            s.expect(b"python hello.py\r", SHELL, output)
            s.expect(b"touch empty.py\r", SHELL)
            s.expect(b"python empty.py\r", SHELL, b"python empty.py\n")
            s.expect(b"python missing.py\r", SHELL, b"file not found")
            s.expect(b"mkdir scripts\r", SHELL)
            s.expect(b"python scripts\r", SHELL, b"is a directory")
            s.expect(b"""echo '1 / 0' > 'scripts/bad script.py'\r""", SHELL)
            s.expect(b"""python 'scripts/bad script.py'\r""", SHELL,
                     b'File "scripts/bad script.py", line 1')
            s.expect(b"nano edited.py\r", b"^U Paste line")
            # Fix the first line with Up/End/Backspace, then cut/paste the last.
            s.send(b"x = 60\rprint(x * 7)\x1b[A\x05\x7f\x1b[B\x0b\x15\x0f")
            s.until(b"Saved")
            s.expect(b"\x18", SHELL)
            s.expect(b"python edited.py\r", SHELL, b"\n42\n")
            s.expect(b"nano edited.py\r", b"^U Paste line", b"x = 6")
            s.expect(b"#\x18", b"Save changes?")
            s.expect(b"n", SHELL)
            s.expect(b"echo history-before-shutdown\r", SHELL, b"\nhistory-before-shutdown\n")
            s.send(b"/bin/shutdown\r")
            s.until(b"Shutting down LixinOS")
            assert s.vm.wait(timeout=10) == 0
        # A new QEMU process gets only the disk image from the first boot.
        with boot(disk) as s:
            s.expect(b"\x1b[A\r", SHELL, b"\nhistory-before-shutdown\n")
            s.expect(b"ls -lah /bin\r", SHELL, b"\nguest-built ls\n")
            s.expect(b"echo persisted | cecho\r", SHELL, b"(one line)> persisted\n")
            s.expect(b"cat /src/ls/ls.c | grep guest-built\r", SHELL, b'guest-built ls"')
            s.expect(b"/bin/cd /ls-test\r", b"lixin:/ls-test$ ")
            s.expect(b"ls -lah\r", b"lixin:/ls-test$ ", b"1.5 KiB  data\n")
            s.expect(b"/bin/ls -lah | grep .secret > /listing\r", b"lixin:/ls-test$ ")
            s.expect(b"cat /listing\r", b"lixin:/ls-test$ ", b"4 B  .secret\n")
            s.expect(b"cd /\r", SHELL)
            s.expect(b"/bin/cat hello.c\r", SHELL, b"file not found")
            s.expect(b"/bin/cd work\r", b"lixin:/work$ ")
            s.expect(b"hello\r", b"lixin:/work$ ", b"Hello from TinyCC\n[exit 42]")
            s.expect(b"cd /\r", SHELL)
            s.expect(b"./hello\r", SHELL, b"Hello from TinyCC\n[exit 42]")
            s.expect(b"./sum\r", SHELL, b"sum=48\n[exit 48]")
            s.expect(b"python hello.py\r", SHELL, output)
            s.expect(b"nano edited.py\r", b"^U Paste line", b"print(x * 7)")
            s.expect(b"\x18", SHELL)
            s.expect(b"python edited.py\r", SHELL, b"\n42\n")


if __name__ == "__main__":
    test()
    test_file_limit()
    print("Passed guest TinyCC rebuild of /bin/ls, arguments, file and stream I/O, exit status, Python and restart persistence.")
