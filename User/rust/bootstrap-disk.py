"""Prepare a fresh, offline Rust build disk from an installed stage0 and locked sources."""
import argparse
import hashlib
import json
import os
import runpy
from pathlib import Path
import struct
import sys
import tempfile
import tomllib

port = Path(__file__).resolve().parent
root = port.parents[1]
sys.path.insert(0, str(root / "scripts"))
from disk_image import BUILD_HEADER, BUILD_SLOT, HISTORY_BYTES, SECTOR, checksum, disk_entries

work = root / "Kernel/target/rust-port"
stamp = struct.pack("<II", 1, 1)


def seed_entries(seed):
    rows, tail = disk_entries(seed)
    for name in ("/bin/cargo", "/bin/rustc", "/bin/gcc"):
        if name not in rows:
            raise ValueError(f"stage0 lacks {name}")
    for reserved in ("/bootstrap", "/scratch", "/stage1"):
        if reserved in rows:
            raise ValueError(f"stage0 already uses {reserved}")
    return rows, tail


def tree(rows, directory, dest):
    rows[dest] = stamp, None
    for entry in sorted(directory.iterdir()):
        name = dest + "/" + entry.name
        if len(name.encode()) > 512:
            raise ValueError(f"guest path too long: {name}")
        if entry.name == ".git":
            continue
        if entry.is_dir() and not entry.is_symlink():
            tree(rows, entry, name)
        elif entry.is_file():
            rows[name] = stamp, entry


def registry(rows):
    # The local registry only needs archives for packages actually built.
    # The complete locked index also resolves optional and target-only packages.
    cargo = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo")) / "registry"
    cache, = (cargo / "cache").glob("index.crates.io-*")
    index = cargo / "index" / cache.name / ".cache"
    versions = {}
    for lock in (work/"rust-1.100.0/Cargo.lock", work/"cargo-src/Cargo.lock"):
        for pkg in tomllib.loads(lock.read_text())["package"]:
            if "source" not in pkg:
                continue
            if pkg["source"] != "registry+https://github.com/rust-lang/crates.io-index":
                raise ValueError(f"unknown dependency source for {pkg['name']}")
            versions.setdefault(pkg["name"], {})[pkg["version"]] = pkg["checksum"]
    base = "/bootstrap/registry"
    rows[base] = rows[base+"/index"] = stamp, None
    for name, locked in versions.items():
        n = len(name)
        prefix = str(n) if n < 3 else "3/"+name[0] if n == 3 else name[:2]+"/"+name[2:4]
        key = prefix+"/"+name
        parts = (index / key).read_bytes()[5:].split(b"\0")[1:]
        records = []
        for version, line in zip(parts[::2], parts[1::2]):
            v = version.decode()
            if v not in locked:
                continue
            if json.loads(line)["cksum"] != locked[v]:
                raise ValueError(f"index checksum mismatch for {name} {v}")
            records.append(line)
            archive = cache / f"{name}-{v}.crate"
            if archive.exists():
                if hashlib.sha256(archive.read_bytes()).hexdigest() != locked[v]:
                    raise ValueError(f"corrupt archive {archive}")
                rows[base+"/"+archive.name] = stamp, archive
        if len(records) != len(locked):
            raise ValueError(f"missing locked index entries for {name}")
        parent = base + "/index"
        for part in prefix.split("/"):
            parent += "/"+part
            rows[parent] = stamp, None
        rows[base+"/index/"+key] = stamp, b"\n".join(records)+b"\n"


def boot_files(image):
    rows, _ = disk_entries(image)
    files = {key: row for key, row in rows.items()
             if key.startswith("/src/kernel/") or key in ("/src/kernel", "/boot/kernel.elf", "/boot/loader.elf")}
    for path in ("/boot/loader.elf", "/boot/kernel.elf"):
        elf = bytes(files[path][1])
        if not elf.startswith(b"\x7fELF") or b"LIXBLD01" not in elf:
            raise ValueError(f"{image}: {path} lacks TCC build-volume support")
    return files


