"""End-to-end foreground C and MicroPython tests through the guest serial port."""
from contextlib import contextmanager
import re
import os
from pathlib import Path
import select
import shutil
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


def check_python_system(s):
    s.expect(b"mkdir py-system\r", SHELL)
    prompt = b"lixin:/py-system$ "
    s.expect(b"cd py-system\r", prompt)
    s.expect(b"python\r", PYTHON)
    for code, expected in (
        ("import os, uos, sys, random, time, io", b""),
        ("print(uos is os, sys.platform, os.uname().sysname, os.getcwd())",
         b"True lixinos LixinOS /py-system"),
        ("print(len(os.urandom(8)), random.randint(5, 5), time.time() > 0)",
         b"8 5 True"),
        ('values = [1, 2, 3]; print(values[:-1], {1, 2} == set(values[:2]), (n := 42), f"{n}")',
         b"\n[1, 2] True 42 42\n"),
        ("import select, uselect; print(select is uselect)", b"\nTrue\n"),
        ("print(min(2, 1), max(1, 2), list(enumerate(reversed([1, 2]))))",
         b"\n1 2 [(0, 2), (1, 1)]\n"),
        ("t=time.ticks_ms(); time.sleep_ms(15); print(time.ticks_diff(time.ticks_ms(),t)>=15)",
         b"\nTrue\n"),
        ("os.mkdir('sub'); os.chdir('sub'); print(os.getcwd())", b"\n/py-system/sub\n"),
        ("f=io.open('note','wb'); f.write(b'12'); f.close()", b""),
        ("f=open('note','a'); f.write('3'); f.close()", b""),
        ("f=open('note','r'); print(f.read(),os.stat('note')[6],os.listdir()); f.close()",
         b"123 3 ['note']"),
        ("print(os.system('echo guest | cat > from-shell'))", b"\n0\n"),
        ("print(open('from-shell').read().strip())", b"\nguest\n"),
        ("print(os.system('exit')); print(os.system('missing-command'))", b"\n1\n"),
        ("os.remove('note'); os.remove('from-shell'); os.chdir('..'); os.rmdir('sub')", b""),
    ):
        s.expect(code.encode() + b"\r", PYTHON, expected)
    s.expect(b"\x04", prompt)
    s.expect(b"python -c 'import sys; print(sys.argv[0])'\r", prompt, b"\n<string>\n")
    s.expect(b"""printf q | python -c 'import select, sys; p = select.poll(); p.register(sys.stdin, select.POLLIN); print(bool(p.poll(50)), sys.stdin.read(1), repr(sys.stdin.read(1)))'\r""",
             prompt, b"\nTrue q ''\n")
    s.expect(b"cd /\r", SHELL)


def seed_test_disk(disk):
    # A disk-resident kernel needs its boot ELF and userland in each test VM.
    seed = os.environ.get("LIXINOS_TEST_SEED")
    if seed and not Path(disk).exists():
        with open(seed, "rb") as source, Path(disk).open("xb") as target:
            shutil.copyfileobj(source, target)


@contextmanager
def boot(disk, command=None, prompt=SHELL):
    seed_test_disk(disk)
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
            try:
                vm.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                vm.kill()
                vm.communicate()


def check_ls(s):
    def listing(command):
        s.send(command + b"\r")
        echoed, body = s.until(SHELL).split(b"\n", 1)
        assert echoed == command, echoed
        return body

    def rows(command):
        plain = listing(command).replace(b"\x1b[36m", b"").replace(b"\x1b[39m", b"")
        plain = re.sub(rb" \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC ", b" ", plain)
        return [line.split() for line in plain.splitlines()]

    s.expect(b"mkdir ls-test\r", SHELL)
    s.expect(b"mkdir ls-test/sub\r", SHELL)
    s.expect(b"touch ls-test/empty\r", SHELL)
    s.expect(b"echo abc > ls-test/.secret\r", SHELL)
    # Each append contributes 383 ASCII bytes plus echo's newline.
    for _ in range(4):
        s.expect(b"echo " + b"x" * 383 + b" >> ls-test/data\r", SHELL)
    default = listing(b"ls ls-test")
    assert default == b"data   empty  \x1b[36msub\x1b[39m\n"
    details = listing(b"ls -lh ls-test")
    assert re.search(rb"1\.5 KiB  [^\n]+ UTC  data\n", details)
    assert listing(b"ls -1 ls-test") == b"data\nempty\n\x1b[36msub\x1b[39m\n"
    plain = b"data\nempty\nsub\n"
    assert listing(b"ls ls-test | cat") == plain
    assert listing(b"ls -1 ls-test | cat") == plain
    s.expect(b"ls ls-test > ls-output\r", SHELL)
    assert listing(b"cat ls-output") == plain
    assert rows(b"ls -l ls-test") == [
        [b"-", b"1536", b"data"], [b"-", b"0", b"empty"], [b"d", b"-", b"sub"]]
    human = [[b"-", b"1.5", b"KiB", b"data"], [b"-", b"0", b"B", b"empty"], [b"d", b"-", b"sub"]]
    assert rows(b"ls -lh ls-test") == human
    full = [[b"d", b"-", b"."], [b"d", b"-", b".."], [b"-", b"4", b"B", b".secret"]] + human
    assert rows(b"ls -a ls-test") == [[b".", b"..", b".secret", b"data", b"empty", b"sub"]]
    assert rows(b"ls -lah ls-test") == full
    assert rows(b"ls -h -a -l ls-test") == full
    assert rows(b"ls -l ls-test/.secret") == [[b"-", b"4", b"ls-test/.secret"]]

    s.expect(b"mkdir column-wrap\r", SHELL)
    names = [bytes([c]) * 39 for c in b"abc"]
    for name in names:
        s.expect(b"touch column-wrap/" + name + b"\r", SHELL)
    assert listing(b"ls column-wrap") == names[0] + b"  " + names[1] + b"\n" + names[2] + b"\n"


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
    s.expect(b"ls -lah ls-test | grep .secret\r", SHELL, b"UTC  .secret\n")

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
    s.send(b"ls -l errtest\r")
    assert re.search(rb"0  [^\n]+ UTC  errtest\n", s.until(prompt))
    s.expect(b"cd /\r", SHELL)


