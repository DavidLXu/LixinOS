"""Compile, link, run, and reboot under either kernel on a disposable disk."""
import argparse
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
test = runpy.run_path(str(root/"scripts/test-native-kernel.py"))
command = test["command"]

def check(flavor, package):
    native = flavor == "tcc"
    source = root/("data/tcc-os/system.img" if native else "data/lixinos.img")
    with tempfile.TemporaryDirectory(prefix="lixin-gcc-") as tmp:
        disk = Path(tmp)/"test.img"
        directory = "/tmp/"+Path(tmp).name
        subprocess.run(["cp","-c",source,disk],check=True)
        subprocess.run([sys.executable,root/"User/gcc/install.py",package,disk],check=True)
        def boot():
            if native:
                return test["native_boot"](root/"data/tcc-os/loader.elf",disk,prompt=test["shell"])
            return test["guest"]["boot"](disk)
        with boot() as serial:
            command(serial,"gcc --version",expected=b"gcc (GCC) 4.8.5")
            command(serial,"mkdir "+directory)
            prompt = f"lixin:{directory}$ ".encode()
            def run(line, **kwargs):
                command(serial,line,prompt=prompt,**kwargs)
            run("cd "+directory)
            run("echo '#define BASE 38' > answer.h")
            run("echo '#include \"answer.h\"' > answer.c")
            run("echo 'int answer(int n){return BASE+n;}' >> answer.c")
            run("echo '#include <stdio.h>' > main.c")
            run("echo '#include <stdlib.h>' >> main.c")
            run("echo 'int answer(int); int main(int n,char **v){if(n!=2)return 2; printf(\"result=%d\\n\",answer(atoi(v[1])));return 0;}' >> main.c")
            run("gcc -E answer.c",expected=b"return 38")
            run("gcc -O2 -S answer.c -o answer.s")
            run("as answer.s -o answer.o")
            run("gcc -pipe -std=c11 -Wall -Werror -O2 -c main.c -o main.o")
            run("gcc answer.o main.o -o answer")
            run("./answer 4",expected=b"\nresult=42\n")
            run("./answer",status=2)
            run("echo 'int broken = ;' > invalid.c")
            run("gcc -c invalid.c -o invalid.o",status=1,expected=b"error:")
            run("gcc -shared main.o -o libmain.so",status=1,expected=b"static executables only")
            run("gcc main.o -o incomplete",status=1,expected=b"undefined reference")
            test["shutdown"](serial)
        with boot() as serial:
            command(serial,directory+"/answer 5",expected=b"\nresult=43\n")
            test["shutdown"](serial)
        print(f"{flavor}: GCC compiled, linked, executed and survived reboot",flush=True)

if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", choices=("rust","tcc"))
    parser.add_argument("--package",type=Path,default=root/"Kernel/target/gcc-port/current-package")
    args=parser.parse_args()
    check(args.kernel,args.package.resolve())
