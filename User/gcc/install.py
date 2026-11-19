"""Install a native toolchain into an idle LixinOS disk via its inactive snapshot."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"scripts"))
from disk_image import BUILD_HEADER, HEADER, SECTOR, SLOT, build_slot, checksum, snapshot_headers

PACKAGES = {
    "gcc": ("/bin/gcc", "/bin/as", "/bin/ld"),
    "rust": ("/bin/rustc", "/bin/cargo"),
}


def install(package, disk, name="gcc"):
    library = "/lib/" + name
    binaries = PACKAGES[name]
    manifest_path = (library + "/installed.json").encode()

    def tool_path(path):
        return ((path in binaries or path.startswith(library + "/"))
                and ".." not in Path(path).parts and str(Path(path)) == path
                and path.encode() != manifest_path)

    package = package.resolve(strict=True)
    with disk.open("r+b") as f:
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        owners = subprocess.run(["lsof","-t","--",str(disk)],capture_output=True)
        if owners.returncode not in (0,1) or owners.stderr or any(int(p)!=os.getpid() for p in owners.stdout.split()):
            raise ValueError(f"{disk} may be in use")
        layout = f.read(SECTOR)
        slot_size = build_slot(os.fstat(f.fileno()).st_size) if layout == BUILD_HEADER else {HEADER: SLOT}.get(layout)
        if slot_size is None:
            raise ValueError("requires a LIXDISK4 or LIXBLD01 volume")
        gen, base, n, digest, header = snapshot_headers(f)[0]
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
        now = struct.pack("<I",int(time.time()))
        manifest = json.loads(rows.get(manifest_path,(None,b"{}"))[1])
        if not isinstance(manifest, dict) or any(not tool_path(key) or not isinstance(value, str)
                                                for key,value in manifest.items()):
            raise ValueError("invalid installed package manifest")
        fresh = {}
        def put(path, value):
            times,old = rows.get(path,(now*2,None))
            if path in rows and old == value:
                return
            if path in rows and path != manifest_path and (old is None or manifest.get(path.decode()) != hashlib.sha256(old).hexdigest()):
                raise ValueError(f"local file conflicts with package: {path!r}")
            rows[path] = times[:4]+now,value
        for item in sorted(package.rglob("*")):
            path = "/"+item.relative_to(package).as_posix()
            if path not in ("/lib",library,"/bin","/tmp") and not tool_path(path):
                raise ValueError(f"unexpected package path {path}")
            value = None if item.is_dir() else item.read_bytes()
            put(path.encode(),value)
            if value is not None:
                fresh[path] = hashlib.sha256(value).hexdigest()
        for path in manifest.keys()-fresh.keys():
            prior = rows.get(path.encode())
            if prior and prior[1] is not None and hashlib.sha256(prior[1]).hexdigest()==manifest[path]:
                del rows[path.encode()]
        put(manifest_path,json.dumps(fresh,sort_keys=True).encode())
        body = bytearray(struct.pack("<I",len(rows)))
        for path,(times,value) in sorted(rows.items()):
            body += struct.pack("<II",len(path),0xffffffff if value is None else len(value))+times+path
            if value is not None:
                body += value
        body += tail
        if body == data:
            return
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
            raise IOError("failed to verify package before commit")
        f.seek(target)
        f.write(commit)
        f.flush()
        os.fsync(f.fileno())
        print(f"{disk}: installed {len(fresh)} files, generation {gen+1}", flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package",type=Path)
    parser.add_argument("disk",type=Path)
    parser.add_argument("--toolchain", choices=PACKAGES, default="gcc")
    args=parser.parse_args()
    install(args.package,args.disk,args.toolchain)
