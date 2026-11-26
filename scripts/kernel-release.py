#!/usr/bin/env python3
"""Publish and restore dated pairs of bootable LixinOS kernels in local Git."""
import argparse
from datetime import datetime
import fcntl
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import runpy
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from disk_image import checksum, prepare, snapshot_headers

ROOT = Path(__file__).resolve().parents[1]
ART = "kernel-artifacts"
PROFILES = {"release": ("release", None), "retina": ("retina", "retina"),
            "window": ("window", "window-console"), "split": ("split", "split"),
            "ssh": ("release", "ssh-mux")}
TAG_PATTERN = r"kernel-\d{4}-\d{2}-\d{2}\.([1-9]\d*)"


def git(*args, cwd=ROOT, env=None):
    return subprocess.check_output(["git", *args], cwd=cwd, env=env).decode().strip()


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def disk_files(path):
    decode = runpy.run_path(str(ROOT / "scripts/export-git.py"))["decode"]
    with path.open("rb") as image:
        for _, offset, size, expected, header in snapshot_headers(image):
            image.seek(offset + 512)
            body = image.read(size)
            if len(body) == size and checksum(body) == expected:
                return decode(body, header[:8])
    raise ValueError(f"no valid filesystem snapshot in {path}")


def import_native(disk, tree):
    # Import sources only. Never archive a developer's disk or home directory.
    files = disk_files(disk)
    prefix = PurePosixPath("/src/kernel")
    for name in ("sources", "loader-sources", "boot.S", "kernel.c", "loader.c"):
        if not files.get(prefix / name):
            raise ValueError(f"{disk} lacks {prefix / name}")
    native = tree / "Kernel/native"
    for entry in native.rglob("*"):
        if entry.is_file() and entry.suffix in (".h", ".c", ".S"):
            entry.unlink()
    for name, data in files.items():
        if prefix not in name.parents or data is None:
            continue
        dest = native.joinpath(*name.relative_to(prefix).parts)
        dest.parent.mkdir(exist_ok=True, parents=True)
        dest.write_bytes(data)


def version_sources(tree, tag):
    header = tree / "Kernel/native/version.h"
    original = header.read_text()
    stamped, count = re.subn(r'^#define KERNEL_TAG "[^"\n]+"$', f'#define KERNEL_TAG "{tag}"',
                             original, flags=re.MULTILINE)
    if not count:  # Source disk made before the introduction of dated releases.
        stamped, count = re.subn(r'^#define KERNEL_RELEASE "[^"\n]+"$',
                                 f'#define KERNEL_TAG "{tag}"\n#define KERNEL_RELEASE KERNEL_TAG "-tcc"',
                                 original, flags=re.MULTILINE)
    if count != 1:
        raise ValueError("unrecognized native/version.h release definition")
    header.write_text(stamped)


