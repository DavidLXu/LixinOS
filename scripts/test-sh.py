"""Verify the disk-resident EL0 login shell, nested execution and replacement."""
import runpy
from pathlib import Path
import tempfile

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]
with tempfile.TemporaryDirectory() as directory:
    disk = Path(directory) / "shell.img"
    with serial["boot"](disk) as s:
        s.expect(b"ls /src/bash\r", prompt, b"bash.c")
        s.expect(b"bash --version\r", prompt, b"LixinOS Bash (C shell)")
        s.expect(b"sh --version\r", prompt, b"LixinOS Bash (C shell)")
        s.expect(b"v=parent\r", prompt)
        s.expect(b"sh\r", prompt)
        s.expect(b"v=child\r", prompt)
        s.expect(b"mkdir work\r", prompt)
        s.expect(b"cd work\r", b"lixin:/work$ ")
        s.expect(b"echo $v | grep child > out\r", b"lixin:/work$ ")
        s.expect(b"exit\r", prompt)
        s.expect(b"echo $v\r", prompt, b"\nparent\n")
        s.expect(b"cat work/out\r", prompt, b"\nchild\n")
        for number in range(2, 12):
            s.expect(b"shell new\r", prompt, f"[shell {number}]".encode())
        s.expect(b"v=last\r", prompt)
        s.expect(b"cd work\r", b"lixin:/work$ ")
        s.expect(b"shell use 1\r", prompt)
        s.expect(b"echo $v\r", prompt, b"\nparent\n")
        s.expect(b"shell list\r", prompt, b"11  /work")
        s.expect(b"shell use 11\r", b"lixin:/work$ ")
        s.expect(b"echo $v\r", b"lixin:/work$ ", b"\nlast\n")
        s.expect(b"shell close\r", prompt, b"[shell 1]")
        s.expect(b"shell use 11\r", prompt, b"unknown shell ID")
        # The saved C parent retains argv, an open file and redirected stdout
        # across both a successful spawn and a failed lookup.
        source = [
            "#include <stdio.h>", "#include <lixin.h>",
            "int main(int argc,char **argv){char b; int fd=lx_open(argv[1]);",
            'char *args[]={"echo","nested-output"};',
            'if(lx_spawn("/bin/echo",2,args)!=0 || lx_spawn("/missing",2,args)!=-1)return 3;',
            'if(lx_read(fd,&b,1)!=1 || lx_close(fd)!=0)return 4;',
            'printf("%s %c\\n",argv[1],b); return 37;}',
        ]
        for i, line in enumerate(source):
            redirect = ">" if i == 0 else ">>"
            s.expect(f"echo '{line}' {redirect} probe.c\r".encode(), prompt)
        s.expect(b"tcc probe.c -o probe\r", prompt)
        s.expect(b"./probe work/out > probe.out\r", prompt)
        s.expect(b"echo $?\r", prompt, b"\n37\n")
        s.expect(b"cat probe.out\r", prompt, b"nested-output\nwork/out c\n")
        # Replace the canonical Bash file in the guest. Both sh and login
        # must follow it across restart while the running shell stays alive.
        s.expect(b"nano /src/bash/bash.c\r", b"^U Paste line")
        s.expect(b'#define BASH_PROMPT "v2:%s$ "\r\x0f', b"Saved")
        s.expect(b"\x18", prompt)
        s.expect(b"tcc /src/bash/bash.c -o /bin/bash\r", prompt)
        s.expect(b"sh\r", b"v2:/$ ")
        s.expect(b"echo version-two\r", b"v2:/$ ", b"\nversion-two\n")
        s.expect(b"exit\r", prompt)
        s.send(b"q\r")
        assert s.vm.wait(timeout=8) == 0
    with serial["boot"](disk, prompt=b"v2:/$ ") as s:
        s.expect(b"echo persistent\r", b"v2:/$ ", b"\npersistent\n")
print("Passed Bash login, sessions, nested spawn, and in-guest rebuild across reboot.")
