"""Host-side layout and atomic upgrade of LixinOS raw disk images."""
import argparse
import fcntl
import os
import re
from pathlib import Path
import shlex
import stat
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
BUILD_HEADER = b"LIXBLD01".ljust(SECTOR, b"\0")
BUILD_SLOT = 4 << 30
SLOT = 192 << 20
FILE_LIMIT = 512 << 20  # maximum on a build disk
PREVIOUS_SLOT = 64 << 20
HISTORY_BYTES = 65 * 1024
HEADER = b"LIXDISK4".ljust(SECTOR, b"\0")
PREVIOUS_HEADER = b"LIXDISK3".ljust(SECTOR, b"\0")
SLOTS = (SECTOR, SECTOR + SLOT)
HISTORY = SECTOR + 2 * SLOT
IMAGE_SIZE = HISTORY + HISTORY_BYTES
OLD_SLOT = 4 << 20
OLD_HISTORY = 2 * OLD_SLOT - HISTORY_BYTES


def cloud_only(info):
    # Darwin's SF_DATALESS is metadata-only. Sparse local images can also
    # have few or no allocated blocks and must remain usable.
    return sys.platform == "darwin" and bool(info.st_flags & getattr(stat, "SF_DATALESS", 0x40000000))


def local_desktop(path):
    if not path.is_file() or not cloud_only(path.stat()):
        return None
    candidates = []
    for image in path.parent.glob("desktop-*.img"):
        try:
            info = image.stat()
        except FileNotFoundError:
            continue
        if stat.S_ISREG(info.st_mode) and info.st_size and not cloud_only(info):
            candidates.append((info.st_mtime_ns, image))
    return max(candidates)[1] if candidates else None


def require_local(path, info):
    if cloud_only(info):
        message = (f"{path} 的内容尚未下载到本机。请在 Finder 中选择「立即下载」后重试，"
                   f"或运行 brctl download {shlex.quote(str(path))}。")
        alternative = local_desktop(path)
        if alternative:
            message += (f"\n要启动另一块有独立文件和历史的本地桌面盘，可在项目根目录运行\n"
                        f"LIXINOS_DISK={shlex.quote(str(alternative))} "
                        "./scripts/run-kernel.sh --kernel rust --release latest --window")
        raise OSError(message)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def build_slot(size):
    for slot in (3 << 30, BUILD_SLOT):
        if size == SECTOR + 2 * slot + HISTORY_BYTES:
            return slot
    raise ValueError("unsupported build volume size")


def snapshot_slots(source):
    source.seek(0)
    header = source.read(SECTOR)
    size = os.fstat(source.fileno()).st_size
    if header in (HEADER, PREVIOUS_HEADER, BUILD_HEADER):
        slot = build_slot(size) if header == BUILD_HEADER else {HEADER: SLOT, PREVIOUS_HEADER: PREVIOUS_SLOT}[header]
        if size < SECTOR + 2 * slot + HISTORY_BYTES:
            raise ValueError("truncated LixinOS disk")
        return [(offset, slot - SECTOR) for offset in (SECTOR, SECTOR + slot)]
    pairs = []
    if size >= 2 * OLD_SLOT + 2 * PREVIOUS_SLOT:
        pairs.extend((offset, PREVIOUS_SLOT - SECTOR)
                     for offset in (2 * OLD_SLOT, 2 * OLD_SLOT + PREVIOUS_SLOT))
    pairs.extend((offset, OLD_SLOT - HISTORY_BYTES - SECTOR) for offset in (0, OLD_SLOT))
    return pairs


def snapshot_headers(source):
    headers = []
    for base, limit in snapshot_slots(source):
        source.seek(base)
        header = source.read(SECTOR)
        if len(header) != SECTOR or header[:8] not in (b"LIXINFS1", b"LIXINFS2", b"LIXINFS3"):
            continue
        generation, size, digest, head_hash = struct.unpack_from("<QIII", header, 8)
        if checksum(header[:24]) == head_hash and 4 <= size <= limit:
            headers.append((generation, base, size, digest, header))
    return sorted(headers, reverse=True)


