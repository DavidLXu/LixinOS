"""Prepare the pinned official codex-exec workspace and its guest runtime patch."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parent
CODEX = "295eca93394bb83a6ec8e0841033f3997f97fcbd"
V8 = "90a65f887a0c0f048f3784afbcb320b373c8a2e0"
SOURCES = [
    ("source", f"https://codeload.github.com/openai/codex/tar.gz/{CODEX}",
     "16f3717b3b0998d7c9ac769f0c01ae8e36d106abed0c21002c926178b6673e34"),
    ("crossterm-port", "https://codeload.github.com/openai-oss-forks/crossterm/tar.gz/37ca4a27a9424ec5dbe6451f48c339d7b45f8d63",
     "7d5de0724fb0d85020afe58ddb99a39a36178b3e0561d3227eb1572031dcd5dc"),
    ("tokio-port", "https://static.crates.io/crates/tokio/tokio-1.52.3.crate",
     "8fc7f01b389ac15039e4dc9531aa973a135d7a4135281b12d7c1bc79fd57fffe"),
    # Cargo resolves V8 for a sibling workspace member. Its submodules aren't
    # linked into codex-exec. Use the matching crate source without Chromium.
    ("v8-source", f"https://codeload.github.com/openai-oss-forks/rusty_v8/tar.gz/{V8}",
     "b0b188d1f1d851abd78337818a28a15e84f143b082ce966976e9206617684373"),
]


def prepare(work):
    work.mkdir(parents=True, exist_ok=True)
    for directory, url, digest in SOURCES:
        archive = work / (directory + ".tar.gz")
        if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            with tempfile.NamedTemporaryFile(dir=work) as tmp:
                subprocess.run(["curl", "-fLsS", "--retry", "3", url, "-o", tmp.name], check=True)
                if hashlib.sha256(Path(tmp.name).read_bytes()).hexdigest() != digest:
                    raise ValueError(f"checksum mismatch for {directory}")
                archive.write_bytes(Path(tmp.name).read_bytes())
        source = work / directory
        if source.exists():
            continue
        with tempfile.TemporaryDirectory(dir=work) as tmp:
            fresh = Path(tmp) / directory
            fresh.mkdir()
            subprocess.run(["tar", "-xzf", archive, "--strip-components=1", "-C", fresh], check=True)
            fresh.rename(source)

    for directory, patch in [
        ("source", "codex.patch"), ("source", "output.patch"), ("source", "interactive.patch"),
        ("source", "tui.patch"), ("crossterm-port", "crossterm.patch"),
        ("source", "readonly.patch"), ("source", "workspace.patch"), ("source", "perf.patch"),
        ("tokio-port", "tokio.patch"),
    ]:
        command = ["patch", "-p1", "-f", "-i", str(PORT / patch)]
        cwd = work / directory
        # Later guest patches build on earlier patches and alter their context.
        if patch in ("tui.patch", "readonly.patch", "workspace.patch"):
            installed = ["patch", "-p1", "-f", "-R", "--dry-run", "-i", str(PORT / "perf.patch")]
            if subprocess.run(installed, cwd=cwd, capture_output=True).returncode == 0:
                continue
        if patch == "readonly.patch":
            installed = ["patch", "-p1", "-f", "-R", "--dry-run", "-i", str(PORT / "workspace.patch")]
            if subprocess.run(installed, cwd=cwd, capture_output=True).returncode == 0:
                continue
        if subprocess.run([*command, "--dry-run"], cwd=cwd, capture_output=True).returncode == 0:
            subprocess.run(command, cwd=cwd, check=True)
        else:
            subprocess.run([*command, "-R", "--dry-run"], cwd=cwd, check=True, stdout=subprocess.DEVNULL)

    for filename, target in [
        ("tui-events.rs", "tui/src/tui/lixinos_events.rs"),
        ("crossterm-input.rs", "../../crossterm-port/src/event/source/unix/lixinos.rs"),
        ("crossterm-terminal.rs", "../../crossterm-port/src/terminal/sys/lixinos.rs"),
        ("interactive.rs", "exec/src/interactive.rs"),
        ("interactive-main.rs", "exec/src/bin/lixinos-codex.rs"),
        ("output.rs", "exec-server/src/lixinos_output.rs"),
        ("workspace.rs", "core/src/tools/runtimes/unified_exec/lixinos_workspace.rs"),
        ("readonly.rs", "core/src/tools/runtimes/unified_exec/lixinos_readonly.rs"),
    ]:
        source = PORT / filename
        destination = work / "source/codex-rs" / target
        if not destination.exists() or destination.read_bytes() != source.read_bytes():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)

    # The release archive tags each workspace crate 0.171.0 while shipping a
    # development-version lock. Preserve every dependency version and checksum.
    lock = work / "source/codex-rs/Cargo.lock"
    blocks = lock.read_text().split("[[package]]")
    for index, block in enumerate(blocks):
        if 'source = ' not in block:
            block = block.replace('version = "0.0.0"', 'version = "0.171.0"')
        if block.startswith(('\nname = "crossterm"\n', '\nname = "tokio"\n', '\nname = "v8"\n')):
            block = re.sub(r'^(source|checksum) = "[^"]+"\n', '', block, flags=re.MULTILINE)
        if block.startswith('\nname = "codex-exec"\n') and ' "codex-tui",' not in block:
            block = block.replace(' "codex-utils-cli",', ' "codex-tui",\n "codex-utils-cli",')
        blocks[index] = block
    updated = "[[package]]".join(blocks)
    if lock.read_text() != updated:
        lock.write_text(updated)


if __name__ == "__main__":
    prepare(Path(sys.argv[1]).resolve())
