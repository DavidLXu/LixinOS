"""Relink upstream's static AArch64 archives with the LixinOS startup and libc."""
import argparse
from pathlib import Path
import shlex
import subprocess

port = Path(__file__).resolve().parent


def link(source, sdk, output, objects=None, dynamic=False):
    release = source / "out/Release"
    if objects is None:
        commands = subprocess.check_output(["ninja", "-t", "commands", "node"], cwd=release, text=True)
        link_line = shlex.split(commands.splitlines()[-1])
        if link_line[link_line.index("-o") + 1] != "node":
            raise ValueError("unexpected upstream Node link command")
        # GYP repeats archives on both sides of main with whole-archive and
        # group switches. Preserve their exact order when changing libc.
        objects = [arg for arg in link_line[1:] if arg.startswith("obj/") or arg in
                   ("-Wl,--whole-archive", "-Wl,--no-whole-archive", "-Wl,--start-group", "-Wl,--end-group")]
    def compiler_file(name):
        return subprocess.check_output(["c++", "-print-file-name=" + name], text=True).strip()
    platform = output.with_suffix(".platform.o")
    pic = ["-fPIC", "-DLX_DYNAMIC_RUNTIME"] if dynamic else []
    subprocess.run(["cc", *pic, "-D__lixinos__", "-Os", "-ffunction-sections", "-I", source/"deps/uv/include", "-c", port/"platform.c", "-o", platform], check=True)
    volume = output.with_suffix(".volume.o")
    subprocess.run(["cc", *pic, "-Os", "-I", source / "src", "-c", port / "volume.c", "-o", volume], check=True)
    if dynamic:
        subprocess.run(["c++", "-pie", "-nostdlib", "-Wl,-E,-z,text,--gc-sections,--build-id=none,-z,max-page-size=4096",
                        "-Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1,-rpath,/lib/cxx-runtime",
                        "-Wl,--wrap=pthread_getattr_np,--wrap=getrlimit,--wrap=uv_guess_handle,--wrap=getaddrinfo,--wrap=read",
                        "-o", output, sdk/"Scrt1.o", sdk/"crti.o", compiler_file("crtbeginS.o"),
                        platform, volume, *objects, "-lstdc++", "-lgcc_s", "-lgcc", "-latomic",
                        sdk/"libc.so", compiler_file("crtendS.o"), sdk/"crtn.o"], cwd=release, check=True)
        return
    subprocess.run(["c++", "-static", "-no-pie", "-nostdlib", "-Wl,--gc-sections,--build-id=none,-z,max-page-size=4096",
                    "-Wl,-T," + str(port/"program.ld"), "-Wl,--wrap=pthread_getattr_np,--wrap=getrlimit,--wrap=uv_guess_handle,--wrap=getaddrinfo,--wrap=read",
                    "-o", output, sdk/"crt1.o", sdk/"crti.o", compiler_file("crtbeginT.o"), platform, volume, *objects,
                    "-Wl,--start-group", "-lstdc++", sdk/"liblx.a", sdk/"libc.a", "-lgcc", "-lgcc_eh", "-latomic",
                    "-Wl,--end-group", compiler_file("crtend.o"), sdk/"crtn.o"], cwd=release, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--dynamic", action="store_true")
    args = parser.parse_args()
    link(args.source.resolve(), args.sdk.resolve(), args.output.resolve(), dynamic=args.dynamic)