def migrate(source, target):
    # Keep the newest checksummed snapshot of each parity. A legacy snapshot
    # can supply the fallback while the first generation in the larger slots commits.
    copied = set()
    for generation, base, size, digest, header in snapshot_headers(source):
        slot = generation % 2
        if slot in copied:
            continue
        source.seek(base + SECTOR)
        body = source.read(size)
        if len(body) != size or checksum(body) != digest:
            continue
        target.seek(SLOTS[slot])
        target.write(header)
        target.write(body)
        target.seek(SLOTS[slot])
        if target.read(SECTOR) != header or target.read(size) != body:
            raise OSError("snapshot migration verification failed")
        copied.add(slot)
    if not copied:
        source.seek(0)
        if source.read(SECTOR) not in (PREVIOUS_HEADER, bytes(SECTOR)):
            raise ValueError("nonempty disk has no valid snapshot")
        while chunk := source.read(1 << 20):
            if chunk.count(0) != len(chunk):
                raise ValueError("nonempty disk has no valid snapshot")
    source.seek(0)
    previous = source.read(SECTOR) == PREVIOUS_HEADER
    source.seek(SECTOR + 2 * PREVIOUS_SLOT if previous else OLD_HISTORY)
    history = source.read(HISTORY_BYTES)
    if len(history) != HISTORY_BYTES:
        raise ValueError("truncated legacy disk")
    target.seek(HISTORY)
    target.write(history)
    target.seek(HISTORY)
    if target.read(HISTORY_BYTES) != history:
        raise OSError("history migration verification failed")


def disk_entries(seed):
    # Read an APFS clone or an idle image, never write to the source volume.
    with seed.open("rb") as src:
        headers = snapshot_headers(src)
        if not headers:
            raise ValueError(f"{seed}: no filesystem snapshot")
        _, base, size, digest, hdr = headers[0]
        src.seek(base + SECTOR)
        body = src.read(size)
    if hdr[:8] != b"LIXINFS3" or checksum(body) != digest:
        raise ValueError(f"{seed}: expected a checksummed LIXINFS3 snapshot")
    rows = {}
    pos = 4
    for _ in range(struct.unpack_from("<I", body)[0]):
        n, size = struct.unpack_from("<II", body, pos)
        time = body[pos+8:pos+16]
        pos += 16
        name = body[pos:pos+n].decode()
        pos += n
        data = None
        if size != 0xffffffff:
            data = memoryview(body)[pos:pos+size]
            pos += size
        if pos > len(body) or name in rows:
            raise ValueError(f"{seed}: invalid filesystem records")
        rows[name] = time, data
    return rows, body[pos:]


def boot_preferences(image):
    """Read a bounded launcher hint without loading a multi-GiB guest volume."""
    default = "window Asia/Shanghai rust 2222 auto"
    key = b"/.desktop-boot"
    if not image.is_file() or cloud_only(image.stat()):
        return default
    with image.open("rb") as source:
        headers = snapshot_headers(source)
        if not headers or headers[0][4][:8] != b"LIXINFS3":
            return default
        _, base, size, _, _ = headers[0]
        source.seek(base + SECTOR)
        end = source.tell() + size
        count, = struct.unpack("<I", source.read(4))
        # Skip file payloads and validate each preference before passing it to the launcher.
        for _ in range(min(count, 65536)):
            if source.tell() + 16 > end:
                break
            length, data_size, _, _ = struct.unpack("<IIII", source.read(16))
            n = 0 if data_size == 0xffffffff else data_size
            if not 0 < length <= 512 or source.tell() + length + n > end:
                break
            path = source.read(length)
            if path == key:
                if n > 192:
                    break
                parts = source.read(n).split()
                if len(parts) != 5:
                    break
                mode, zone, kernel, port, model = parts
                if (mode not in (b"window", b"split") or kernel not in (b"rust", b"tcc")
                        or not re.fullmatch(rb"[0-9]{1,5}", port) or int(port) > 65535
                        or not re.fullmatch(rb"[a-zA-Z0-9._-]{1,63}", model)
                        or not re.fullmatch(rb"[a-zA-Z0-9_+-]+(?:/[a-zA-Z0-9_+-]+)*", zone)
                        or len(zone) > 63 or not (Path("/usr/share/zoneinfo") / zone.decode()).is_file()):
                    break
                return b" ".join(parts).decode("ascii")
            source.seek(n, os.SEEK_CUR)
    return default


