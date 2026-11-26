"""Run Debian's ld.so --list on Tencent's ARM64 WeChat inside a disposable LixinOS VM."""
import argparse
import hashlib
import io
import os
import re
from pathlib import Path, PurePosixPath
import runpy
import subprocess
import tarfile
import tempfile

from disk_image import BUILD_HEADER, BUILD_SLOT, HISTORY_BYTES, SECTOR, edit_files

ROOT = Path(__file__).resolve().parents[1]
NATIVE = runpy.run_path(str(ROOT / "scripts/test-native-kernel.py"))
WECHAT_SHA = "b9ee12ba417ff68840d57020ef7014abb23c7b4f4c00ce807672b100c90bc853"
WECHAT = "/opt/wechat/wechat"
LOADER = "/lib/ld-linux-aarch64.so.1"
LIBDIR = "/lib/aarch64-linux-gnu/"
BUNDLED = set("""libandromeda.so libconfService.so libilink2.so libilink_stream.so
libilink_wrapper.so libmmcronet.so libmmmojo.so libowl.so libroam_migration.so
libvoipChannel.so libvoipCodec.so libvoipComm.so""".split())
GLIBC = set("libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 librt.so.1".split())


def digest(path):
    with path.open("rb") as stream:
        h = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
        return h.hexdigest()


def copy_members(rows, deb, members):
    compressed = subprocess.check_output(["tar", "-xOf", deb, "data.tar.xz"])
    with tarfile.open(fileobj=io.BytesIO(compressed), mode="r:xz") as tar:
        for entry in tar:
            name = PurePosixPath(entry.name).as_posix()
            if entry.isfile() and name in members:
                dest = LIBDIR + members.pop(name)
                rows[dest.encode()] = bytes(8), tar.extractfile(entry).read()
    if members:
        raise ValueError(f"missing libraries in {deb.name}: {members}")


def stage_debian(rows, cache, gui=False):
    cache.mkdir(parents=True, exist_ok=True)
    expected = set(GLIBC) | BUNDLED
    lock = ROOT / "User/linux-run/wechat-libs.lock"
    locks = [lock, lock.with_name("wechat-gui-libs.lock")] if gui else [lock]
    for line in (line for file in locks for line in file.read_text().splitlines()):
        if not line or line.startswith("#"):
            continue
        checksum, url, *entries = line.split()
        deb = cache / PurePosixPath(url).name
        if not deb.exists():
            with tempfile.NamedTemporaryFile(dir=cache) as temp:
                subprocess.run(["curl", "-fsSL", "--retry", "2", "https://deb.debian.org/debian/" + url,
                                "-o", temp.name], check=True)
                if digest(Path(temp.name)) != checksum:
                    raise ValueError(f"download checksum mismatch: {deb.name}")
                # A verified file becomes visible atomically, also with concurrent tests.
                try:
                    os.link(temp.name, deb)
                except FileExistsError:
                    pass
        if digest(deb) != checksum:
            raise ValueError(f"cached checksum mismatch: {deb.name}")
        members = {}
        for entry in entries:
            soname, member = entry.split("=")
            expected.add(soname)
            members[member] = soname
        copy_members(rows, deb, members)

    # Keep the loader and its companion libraries on the same glibc build as linux-run.
    glibc = ROOT / "Kernel/target/linux-user/libc6-arm64-cross.deb"
    if digest(glibc) != "91936cbbee75771c360ed16513f6e734a25ca5517fe7c3e13dfd7835d7553186":
        raise ValueError("unexpected glibc package")
    copy_members(rows, glibc, {"usr/aarch64-linux-gnu/lib/" + lib: lib for lib in GLIBC})
    return expected