def prepare_large_file(s):
    source = [
        b'#include <lixin.h>',
        b'int main(int argc, char **argv) {',
        b'unsigned char block[4096]; for (int i=0; i<4096; i++) block[i]=i*13;',
        b'for (int i=0; i<25600; i++) if (lx_write(1,block,4096)!=4096) return 1;',
        b'return argc==1 || lx_write(1,block,1)==1 ? 0 : 1; }',
    ]
    for i, line in enumerate(source):
        s.expect(b"echo '" + line + (b"' >" if i == 0 else b"' >>") + b" generate.c\r", SHELL)
    s.send(b"tcc generate.c -o generate\r")
    compiled = s.until(SHELL)
    assert b"error" not in compiled, compiled
    s.send(b"./generate > large\r")
    output = s.until(SHELL, timeout=120)
    s.send(b"echo $?\r")
    result = s.until(SHELL)
    assert b"\n0\n" in result, (compiled, output, result)
    s.expect(b"echo >> large\r", SHELL, b"file too large")
    s.send(b"./generate oversized > rejected\r")
    assert b"file too large" in s.until(SHELL, timeout=120)
    s.expect(b"ls -1 rejected\r", SHELL, b"path not found")


def verify_large_file(s):
    s.send(b"ls -l large\r")
    assert re.search(rb"104857600  [^\n]+ UTC  large\n", s.until(SHELL))
    source = [
        b'#include <lixin.h>',
        b'#include <stdio.h>',
        b'int main(void) { int fd=lx_open("large"), n, count=0; unsigned char block[4096];',
        b'if (fd<0) return 1;',
        b'while ((n=lx_read(fd,block,4096))>0) { for (int i=0; i<n; i++,count++)',
        b'if (block[i]!=(unsigned char)((count%4096)*13)) return 2; }',
        b'if (n || count!=104857600) return 3; puts("100 MiB verified"); return lx_close(fd); }',
    ]
    for i, line in enumerate(source):
        s.expect(b"echo '" + line + (b"' >" if i == 0 else b"' >>") + b" verify.c\r", SHELL)
    s.expect(b"tcc verify.c -o verify\r", SHELL)
    s.send(b"./verify\r")
    assert b"100 MiB verified" in s.until(SHELL, timeout=120)


def test_file_limit():
    with tempfile.TemporaryDirectory() as temp:
        disk = Path(temp) / "large.img"
        with boot(disk) as s:
            prepare_large_file(s)
            s.send(b"shutdown\r")
            assert s.vm.wait(timeout=15) == 0
        with boot(disk) as s:
            verify_large_file(s)


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
            s.send(b"/bin/ls -1 /bin\r")
            installed = s.until(b"lixin:/work$ ").splitlines()[1:]
            assert {b"agent", b"agent-run", b"bash", b"cat", b"cd"} <= set(installed), installed
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
            s.expect(b"ls -lah\r", b"lixin:/ls-test$ ", b"UTC  data\n")
            s.expect(b"/bin/ls -lah | grep .secret > /listing\r", b"lixin:/ls-test$ ")
            s.expect(b"cat /listing\r", b"lixin:/ls-test$ ", b"UTC  .secret\n")
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
    with tempfile.TemporaryDirectory() as directory, boot(Path(directory) / "python-system.img") as guest:
        check_python_system(guest)
        guest.send(b"shutdown\r")
        assert guest.vm.wait(timeout=10) == 0
    print("Passed guest TinyCC rebuild of /bin/ls, arguments, file and stream I/O, exit status, Python and restart persistence.")
