"""Stage the static Rust tool images and their matching target libraries."""
import argparse
from pathlib import Path
import runpy
import shutil
import subprocess
import struct
import zlib

root = Path(__file__).resolve().parents[2]
work = root / "Kernel/target/rust-port"
target = "aarch64-unknown-linux-musl"
gcc = runpy.run_path(str(root / "User/gcc/package.py"))
tool = gcc["tool"]

def package(directory, sysroot=True):
    for name, build, executable in [("rustc", "build", "rustc-main"), ("cargo", "cargo-build", "cargo")]:
        elf = work / build / target / "release" / executable
        tool(directory, "/bin/" + name, elf, "/lib/rust", "-DLX_RUST", work)
    runtime = directory / "lib/rust/rt"
    runtime.mkdir(parents=True, exist_ok=True)
    cc = gcc["py"] / "cc"
    options = ["-Os", "-ffunction-sections", "-fdata-sections"]
    subprocess.run([cc, *options, "-ffreestanding", "-fno-builtin", "-DLX_APPLICATION",
                    "-I", root / "User/gcc", "-c", root / "User/cpython/loader.c",
                    "-o", runtime / "crt0.o"], check=True)
    subprocess.run([cc, *options, "-I", gcc["py"] / "musl-1.2.5/src/process",
                    "-c", root / "User/rust/spawn.c", "-o", runtime / "spawn.o"], check=True)
    subprocess.run([cc, *options, "-c", root / "User/rust/macro-client.c",
                    "-o", runtime / "macro-client.o"], check=True)
    subprocess.run([cc, *options, "-c", root / "User/rust/metadata.c",
                    "-o", runtime / "metadata.o"], check=True)
    subprocess.run(["aarch64-none-elf-ld", "-r", gcc["work"] / "rt/posix.o",
                    gcc["work"] / "rt/memory.o", gcc["work"] / "rt/api.o", "-o", runtime / "posix.o"], check=True)
    if sysroot:
        libs = Path(subprocess.check_output(["rustc", "--target", target, "--print", "target-libdir"], text=True).strip())
        dest = directory / "lib/rust/lib/rustlib" / target / "lib"
        dest.mkdir(parents=True, exist_ok=True)
        for pattern in ("*.rlib", "*.rmeta"):
            for lib in libs.glob(pattern):
                out = dest / lib.name
                if lib.suffix == ".rmeta":
                    raw = lib.read_bytes()
                    out.write_bytes(b"LXRMETA1" + struct.pack("<I", len(raw)) + zlib.compress(raw, 9))
                else:
                    subprocess.run(["aarch64-none-elf-objcopy", "--strip-debug", lib, out], check=True)
        shutil.copy2(libs / "self-contained/libunwind.a", dest / "libunwind.a")
        # Upstream proc_macro requests the GCC unwind ABI when linking its worker.
        (dest / "libgcc_s.a").write_text("INPUT ( /lib/rust/lib/rustlib/" + target + "/lib/libunwind.a )\n")
    shutil.copyfile(work / "rust-1.100.0/COPYRIGHT", directory / "lib/rust/COPYRIGHT.rust")
    for license in ("LICENSE-APACHE", "LICENSE-MIT", "LICENSE-THIRD-PARTY"):
        shutil.copyfile(work / "cargo-src" / license, directory / "lib/rust" / license)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--tools-only", action="store_true", help="stage executables for startup tests")
    args = parser.parse_args()
    package(args.directory, not args.tools_only)