def stage_wechat(rows, deb, gui=False):
    if digest(deb) != WECHAT_SHA:
        raise ValueError("expected Tencent WeChat 4.1.13.29 arm64")
    rows[b"/opt"] = rows[b"/opt/wechat"] = bytes(8), None
    with subprocess.Popen(["tar", "-xOf", deb, "data.tar.xz"], stdout=subprocess.PIPE) as proc:
        with tarfile.open(fileobj=proc.stdout, mode="r|xz") as payload:
            for member in payload:
                path = PurePosixPath(member.name)
                if path.parent != PurePosixPath("opt/wechat") or not member.isfile():
                    continue
                if (path.name == "wechat" or path.name in BUNDLED or
                        (gui and path.name in {"libcrashpad_client.so", "crashpad_handler"})):
                    rows[("/" + str(path)).encode()] = bytes(8), payload.extractfile(member).read()
        proc.stdout.read()
        if proc.wait() != 0:
            raise ValueError("cannot read Tencent package")
    if len(rows[WECHAT.encode()][1]) != 174290624:
        raise ValueError("unexpected WeChat executable")


def probe(directory, args):
    disk = directory / "test.img"
    with disk.open("xb") as stream:
        stream.write(BUILD_HEADER)
        stream.truncate(SECTOR + 2 * BUILD_SLOT + HISTORY_BYTES)

    def boot(kernel):
        cmd = ["env", f"LIXINOS_TCC_DISK={disk}", ROOT / "scripts/run-kernel.sh",
               "--kernel", kernel, "--release", args.release, "--serial"]
        return NATIVE["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)")

    def shell(serial):
        serial.until(b"$ ")
        serial.expect(b"cd /\r", NATIVE["shell"])

    with boot("rust") as serial:
        shell(serial)
        NATIVE["command"](serial, "mkdir /linux-test")
        NATIVE["shutdown"](serial)

    expected = set()

    def stage(rows):
        for path in args.package.rglob("*"):
            destination = "/" + path.relative_to(args.package).as_posix()
            rows[destination.encode()] = bytes(8), path.read_bytes() if path.is_file() else None
        stage_wechat(rows, args.deb)
        if args.with_debian_libs:
            expected.update(stage_debian(rows, args.cache))

    edit_files(disk, stage)

    for kernel in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
        with boot(kernel) as serial:
            shell(serial)
            command = f"linux-run --trace {LOADER} --list {WECHAT}"
            serial.send(command.encode() + b"\r")
            output = serial.until(NATIVE["shell"], timeout=180)
            print(output.decode(errors="replace"), flush=True)
            assert b"opened /opt/wechat/wechat (174290624 bytes)" in output
            paged = re.search(rb"paged in (\d+) file pages \((\d+) bytes\)", output)
            assert paged and 0 < int(paged[2]) < 174290624, "expected sparse file mappings"
            if args.with_debian_libs:
                mappings = dict(re.findall(rb"^\s*(lib[\w.+-]+) => (/[^ ]+) \(0x[0-9a-f]+\)", output, re.M))
                assert set(mappings) == {name.encode() for name in expected}, mappings
                assert re.search(rb"^\s*/lib/ld-linux-aarch64.so.1 \(0x", output, re.M)
                for name in expected:
                    path = ("/opt/wechat/" if name in BUNDLED else LIBDIR) + name
                    assert mappings.get(name.encode()) == path.encode(), name
                assert b"not found" not in output and b"error while loading" not in output
                serial.expect(b"echo $?\r", NATIVE["shell"], b"\n0\n")
                result = f"{len(expected)} shared libraries resolved, exit 0"
            else:
                assert b"libglib-2.0.so.0: cannot open shared object file" in output
                serial.expect(b"echo $?\r", NATIVE["shell"], b"\n127\n")
                result = "missing libglib-2.0.so.0, exit 127"
            NATIVE["shutdown"](serial)
            print(f"{kernel}: {result}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deb", type=Path, required=True)
    parser.add_argument("--with-debian-libs", action="store_true")
    parser.add_argument("--cache", type=Path, default=ROOT / "Kernel/target/linux-user/wechat-debs")
    parser.add_argument("--package", type=Path, default=ROOT / "Kernel/target/linux-user/package")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lixinos-wechat-") as directory:
        probe(Path(directory), args)


if __name__ == "__main__":
    main()