def build(tree, output, disk, tag):
    kernel = tree / "Kernel"
    target = kernel / "target"
    user_cache = target / "user-build"
    target.mkdir(exist_ok=True)
    cache = ROOT / "Kernel/target/user-build"
    if cache.is_dir():
        # Only seed the projects used by build-user.sh. Independent desktop
        # ports can be compiling here and renaming temporary objects at any time.
        user_cache.mkdir()
        dependencies = {"cpython-port", "git-objects", "libgit2", "links-port", "links-web",
                        "lust-port", "lust-src", "micropython", "python-objects", "ssh-build",
                        "tinycc"}
        seeds = [entry for entry in cache.iterdir() if entry.is_file() or
                 entry.name in dependencies or entry.name.startswith(("arm-gnu-toolchain-",
                                                                       "bearssl-", "links-2."))]
        if seeds:
            subprocess.run(["cp", "-cRp", *seeds, user_cache], check=True)
    rustup = shutil.which("rustup") or str(Path.home() / ".cargo/bin/rustup")
    env = dict(os.environ, PATH=str(Path(rustup).parent) + os.pathsep + os.environ["PATH"])
    for variant, (profile, feature) in PROFILES.items():
        args = [rustup, "run", "stable", "cargo", "build", "--locked", "--profile", profile,
                "--target-dir", str(target)]
        if feature:
            args.extend(["--features", feature])
        subprocess.run(args, cwd=kernel, env=env, check=True)
        shutil.copyfile(target / "aarch64-unknown-none-softfloat" / profile / "lixin-kernel",
                        output / f"rust-{variant}.elf")
    shutil.copyfile(user_cache / "cpython-port/cpython.pkg", output / "cpython.pkg")

    prepare(disk)
    test = runpy.run_path(str(tree / "scripts/test-native-kernel.py"))
    # Use a new disk, including when a user has configured test seed overrides.
    args = ["qemu-system-aarch64", "-machine", "virt", "-accel", "hvf", "-cpu", "host",
            "-m", "640M", "-display", "none", "-monitor", "none", "-serial", "stdio",
            "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"if=none,id=vol,format=raw,file={disk}",
            "-device", "virtio-blk-device,drive=vol,bus=virtio-mmio-bus.0",
            "-object", "rng-random,id=rng,filename=/dev/urandom",
            "-device", "virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5",
            "-fw_cfg", f"name=opt/lixinos/cpython,file={output / 'cpython.pkg'}",
            "-kernel", str(output / "rust-release.elf")]
    with test["guest"]["boot"](disk, args, prompt=f"Kernel {tag}-rust".encode()) as serial:
        serial.until(test["shell"])
        test["command"](serial, "uname -r", expected=f"\n{tag}-rust\n".encode())
        test["command"](serial, "tcc /src/tinycc/bootstrap.c -o /bin/tcc")
        test["command"](serial, "tcc --bootloader @/src/kernel/loader-sources -o /boot/loader.elf")
        test["command"](serial, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
        test["shutdown"](serial)
    files = disk_files(disk)
    for name in ("kernel", "loader"):
        (output / f"tcc-{name}.elf").write_bytes(files[PurePosixPath(f"/boot/{name}.elf")])
    with test["native_boot"](output / "tcc-loader.elf", disk, prompt=f"Kernel {tag}-tcc".encode()) as serial:
        serial.until(test["shell"])
        test["command"](serial, "uname -r", expected=f"\n{tag}-tcc\n".encode())
        test["shutdown"](serial)
    with disk.open("rb") as src, gzip.open(output / "tcc-system.img.gz", "wb", compresslevel=1) as dst:
        shutil.copyfileobj(src, dst, 1 << 20)


def publish(temp, guest_disk):
    now = datetime.now().astimezone()
    prefix = f"kernel-{now.date()}."
    previous = latest_tag(prefix)
    tag = prefix + str(int(previous[len(prefix):]) + 1 if previous else 1)
    print(f"Building {tag}", flush=True)
    tree = temp / "src"
    tree.mkdir()
    index = dict(os.environ, GIT_INDEX_FILE=str(temp / "index"))
    head = git("rev-parse", "HEAD")
    git("read-tree", head, env=index)
    # Include pending project source changes without touching the real Git index.
    git("add", "-u", "--", ".", env=index)
    git("add", "-A", "--", "Kernel", "User", "scripts", "docs", "README.md", "AGENTS.md", env=index)
    git("rm", "-rq", "--cached", "--ignore-unmatch", ART, env=index)
    git("checkout-index", "-a", "--prefix=" + str(tree) + "/", env=index)
    if guest_disk:
        import_native(guest_disk, tree)
    version_sources(tree, tag)
    index.update(GIT_DIR=git("rev-parse", "--absolute-git-dir"), GIT_WORK_TREE=str(tree))
    git("add", "-A", "--", "Kernel/native", cwd=tree, env=index)
    source_tree = git("write-tree", cwd=tree, env=index)
    output = tree / ART
    output.mkdir()
    disk = temp / "tcc-system.img"
    build(tree, output, disk, tag)
    manifest = {"tag": tag, "date": now.isoformat(timespec="seconds"), "source_tree": source_tree,
                "tcc_disk_sha256": digest(disk),
                "sha256": {p.name: digest(p) for p in sorted(output.iterdir())}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    git("add", "-f", "--", ART, cwd=tree, env=index)
    commit = git("commit-tree", git("write-tree", cwd=tree, env=index), "-p", head, "-m",
                 f"Archive Rust and TCC kernels for {tag}")
    git("tag", "-a", tag, commit, "-m", f"LixinOS Rust and TCC kernels, {now.isoformat(timespec='seconds')}")
    print(f"Published {tag} at {commit}", flush=True)


def _git_dir():
    dot = ROOT / ".git"
    if dot.is_dir():
        return dot
    if dot.is_file():
        line = dot.read_text().strip()
        if line.startswith("gitdir:"):
            target = Path(line.split(":", 1)[1].strip())
            return target if target.is_absolute() else (ROOT / target).resolve()
    return None


def _tag_key(tag):
    m = re.fullmatch(r"kernel-(\d{4})-(\d{2})-(\d{2})\.([1-9]\d*)", tag)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else None


def latest_tag(prefix="kernel-"):
    gdir = _git_dir()
    if gdir is None:
        return ""
    common = gdir
    commondir = gdir / "commondir"
    if commondir.is_file():
        rel = Path(commondir.read_text().strip())
        common = rel if rel.is_absolute() else (gdir / rel).resolve()
    tags = []
    tags_dir = common / "refs/tags"
    if tags_dir.is_dir():
        for entry in tags_dir.iterdir():
            key = _tag_key(entry.name)
            if key is not None and entry.name.startswith(prefix):
                tags.append((key, entry.name))
    packed = common / "packed-refs"
    if packed.is_file():
        for line in packed.read_text(errors="ignore").splitlines():
            if not line or line[0] in "#^":
                continue
            parts = line.split(" ", 1)
            if len(parts) == 2 and parts[1].startswith("refs/tags/"):
                name = parts[1][len("refs/tags/"):]
                key = _tag_key(name)
                if key is not None and name.startswith(prefix):
                    tags.append((key, name))
    if tags:
        return max(tags)[1]
    git_tags = git("tag", "-l", "--sort=-version:refname", prefix + "*").splitlines()
    return next((tag for tag in git_tags if re.fullmatch(TAG_PATTERN, tag)), "")


def _file_stamp(path):
    st = path.stat()
    return [st.st_ino, st.st_size, st.st_mtime_ns]


def boot_files_info(tag, variant):
    """Cache and stat-verify only the selected boot binaries from a local paired release."""
    tag = latest_tag() if tag == "latest" else tag
    if not re.fullmatch(TAG_PATTERN, tag):
        raise ValueError("no matching local kernel release")
    files = ("tcc-kernel.elf",) if variant == "tcc" else (f"rust-{variant}.elf", "cpython.pkg")
    directory = ROOT / "data/kernel-cache" / tag
    stamp_path = directory / ".verified.json"
    if stamp_path.is_file():
        try:
            cached = json.loads(stamp_path.read_text())
            if cached.get("tag") == tag and "has_gpu_abi" in cached:
                stamps = cached.get("stamps", {})
                if all((directory / name).is_file() and stamps.get(name) == _file_stamp(directory / name)
                       for name in files):
                    return directory, bool(cached["has_gpu_abi"])
        except (OSError, ValueError, KeyError):
            pass
    ref = "refs/tags/" + tag
    manifest = json.loads(git("show", f"{ref}:{ART}/manifest.json"))
    if manifest["tag"] != tag:
        raise ValueError("kernel release manifest tag mismatch")
    has_gpu_abi = subprocess.run(
        ["git", "cat-file", "-e", f"{ref}:Kernel/native/gpu-abi.h"],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0
    directory.mkdir(parents=True, exist_ok=True)
    stamps = {}
    if stamp_path.is_file():
        try:
            old = json.loads(stamp_path.read_text())
            if old.get("tag") == tag:
                stamps = dict(old.get("stamps", {}))
        except (OSError, ValueError):
            pass
    for name in files:
        expected = manifest["sha256"][name]
        path = directory / name
        if path.is_file() and digest(path) == expected:
            stamps[name] = _file_stamp(path)
            continue
        with tempfile.NamedTemporaryFile(dir=directory, prefix="boot-", delete=False) as stream:
            staging = Path(stream.name)
            try:
                subprocess.run(["git", "show", f"{ref}:{ART}/{name}"], cwd=ROOT,
                               stdout=stream, check=True)
                stream.flush()
                if digest(staging) != expected:
                    raise ValueError(f"kernel release checksum mismatch: {name}")
                os.replace(staging, path)
                stamps[name] = _file_stamp(path)
            finally:
                staging.unlink(missing_ok=True)
    tmp_stamp = directory / f".verified.{os.getpid()}.tmp"
    try:
        tmp_stamp.write_text(json.dumps({
            "tag": tag, "has_gpu_abi": has_gpu_abi, "stamps": stamps,
        }))
        os.replace(tmp_stamp, stamp_path)
    finally:
        tmp_stamp.unlink(missing_ok=True)
    return directory, has_gpu_abi


def boot_files(tag, variant):
    directory, _ = boot_files_info(tag, variant)
    print(directory)


def restore(tag, dest):
    if not re.fullmatch(TAG_PATTERN, tag):
        raise ValueError("expected kernel-YYYY-MM-DD.N")
    if dest.exists() or dest.is_symlink():
        raise ValueError(f"choose a new restore directory: {dest}")
    git("worktree", "add", "--detach", str(dest), "refs/tags/" + tag)
    try:
        artifacts = dest / ART
        manifest = json.loads((artifacts / "manifest.json").read_text())
        expected = {f"rust-{v}.elf" for v in PROFILES} | {
            "tcc-kernel.elf", "tcc-loader.elf", "tcc-system.img.gz", "cpython.pkg"}
        if manifest["tag"] != tag or set(manifest["sha256"]) != expected:
            raise ValueError("incomplete kernel release manifest")
        for name, checksum_hex in manifest["sha256"].items():
            if digest(artifacts / name) != checksum_hex:
                raise ValueError(f"checksum mismatch: {name}")
        native = dest / "data/tcc-os"
        native.mkdir(parents=True)
        with gzip.open(artifacts / "tcc-system.img.gz", "rb") as src, (native / "system.img").open("xb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        if digest(native / "system.img") != manifest["tcc_disk_sha256"]:
            raise ValueError("restored TCC boot disk checksum mismatch")
        for name in ("kernel", "loader"):
            shutil.copyfile(artifacts / f"tcc-{name}.elf", native / f"{name}.elf")
    except BaseException:
        # Only this newly created worktree is removed after an incomplete restore.
        git("worktree", "remove", "--force", str(dest))
        raise
    print(f"Restored {tag} in {dest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    publish_cmd = commands.add_parser("publish", help="build, boot-test and tag both kernels")
    publish_cmd.add_argument("--tcc-source-disk", type=Path, help="use /src/kernel from a saved guest disk")
    commands.add_parser("list", help="list paired releases")
    commands.add_parser("latest", help="print the newest local paired release")
    boot = commands.add_parser("boot-files", help="verify and cache boot files from a local release")
    boot.add_argument("tag")
    boot.add_argument("variant", choices=(*PROFILES, "tcc"))
    get = commands.add_parser("restore", help="restore and verify a new runnable worktree")
    get.add_argument("tag")
    get.add_argument("directory", type=Path)
    args = parser.parse_args()
    if args.action == "list":
        print(git("tag", "-l", "--sort=-version:refname", "kernel-*"))
    elif args.action == "latest":
        print(latest_tag())
    elif args.action == "boot-files":
        boot_files(args.tag, args.variant)
    elif args.action == "restore":
        restore(args.tag, args.directory.absolute())
    else:
        common = Path(git("rev-parse", "--git-common-dir"))
        with (ROOT / common / "lixinos-kernel-release.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            with tempfile.TemporaryDirectory(prefix="lixinos-kernels-") as tmp:
                publish(Path(tmp), args.tcc_source_disk)


if __name__ == "__main__":
    main()
