"""Stage the DeepSeek Harness CLI launcher and its startup dependencies for LixinOS."""
import argparse
import base64
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

port = Path(__file__).resolve().parent
root = port.parents[1]
version = "0.2.1-rc.5"
url = f"https://registry.npmjs.org/@deepseek-ai/dsh/-/dsh-{version}.tgz"
integrity = "WkyJZv4zl9sCAxSzRctGm58m0rilGqwR6pniBiLOz5CPDfHfDuAUicg0AAazXI7oj/Cph6/cjNTncA/rpLTXpw=="


def build(output):
    if output.exists():
        raise FileExistsError(output)
    compiler = shutil.which("aarch64-none-elf-gcc")
    if not compiler:
        compiler, = (root / "Kernel/target/user-build").glob("arm-gnu-*/bin/aarch64-none-elf-gcc")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        work = Path(temp)
        stage = work / "stage"
        lib = stage / "lib/dsh"
        lib.mkdir(parents=True)
        archive = work / "cli.tgz"
        subprocess.run(["curl", "-fsSL", "--retry", "2", url, "-o", archive], check=True)
        if hashlib.sha512(archive.read_bytes()).digest() != base64.b64decode(integrity):
            raise ValueError("unexpected DSH archive digest")
        with tarfile.open(archive) as tar:
            for item in tar:
                name = Path(item.name).relative_to("package")
                if item.isfile() and (name in (Path("LICENSE"), Path("package.json")) or
                                      (name.parent == Path("lib") and name.suffix == ".js")):
                    target = lib / name
                    target.parent.mkdir(exist_ok=True)
                    target.write_bytes(tar.extractfile(item).read())
        subprocess.run(["patch", "-p1", "-i", port / "launcher.patch"], cwd=lib, check=True)
        for name in ("package.json", "package-lock.json"):
            shutil.copyfile(port / name, work / name)
        subprocess.run(["npm", "ci", "--ignore-scripts", "--omit=optional", "--no-audit", "--no-fund"], cwd=work, check=True)
        shutil.copytree(work / "node_modules", lib / "node_modules",
                        ignore=shutil.ignore_patterns(".bin", ".package-lock.json", "prebuilds", "*.map",
                                                    "*.d.ts", "*.d.mts", "*.d.cts"))
        plugins = lib / "node_modules/@deepseek-ai"
        for patch, plugin in (("resolver.patch", "dsh-app-boot"),
                              ("session.patch", "dsh-session-persistence-jsonl"),
                              ("utf8.patch", "dsh-fs-local"),
                              ("history.patch", "dsh-session-persistence-jsonl"),
                              ("guest-files.patch", "dsh-fs-local"),
                              ("guest-shell.patch", "dsh-tool-bash"),
                              ("web-search.patch", "dsh-web-search-deepseek"),
                              ("telemetry.patch", "dsh-base")):
            subprocess.run(["patch", "-p1", "-i", port / patch], cwd=plugins / plugin, check=True)
        host_tools = work / "build-tools"
        shutil.copytree(port / "build-tools", host_tools)
        subprocess.run(["npm", "ci", "--ignore-scripts", "--no-audit", "--no-fund"],
                       cwd=host_tools, check=True)
        subprocess.run(["node", host_tools / "unicode.cjs", lib / "node_modules"], check=True)
        for name in ("fetch.cjs", "workspace.mjs", "volume.cjs"):
            shutil.copyfile(port / name, lib / name)
        (stage / "bin").mkdir()
        subprocess.run([compiler, "-Os", "-ffreestanding", "-fno-builtin", "-nostdlib",
                        "-mgeneral-regs-only", "-Wl,--build-id=none,-s,-z,max-page-size=4096",
                        "-T", port.parent / "ls/link.ld", port / "launcher.c",
                        "-o", stage / "bin/dsh"], check=True)
        stage.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    build(parser.parse_args().output.resolve())
