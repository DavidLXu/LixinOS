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
SLOT = 64 << 20
HISTORY_BYTES = 65 * 1024
HEADER = b"LIXDISK3".ljust(SECTOR, b"\0")
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
    if source.read(SECTOR) == HEADER:
        if os.fstat(source.fileno()).st_size < IMAGE_SIZE:
            raise ValueError("truncated LixinOS disk")
        return [(offset, SLOT - SECTOR) for offset in SLOTS]
    pairs = []
    if os.fstat(source.fileno()).st_size >= 2 * OLD_SLOT + 2 * SLOT:
        pairs.extend((offset, SLOT - SECTOR) for offset in (2 * OLD_SLOT, 2 * OLD_SLOT + SLOT))
    pairs.extend((offset, OLD_SLOT - HISTORY_BYTES - SECTOR) for offset in (0, OLD_SLOT))
    return pairs


def snapshot_headers(source):
    headers = []
    for base, limit in snapshot_slots(source):
        source.seek(base)
        header = source.read(SECTOR)
        if len(header) != SECTOR or header[:8] not in (b"LIXINFS1", b"LIXINFS2"):
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
        while chunk := source.read(1 << 20):
            if chunk.count(0) != len(chunk):
                raise ValueError("nonempty disk has no valid snapshot")
    source.seek(OLD_HISTORY)
    history = source.read(HISTORY_BYTES)
    if len(history) != HISTORY_BYTES:
        raise ValueError("truncated legacy disk")
    target.seek(HISTORY)
    target.write(history)
    target.seek(HISTORY)
    if target.read(HISTORY_BYTES) != history:
        raise OSError("history migration verification failed")


def prepare(path):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        source = path.open("r+b")
    except FileNotFoundError:
        source = None
    try:
        if source:
            if source.read(SECTOR) == HEADER:
                if os.fstat(source.fileno()).st_size < IMAGE_SIZE:
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
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".lixinos-", delete=False) as tmp:
            name = Path(tmp.name)
            try:
                tmp.truncate(IMAGE_SIZE)
                tmp.write(HEADER)
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
    try:
        prepare(parser.parse_args().image)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Cannot prepare LixinOS disk: {exc}\n")
