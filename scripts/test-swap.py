"""Exercise paging and physical frame reuse in the Rust and guest-built TCC kernels."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
from disk_image import prepare, edit_files
from swap_image import create

ROOT = Path(__file__).resolve().parents[1]
h = runpy.run_path(str(ROOT/"scripts/test-kernel-threads.py"))
n = h["native"]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rust",required=True,type=Path)
    p.add_argument("--tcc",type=Path)
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
        tcc=args.tcc or work/"kernel.elf"
        if not args.tcc:
            tcc.write_bytes(n["read_file"](disk,"/boot/kernel.elf"))
        subprocess.run(["bash",ROOT/"User/gcc/build.sh","rt"],check=True)
        elf=work/"swap-probe.elf"
        subprocess.run([ROOT/"Kernel/target/gcc-port/cc","-O2",ROOT/"User/cpython/swap-probe.c","-o",elf],check=True)
        package=work/"package"
        runpy.run_path(str(ROOT/"User/gcc/package.py"))["tool"](package,"/bin/swap-probe",elf,library="/lib/swap-probe",build_dir=work)
        def install(rows):
            rows[b"/swap-input"] = bytes(8), b"private file"
            for f in package.rglob("*"):
                rows[("/"+str(f.relative_to(package))).encode()]=bytes(8),f.read_bytes() if f.is_file() else None
        edit_files(disk,install)
        pagefile=create(work/"swap.raw",64,3)
        for name,k,ram in (("rust",args.rust,"768M"),("tcc",tcc,"1536M")):
            cmd=h["args"](k,disk,2,ram)+["-drive",f"if=none,id=s,format=raw,snapshot=on,file={pagefile}",
                                       "-device","virtio-blk-device,drive=s,bus=virtio-mmio-bus.9"]
            with h["boot"](disk,cmd) as g:
                g.send(b"/bin/swap-probe\r")
                out=g.until(n["shell"],timeout=90)
                print(name,out,flush=True)
                assert b"swap roundtrip OK" in out
                n["shutdown"](g)


if __name__=="__main__":
    main()
