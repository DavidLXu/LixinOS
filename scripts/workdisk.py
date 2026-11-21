"""Manage the fixed local LixinOS workspace and offline recovery copies."""
import argparse
from datetime import datetime
import os
from pathlib import Path, PurePosixPath
import struct
import time
import subprocess
import tempfile

from disk_image import disk_entries, edit_files, require_local

base = Path(os.environ.get("LIXINOS_WORKDIR", Path.home() / "Library/Application Support/LixinOS"))
disk = base / "daily.img"
backups = base / "backups"
# Keep the toolchain launchers' runtime defaults when supplying an explicit
# environment to a musl executable. The user can edit every setting in guest.
PROFILE = b"""# LixinOS login environment
export HOME=/home/lixin
export PATH=$HOME/bin:/bin
export EDITOR=nano
export TMPDIR=/tmp
export LC_ALL=C.UTF-8
export COMPILER_PATH=/lib/gcc/bin:/bin
export RUSTC=/bin/rustc
export CARGO_HOME=/.cargo
export CARGO_BUILD_JOBS=1
export CARGO_PROFILE_DEV_DEBUG=0
export CARGO_INCREMENTAL=0
export CARGO_HTTP_CAINFO=/lib/cpython-3.14.5/roots.pem
export PYTHON_BASIC_REPL=1
export _PYTHON_SYSCONFIGDATA_NAME=_lixin_sysconfig
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_PROGRESS_BAR=off
export PIP_ONLY_BINARY=:all:
export PIP_NO_CACHE_DIR=1
export PIP_NO_COMPILE=1
export PIP_ROOT_USER_ACTION=ignore
export SSL_CERT_FILE=/lib/cpython-3.14.5/roots.pem
"""


def personal_home(image):
    def seed(rows):
        stamp = struct.pack("<I", int(time.time())) * 2
        for name in (b"/home", b"/home/lixin", b"/home/lixin/bin"):
            if rows.setdefault(name, (stamp, None))[1] is not None:
                raise ValueError(f"个人目录与已有文件冲突：{name.decode()}")
        if rows.setdefault(b"/home/lixin/.profile", (stamp, PROFILE))[1] is None:
            raise ValueError(".profile 与已有目录冲突")
    edit_files(image, seed)


def record_workdisk():
    rows, _ = disk_entries(disk)
    loader = rows.get("/boot/loader.elf", (None, None))[1]
    target = disk.with_suffix(".loader.elf")
    if loader is not None and bytes(loader[:4]) == b"\x7fELF" and not target.exists():
        with tempfile.NamedTemporaryFile(dir=base) as tmp:
            tmp.write(loader)
            tmp.flush()
            os.fsync(tmp.fileno())
            try:
                os.link(tmp.name, target)
            except FileExistsError:
                pass
    (base / ".daily-configured").touch(mode=0o600, exist_ok=True)


def clone(source, dest, initialize=False):
    if dest.exists():
        raise ValueError(f"目标已存在：{dest}")
    require_local(source, source.stat())
    busy = subprocess.run(["lsof", "-t", "--", str(source)], capture_output=True)
    if busy.returncode not in (0, 1) or busy.stdout or busy.stderr:
        raise ValueError(f"关机后再复制磁盘：{source}")
    before = source.stat()
    dest.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(dir=dest.parent) as temp:
        staging = Path(temp) / "disk.img"
        subprocess.run(["cp", "-c", source, staging], check=True)
        after = source.stat()
        if not os.path.samestat(before, after) or before.st_mtime_ns != after.st_mtime_ns:
            raise ValueError("复制过程中源盘发生变化，请关机后重试")
        disk_entries(staging)  # Verify the active filesystem before publishing the copy.
        if initialize:
            personal_home(staging)
        staging.chmod(0o600)
        os.link(staging, dest)  # Never replace an existing work disk or backup.


def restore(backup, path, replace):
    if not path.startswith("/") or path == "/" or ".." in PurePosixPath(path).parts or str(PurePosixPath(path)) != path:
        raise ValueError("需要 guest 文件的绝对路径")
    require_local(backup, backup.stat())
    rows, _ = disk_entries(backup)
    if path not in rows or rows[path][1] is None:
        raise ValueError("备份里找不到该文件")
    record = rows[path]
    def put(current):
        parent = str(PurePosixPath(path).parent).encode()
        if parent not in current or current[parent][1] is not None:
            raise ValueError("请先在工作盘创建目标父目录")
        key = path.encode()
        if key in current and (current[key][1] is None or not replace):
            raise ValueError("目标已存在；仅当确实要覆盖此文件时使用 --replace")
        current[key] = record
    edit_files(disk, put)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("init").add_argument("source", type=Path)
    sub.add_parser("status")
    sub.add_parser("backup")
    sub.add_parser("setup-home")
    r = sub.add_parser("restore")
    r.add_argument("backup", type=Path)
    r.add_argument("guest_path")
    r.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    if args.action == "init":
        clone(args.source.resolve(strict=True), disk, initialize=True)
        record_workdisk()
        print(disk)
    elif args.action == "setup-home":
        personal_home(disk)
        record_workdisk()
        print(disk)
    elif args.action == "status":
        print("我的工作盘", disk, "已就绪" if disk.is_file() else "未初始化")
        for saved in sorted(backups.glob("*.img")):
            print(saved)
    elif args.action == "backup":
        dest = backups / datetime.now().strftime("%Y%m%d-%H%M%S-%f.img")
        clone(disk, dest)
        print(dest)
    else:
        restore(args.backup.resolve(strict=True), args.guest_path, args.replace)
        print("已恢复", args.guest_path)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
