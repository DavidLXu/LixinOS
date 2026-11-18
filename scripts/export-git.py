#!/usr/bin/env python3
"""Export a LixinOS Git worktree and its .git directory from a disk snapshot."""
import argparse
from pathlib import Path, PurePosixPath
import os
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
from disk_image import checksum, snapshot_headers


def decode(data, versioned):
    cursor = 0

    def take(n):
        nonlocal cursor
        if cursor + n > len(data):
            raise ValueError("truncated snapshot")
        part = data[cursor:cursor + n]
        cursor += n
        return part

    count, = struct.unpack("<I", take(4))
    if count > 1_048_576:
        raise ValueError("invalid entry count")
    files = {}
    for _ in range(count):
        length, size = struct.unpack("<II", take(8))
        if not 0 < length <= 512:
            raise ValueError("invalid path length")
        name = take(length).decode()
        path = PurePosixPath(name)
        if (not name.startswith("/") or name.startswith("//") or str(path) != name
                or ".." in path.parts or "\0" in name or path in files
                or (name != "/" and files.get(path.parent, False) is not None)):
            raise ValueError("invalid directory tree")
        if size != 0xffffffff and size > 1024 * 1024:
            raise ValueError("invalid file size")
        files[path] = None if size == 0xffffffff else take(size)
    if files.get(PurePosixPath("/"), False) is not None or (not versioned and cursor != len(data)):
        raise ValueError("invalid filesystem")
    return files


def export(disk, repo, dest):
    if not repo.is_absolute() or ".." in repo.parts:
        raise ValueError("give the absolute LixinOS repository path")
    with disk.open("rb") as source:
        for generation, base, size, digest, header in snapshot_headers(source):
            versioned = header[:8] == b"LIXINFS2"
            source.seek(base + 512)
            data = source.read(size)
            if len(data) == size and checksum(data) == digest:
                try:
                    files = decode(data, versioned)
                    break
                except ValueError:
                    continue
        else:
            raise ValueError("no valid filesystem snapshot")
    if not isinstance(files.get(repo / ".git/HEAD"), bytes):
        raise ValueError(f"no Git repository at {repo}")
    # Reserve the output name exclusively. On a case-insensitive Mac volume,
    # exclusive creation also detects guest names that would collide.
    dest.mkdir()
    try:
        for path, data in sorted(files.items(), key=lambda x: len(x[0].parts)):
            if path == repo or repo not in path.parents:
                continue
            target = dest / path.relative_to(repo)
            if data is None:
                target.mkdir()
            else:
                with target.open("xb") as stream:
                    stream.write(data)
    except BaseException:
        shutil.rmtree(dest)
        raise
    return generation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=PurePosixPath, help="absolute path in LixinOS")
    parser.add_argument("destination", type=Path, help="new directory on the Mac")
    parser.add_argument("--disk", type=Path, default=os.environ.get("LIXINOS_DISK", ROOT / "data/lixinos.img"))
    args = parser.parse_args()
    try:
        generation = export(args.disk, args.repository, args.destination)
    except (ValueError, OSError) as error:
        parser.exit(1, f"export: {error}\n")
    print(f"Exported snapshot {generation} to {args.destination}")


if __name__ == "__main__":
    main()
