"""Check POSIX identities, filesystem access and persisted ownership on both kernels."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from disk_image import prepare, edit_files

ROOT = Path(__file__).resolve().parents[1]
h = runpy.run_path(str(ROOT/"scripts/test-kernel-threads.py"))
n = h["native"]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rust",required=True,type=Path)
    p.add_argument("--tcc",type=Path)
    p.add_argument("--save-tcc",type=Path,help="save the guest-compiled kernel for subsequent tests")
    args=p.parse_args()
    toolchain = next((ROOT/"Kernel/target/user-build").glob("arm-gnu-toolchain-*-aarch64-none-elf"))
    os.environ["PATH"] = str(toolchain/"bin")+os.pathsep+os.environ["PATH"]
    with tempfile.TemporaryDirectory() as tmp:
        work=Path(tmp)
        disk=work/"guest.img"
        prepare(disk)
        with h["boot"](disk,h["args"](args.rust,disk,2,"768M")) as g:
            if not args.tcc:
                n["command"](g,"tcc /src/tinycc/bootstrap.c -o /bin/tcc")
                n["command"](g,"tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
            n["command"](g,"echo ok > /initial")
            n["shutdown"](g)
        tcc=args.tcc or args.save_tcc or work/"kernel.elf"
        if not args.tcc:
            tcc.write_bytes(n["read_file"](disk,"/boot/kernel.elf"))
        subprocess.run(["bash",ROOT/"User/gcc/build.sh","rt"],check=True)
        elf=work/"identity-probe.elf"
        subprocess.run([ROOT/"Kernel/target/gcc-port/cc","-O2",ROOT/"User/cpython/identity-probe.c","-o",elf],check=True)
        package=work/"package"
        runpy.run_path(str(ROOT/"User/gcc/package.py"))["tool"](package,"/bin/identity-probe",elf,library="/lib/identity-probe",build_dir=work)
        def install(rows):
            rows.setdefault(b"/tmp",(bytes(8),None))
            for f in package.rglob("*"):
                rows[("/"+str(f.relative_to(package))).encode()]=bytes(8),f.read_bytes() if f.is_file() else None
        edit_files(disk,install)
        for name,k,ram in (("rust",args.rust,"768M"),("tcc",tcc,"1536M")):
            for argument,expected in (("",b"private files OK"),(" verify",b"second user denied")):
                with h["boot"](disk,h["args"](k,disk,2,ram)) as g:
                    g.send(("/bin/identity-probe"+argument+"\r").encode())
                    out=g.until(n["shell"],timeout=60)
                    print(name,out,flush=True)
                    assert expected in out
                    n["shutdown"](g)


if __name__=="__main__":
    main()
