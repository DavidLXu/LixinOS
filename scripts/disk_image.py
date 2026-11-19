"""Host-side layout and atomic upgrade of LixinOS raw disk images."""
import argparse
import fcntl
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile

SECTOR = 512
SLOT = 192 << 20
FILE_LIMIT = 100 << 20
PREVIOUS_SLOT = 64 << 20
HISTORY_BYTES = 65 * 1024
HEADER = b"LIXDISK4".ljust(SECTOR, b"\0")
PREVIOUS_HEADER = b"LIXDISK3".ljust(SECTOR, b"\0")
SLOTS = (SECTOR, SECTOR + SLOT)
HISTORY = SECTOR + 2 * SLOT
IMAGE_SIZE = HISTORY + HISTORY_BYTES
OLD_SLOT = 4 << 20
OLD_HISTORY = 2 * OLD_SLOT - HISTORY_BYTES


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def snapshot_slots(source):
    source.seek(0)
    header = source.read(SECTOR)
    size = os.fstat(source.fileno()).st_size
    if header in (HEADER, PREVIOUS_HEADER):
        slot = SLOT if header == HEADER else PREVIOUS_SLOT
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


def prepare(path, previous_layout=False):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        source = path.open("r+b")
    except FileNotFoundError:
        source = None
    try:
        if source:
            header = source.read(SECTOR)
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
    parser.add_argument("--previous-layout", action="store_true")
    args = parser.parse_args()
    try:
        prepare(args.image, args.previous_layout)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Cannot prepare LixinOS disk: {exc}\n")
