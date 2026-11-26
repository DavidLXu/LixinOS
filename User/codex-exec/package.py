"""Package the linked Codex executable and its upstream notices for LixinOS."""
import argparse
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[2]


def is_dynamic_pie(executable):
    with executable.open("rb") as stream:
        header = stream.read(64)
    return header[:7] == b"\x7fELF\x02\x01\x01" and struct.unpack_from("<HH", header, 16) == (3, 183)


def find_strip():
    strip = shutil.which("aarch64-none-elf-strip")
    if strip:
        return strip
    matches = sorted((ROOT / "Kernel/target/user-build").glob("arm-gnu-toolchain-*/bin/aarch64-none-elf-strip"))
    return str(matches[0]) if matches else None


def package_exec(work, output):
    if output.exists():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    executable = work / "target/aarch64-unknown-linux-musl/release/lixinos-codex"
    with TemporaryDirectory(dir=output.parent) as temp:
        package = Path(temp) / "package"
        if is_dynamic_pie(executable):
            (package / "bin").mkdir(parents=True)
            (package / "lib/codex").mkdir(parents=True)
            dest = package / "bin/codex"
            strip = find_strip()
            if strip:
                subprocess.run([strip, "-o", str(dest), str(executable)], check=True)
            else:
                shutil.copyfile(executable, dest)
            dest.chmod(0o755)
        else:
            package_tool = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]
            package_tool(package, "/bin/codex", executable, "/lib/codex", "-DLX_CODEX",
                         work, deflate=True)
        shutil.copytree(work / "source/codex-rs/skills/src/assets/samples",
                        package / "lib/codex/skills")
        for notice in ("LICENSE", "NOTICE"):
            shutil.copyfile(work / "source" / notice, package / "lib/codex" / notice)
        package.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    package_exec(args.work.resolve(), args.output.resolve())
