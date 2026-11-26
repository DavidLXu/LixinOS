"""Package native GNU tools and the guest static C development files."""
import argparse
import hashlib
from pathlib import Path
import os
import shutil
import struct
import subprocess
import zlib

port = Path(__file__).resolve().parent
root = port.parents[1]
work = Path(os.environ.get("LIXINOS_GCC_BUILD", root/"Kernel/target/gcc-port")).resolve()
cache = root / "Kernel/target/user-build"
if not shutil.which("aarch64-none-elf-gcc"):
    cross, = cache.glob("arm-gnu-toolchain-*-aarch64-none-elf")
    os.environ["PATH"] = str(cross/"bin") + os.pathsep + os.environ["PATH"]
py = cache/"cpython-port"
sdk = py/"sysroot"

def run(*args):
    subprocess.run(args, check=True)

def tool(package, name, elf, library="/lib/gcc", launcher_flag="-DLX_GCC", build_dir=work, deflate=False):
    data = elf.read_bytes()
    entry, phoff = struct.unpack_from("<QQ", data, 24)
    phsize, n = struct.unpack_from("<HH", data, 54)
    segments = [struct.unpack_from("<II6Q", data, phoff+i*phsize) for i in range(n)]
    rx, rw = [p for p in segments if p[0] == 1]
    if (rx[1:4] != (5, 0, 0x78000000) or rw[1] != 6 or rw[3] % (2<<20)
        # Large-memory kernels extend the libc arena to the separate program
        # arena at 0xa0000000. SVC 37 checks the running kernel's actual limit.
        or not rx[3] <= entry < rx[3]+rx[5] <= rw[3] or rw[3]+rw[6]+(2<<20) >= 0xa0000000):
        raise ValueError(f"invalid load image for {name}")
    layout = build_dir/f"launch-{elf.stem}"
    layout.mkdir(exist_ok=True)
    contents = package/library.lstrip("/")/"images"
    contents.mkdir(parents=True, exist_ok=True)
    constants = dict(IMAGE_BASE=rx[3], TEXT_BYTES=rw[3]-rx[3], ENTRY_PC=entry, PHNUM=n,
                     STACK_TOP=((rw[3]+rw[6]+4095) & ~4095) + (2<<20))
    lines = [f"#define {key} {value:#x}UL" for key,value in constants.items()]
    lines.append("static const struct piece pieces[] = {")
    for seg in (rx, rw):
        block = data[seg[2]:seg[2]+seg[5]]
        if not block:
            continue
        payload = block
        if deflate:
            compressor = zlib.compressobj(level=9, wbits=-15)
            payload = compressor.compress(block) + compressor.flush()
        filename = hashlib.sha256(payload).hexdigest()[:24]
        (contents/filename).write_bytes(payload)
        digest = 2166136261
        for b in block:
            digest = ((digest ^ b) * 16777619) & 0xffffffff
        lines.append(f'{{"{library}/images/{filename}", {seg[3]:#x}, {len(block)}, {digest:#x}}},')
    (layout/"layout.h").write_text("\n".join([*lines,"};",""]))
    target = package/name.lstrip("/")
    target.parent.mkdir(parents=True,exist_ok=True)
    run("aarch64-none-elf-gcc","-Os","-ffreestanding","-fno-builtin","-fdata-sections","-ffunction-sections",
        "-nostdlib","-Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-s","-T",port.parent/"ls/link.ld",
        launcher_flag,f'-DLX_ARGV0="{name}"',"-I",layout,port.parent/"cpython/loader.c",
        port.parent/"tcc/api.c",
        *(["-DLX_DEFLATE", "-I", sdk/"include", sdk/"lib/libz.a"]
          if deflate else []), "-o",target)

def package_tools(directory):
    library = directory/"lib/gcc"
    library.mkdir(parents=True, exist_ok=True)
    tools = {
        "/bin/as": "build-binutils/gas/as-new",
        "/bin/ld": "build-binutils/ld/ld-new",
        "/bin/gcc": "build-gcc/gcc/xgcc",
        "/lib/gcc/bin/cc1": "build-gcc/gcc/cc1",
    }
    for name, binary in tools.items():
        tool(directory, name, work/binary)
    shutil.copytree(sdk/"include", library/"include", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("openssl","zconf.h","zlib.h"))
    shutil.copyfile(port.parent/"tcc/lixin.h", library/"include/lixin.h")
    shutil.copyfile(sdk/"lib/libc.a", library/"libc.a")
    # musl supplies the symbols in libc and ships these linker-compatible archives.
    for archive in ("libm.a", "libdl.a", "libpthread.a", "librt.a"):
        shutil.copyfile(sdk/"lib"/archive, library/archive)
    shutil.copyfile(work/"rt/liblx.a", library/"liblixin.a")
    shutil.copyfile(port/"application.ld", library/"application.ld")
    libgcc = subprocess.check_output(["aarch64-none-elf-gcc","-print-libgcc-file-name"],text=True).strip()
    run("aarch64-none-elf-objcopy", "--strip-debug", libgcc, library/"libgcc.a")
    (directory/"tmp").mkdir(exist_ok=True)
    run("aarch64-none-elf-objcopy","--redefine-sym","_start=lx_musl_start",sdk/"lib/crt1.o",library/"crt1.o")
    run(py/"cc","-Os","-ffreestanding","-fno-builtin","-DLX_APPLICATION","-I",port,
        "-c",port.parent/"cpython/loader.c","-o",library/"crt0.o")
    for src,name in [(work/"gcc-4.8.5/COPYING3","COPYING.GCC"),(work/"gcc-4.8.5/COPYING.RUNTIME","COPYING.RUNTIME"),
                     (work/"binutils-2.43.1/COPYING3","COPYING.binutils"),(py/"musl-1.2.5/COPYRIGHT","COPYRIGHT.musl"),
                     (work/"gmp-6.2.1/COPYING.LESSERv3","COPYING.GMP"),(work/"mpfr-4.1.0/COPYING.LESSER","COPYING.MPFR"),
                     (work/"mpc-1.2.1/COPYING.LESSER","COPYING.MPC")]:
        shutil.copyfile(src,library/name)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory",type=Path)
    args = parser.parse_args()
    package_tools(args.directory)