def prepare(seed, output, tcc_boot_disk=None):
    rows, tail = seed_entries(seed)
    if tcc_boot_disk is not None:
        files = boot_files(tcc_boot_disk)
        rows = {key: row for key, row in rows.items() if not key.startswith("/src/kernel/")}
        rows.setdefault("/boot", (stamp, None))
        rows.update(files)
    rows.setdefault("/.cargo", (stamp, None))
    if "/.cargo/config" not in rows:
        rows.setdefault("/.cargo/config.toml", (stamp, port / "build-config.toml"))
    rows["/bootstrap"] = rows["/scratch"] = rows["/bootstrap/.cargo"] = stamp, None
    rows["/bootstrap/.cargo/config.toml"] = stamp, (
        b'[source.crates-io]\nreplace-with="bootstrap"\n[source.bootstrap]\nlocal-registry="/bootstrap/registry"\n')
    for name in ("bootstrap.rs", "large.ld"):
        rows["/bootstrap/"+name] = stamp, port/name
    rust = work / "rust-1.100.0"
    rows["/bootstrap/rust"] = stamp, None
    for name in ("compiler", "library"):
        tree(rows, rust/name, "/bootstrap/rust/"+name)
    for name in ("Cargo.lock", "Cargo.toml", "LICENSE-APACHE", "LICENSE-MIT", "README.md", "RELEASES.md"):
        rows["/bootstrap/rust/"+name] = stamp, rust/name
    rows["/bootstrap/rust/tests"] = rows["/bootstrap/rust/tests/ui"] = rows["/bootstrap/rust/tests/ui/attributes"] = stamp, None
    tree(rows, rust/"tests/ui/attributes/doc_examples", "/bootstrap/rust/tests/ui/attributes/doc_examples")
    tree(rows, work/"cargo-src", "/bootstrap/cargo")
    tree(rows, work/"tar-0.4.46", "/bootstrap/tar-0.4.46")
    gcc = runpy.run_path(str(root / "User/gcc/package.py"))
    with tempfile.TemporaryDirectory(dir=work) as tools:
        staged = Path(tools)
        for name in ("ar", "ranlib"):
            gcc["tool"](staged, "/bin/"+name, gcc["work"]/("build-binutils/binutils/"+name))
        for entry in staged.rglob("*"):
            path = "/" + entry.relative_to(staged).as_posix()
            if entry.is_file():
                rows[path] = stamp, entry.read_bytes()
    openssl = gcc["py"]/"openssl-3.5.9"
    base = "/bootstrap/openssl"
    rows[base] = rows[base+"/include"] = rows[base+"/lib"] = stamp, None
    tree(rows, openssl/"include/openssl", base+"/include/openssl")
    for name in ("libssl.a", "libcrypto.a"):
        rows[base+"/lib/"+name] = stamp, openssl/name
    registry(rows)
    write_volume(rows, tail, output, with_loader=tcc_boot_disk is not None)


def write_volume(rows, tail, output, *, with_loader=False, history=None):
    loader = output.with_suffix(".loader.elf")
    if output.exists(): raise FileExistsError(output)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".bootstrap-") as tmp:
        tmp.truncate(SECTOR + 2 * BUILD_SLOT + HISTORY_BYTES)
        tmp.write(BUILD_HEADER)
        start = 2 * SECTOR
        tmp.seek(start)
        tmp.write(struct.pack("<I", len(rows)))
        for name,(time,data) in sorted(rows.items()):
            path = name.encode()
            if isinstance(data, Path): data = data.read_bytes()
            tmp.write(struct.pack("<II",len(path),len(data) if data is not None else 0xffffffff))
            tmp.write(time+path)
            if data is not None: tmp.write(data)
        tmp.write(tail)
        size = tmp.tell()-start
        if size > BUILD_SLOT-SECTOR: raise ValueError("build volume full")
        tmp.seek(start)
        digest = checksum(tmp.read(size))
        header = b"LIXINFS3" + struct.pack("<QII",0,size,digest)
        tmp.seek(SECTOR)
        tmp.write((header+struct.pack("<I",checksum(header))).ljust(SECTOR,b"\0"))
        if history is not None:
            if len(history) != HISTORY_BYTES:
                raise ValueError("invalid shell history size")
            tmp.seek(SECTOR + 2 * BUILD_SLOT)
            tmp.write(history)
        tmp.flush()
        os.fsync(tmp.fileno())
        if not with_loader:
            os.link(tmp.name, output)
        else:
            with tempfile.NamedTemporaryFile(dir=loader.parent) as boot:
                boot.write(rows["/boot/loader.elf"][1])
                boot.flush()
                os.fsync(boot.fileno())
                os.link(boot.name, loader)
                try:
                    os.link(tmp.name, output)
                except BaseException:
                    loader.unlink()
                    raise
    print(f"{len(rows)} entries, {size:,} bytes published to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seed",type=Path)
    parser.add_argument("output",type=Path)
    parser.add_argument("--tcc-boot-disk", type=Path, help="copy a build-capable TCC kernel, loader and their sources from this boot disk")
    args = parser.parse_args()
    prepare(args.seed, args.output, args.tcc_boot_disk)
