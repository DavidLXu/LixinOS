"""Create a sparse paging template. Each boot uses a separate QEMU snapshot."""
import os
from pathlib import Path
import struct
import tempfile

BLOCK = 2 << 20


def create(path, slots=2048, resident_limit=0):
    header = struct.pack("<8sH", b"LXSWAP01", resident_limit).ljust(512, b"\0")
    size = 512 + slots * BLOCK
    path.parent.mkdir(exist_ok=True, parents=True)
    if not path.exists():
        with tempfile.NamedTemporaryFile(dir=path.parent) as tmp:
            tmp.write(header)
            tmp.truncate(size)
            tmp.flush()
            try:
                os.link(tmp.name, path)
            except FileExistsError:
                pass
    with path.open("rb") as source:
        if os.fstat(source.fileno()).st_size != size or source.read(512) != header:
            raise ValueError(f"invalid paging template {path}")
    return path


if __name__ == "__main__":
    print(create(Path.home()/"Library/Caches/LixinOS/swap-4g.raw"))
