"""Build a musl pthread probe and exercise both CPU counts on a private QEMU disk."""
import argparse
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

from disk_image import edit_files, prepare

ROOT = Path(__file__).resolve().parents[1]
native = runpy.run_path(str(ROOT/"scripts/test-native-kernel.py"))
boot = native["guest"]["boot"]
shell = native["shell"]

def args(kernel, disk, cores, ram):
    return ["qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-smp", str(cores), "-m", ram, "-kernel", str(kernel), "-display", "none",
            "-serial", "stdio", "-monitor", "none", "-nic", "none", "-global",
            "virtio-mmio.force-legacy=false", "-drive", f"if=none,id=fs,format=raw,file={disk}",
            "-device", "virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0", "-object",
            "rng-random,id=rng,filename=/dev/urandom", "-device",
            "virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5"]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", type=Path, required=True)
    parser.add_argument("--tcc", type=Path, help="Standalone TCC kernel ELF")
    parser.add_argument("--build-tcc",action="store_true", help="Compile the embedded kernel sources in the guest")
    options=parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        work=Path(tmp)
        disk=work/"test.img"
        prepare(disk)
        subprocess.run(["bash","User/gcc/build.sh","rt"],cwd=ROOT,check=True)
        package_tool=runpy.run_path(str(ROOT/"User/gcc/package.py"))["tool"]
        elf=work/"pthread-probe.elf"
        subprocess.run([ROOT/"Kernel/target/gcc-port/cc","-O2","-mno-outline-atomics",ROOT/"User/cpython/pthread-probe.c","-o",elf],check=True)
        stage=work/"package"
        package_tool(stage,"/bin/pthread-probe",elf,library="/lib/pthread-probe",build_dir=work)
        with boot(disk,args(options.rust,disk,1,"640M")) as guest:
            if options.build_tcc:
                native["command"](guest,"tcc /src/tinycc/bootstrap.c -o /bin/tcc")
                native["command"](guest,"tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
            native["command"](guest,"echo ok > /thread-test")
            native["shutdown"](guest)
        def install(rows):
            for path in stage.rglob("*"):
                rows[("/"+str(path.relative_to(stage))).encode()] = (bytes(8),path.read_bytes() if path.is_file() else None)
        edit_files(disk,install)
        tcc=options.tcc
        if options.build_tcc:
            tcc=work/"tcc.elf"
            tcc.write_bytes(native["read_file"](disk,"/boot/kernel.elf"))
        for kernel,ram in [(options.rust,"640M"),(tcc,"1536M")]:
            if not kernel: continue
            for cores in (2,1):
                with boot(disk,args(kernel,disk,cores,ram)) as guest:
                    guest.send(b"/bin/pthread-probe\r")
                    out=guest.until(shell,timeout=45)
                    m=re.search(rb"pthread OK .*cores=([01]),([01]) detached=32 simultaneous=([0-9]+)",out)
                    assert m and (m[1]!=m[2])==(cores==2),out
                    print(kernel.name,cores,m[0].decode(),flush=True)
                    native["command"](guest,"/bin/pthread-probe exit-group",status=23)
                    native["command"](guest,"/bin/pthread-probe exit-main",expected=b"survived main pthread_exit")
                    native["shutdown"](guest)

if __name__=="__main__":
    main()