def edit_files(disk, edit):
    """Apply a file-table change offline, preserving history and opaque version records."""
    require_local(disk, disk.stat())
    with disk.open("r+b") as f:
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        owners = subprocess.run(["lsof","-t","--",str(disk)],capture_output=True)
        if owners.returncode not in (0,1) or owners.stderr or any(int(p)!=os.getpid() for p in owners.stdout.split()):
            raise ValueError(f"{disk} may be in use")
        layout = f.read(SECTOR)
        slot_size = build_slot(os.fstat(f.fileno()).st_size) if layout == BUILD_HEADER else {HEADER: SLOT}.get(layout)
        if slot_size is None:
            raise ValueError("requires a LIXDISK4 or LIXBLD01 volume")
        headers = snapshot_headers(f)
        if not headers:
            raise ValueError(f"{disk}: no filesystem snapshot")
        gen, base, n, digest, header = headers[0]
        f.seek(base+SECTOR)
        data = f.read(n)
        if header[:8] != b"LIXINFS3" or checksum(data) != digest:
            raise ValueError("invalid source snapshot")
        count, = struct.unpack_from("<I",data)
        offset = 4
        rows = {}
        for _ in range(count):
            length,size = struct.unpack_from("<II", data, offset)
            times = data[offset+8:offset+16]
            path = data[offset+16:offset+16+length]
            offset += 16+length
            value = None if size == 0xffffffff else data[offset:offset+size]
            if value is not None:
                offset += size
            if offset>n or path in rows:
                raise ValueError("damaged filesystem records")
            rows[path] = times,value
        tail = data[offset:]
        edit(rows)
        body = bytearray(struct.pack("<I",len(rows)))
        for path,(times,value) in sorted(rows.items()):
            body += struct.pack("<II",len(path),0xffffffff if value is None else len(value))+times+path
            if value is not None:
                body += value
        body += tail
        if body == data:
            return gen
        if len(body)>slot_size-SECTOR:
            raise ValueError("insufficient snapshot capacity")
        target = SECTOR + ((gen+1)%2)*slot_size
        if target == base:
            raise ValueError("invalid snapshot parity")
        commit = b"LIXINFS3"+struct.pack("<QII",gen+1,len(body),checksum(body))
        commit = (commit+struct.pack("<I",checksum(commit))).ljust(SECTOR,b"\0")
        f.seek(target+SECTOR)
        f.write(body)
        f.flush()
        os.fsync(f.fileno())
        f.seek(target+SECTOR)
        if f.read(len(body)) != body:
            raise IOError("failed to verify snapshot before commit")
        f.seek(target)
        f.write(commit)
        f.flush()
        os.fsync(f.fileno())
        return gen + 1


def prepare(path, previous_layout=False):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        require_local(path, path.stat())
        source = path.open("r+b")
    except FileNotFoundError:
        source = None
    try:
        if source:
            require_local(path, os.fstat(source.fileno()))
            header = source.read(SECTOR)
            if header == BUILD_HEADER:
                build_slot(os.fstat(source.fileno()).st_size)
                if previous_layout:
                    raise ValueError("invalid or incompatible dedicated build volume")
                return
            if previous_layout and header == HEADER:
                raise ValueError("this kernel needs a LIXDISK4-compatible loader")
            if header == (PREVIOUS_HEADER if previous_layout else HEADER):
                if os.fstat(source.fileno()).st_size < (SECTOR + 2 * PREVIOUS_SLOT + HISTORY_BYTES if previous_layout else IMAGE_SIZE):
                    raise ValueError("truncated LixinOS disk")
                return
            fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # macOS QEMU builds can use different advisory-lock backends.
            # Avoid replacing a live VM's inode even if lockf succeeded.
            owners = subprocess.run(["lsof", "-t", "--", str(path)], capture_output=True)
            if owners.returncode not in (0, 1) or owners.stderr:
                raise OSError("could not check whether the disk is in use")
            if any(int(pid) != os.getpid() for pid in owners.stdout.split()):
                raise OSError("disk is in use; shut down its VM before upgrading")
            original = os.fstat(source.fileno())
        if previous_layout and source:
            raise ValueError("upgrade the kernel before migrating this disk")
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".lixinos-", delete=False) as tmp:
            name = Path(tmp.name)
            try:
                tmp.truncate(SECTOR + 2 * PREVIOUS_SLOT + HISTORY_BYTES if previous_layout else IMAGE_SIZE)
                tmp.write(PREVIOUS_HEADER if previous_layout else HEADER)
                if source:
                    migrate(source, tmp)
                    os.fchmod(tmp.fileno(), stat.S_IMODE(original.st_mode))
                tmp.flush()
                os.fsync(tmp.fileno())
                if source:
                    current = path.stat()
                    if not os.path.samestat(original, current) or current.st_mtime_ns != original.st_mtime_ns:
                        raise OSError("source image changed during migration")
                    os.replace(name, path)
                else:
                    try:
                        os.link(name, path)
                    except FileExistsError:
                        raise OSError("another launcher created the image; retry")
                directory = os.open(path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            finally:
                name.unlink(missing_ok=True)
    finally:
        if source:
            source.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    options = parser.add_mutually_exclusive_group()
    options.add_argument("--boot-preferences", action="store_true")
    options.add_argument("--previous-layout", action="store_true")
    options.add_argument("--local-alternative", action="store_true",
                         help="print a resident desktop disk if the requested disk is cloud-only")
    args = parser.parse_args()
    try:
        if args.boot_preferences:
            print(boot_preferences(args.image))
        elif args.local_alternative:
            print(local_desktop(args.image.resolve()) or "")
        else:
            prepare(args.image, args.previous_layout)
    except KeyboardInterrupt:
        parser.exit(130, "已中断磁盘准备。\n")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Cannot prepare LixinOS disk: {exc}\n")
