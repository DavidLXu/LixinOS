"""Stage the software-rendered content shell and its ARM64 shared-library dependencies."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

RUNTIME_FILES = ("content_shell", "content_shell.pak", "icudtl.dat", "snapshot_blob.bin")


def install_chinese_font(root, package):
    # Debian fonts-wqy-microhei 0.2.0-beta-3.1 contains both faces in one collection.
    digest = "239163df984eea5c9da3b551aee374936222cdbd67f1a1be3cdcf3d76143ad66"
    if hashlib.sha256(package.read_bytes()).hexdigest() != digest:
        raise ValueError("Unexpected fonts-wqy-microhei package checksum")
    data = subprocess.check_output(["tar", "-xOf", package, "data.tar.xz"])
    members = {
        "./usr/share/fonts/truetype/wqy/wqy-microhei.ttc": "opt/chromium/fonts/wqy-microhei.ttc",
        "./usr/share/doc/fonts-wqy-microhei/copyright": "share/licenses/chromium/WenQuanYi",
    }
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as archive:
        for member, target in members.items():
            dest = root / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as stream, dest.open("wb") as output:
                shutil.copyfileobj(stream, output)
    shutil.copyfile(Path(__file__).with_name("fonts.conf"), root / "opt/chromium/fonts.conf")


def stage(out, dest, guest, library_dirs, readelf, strip=None, font_package=None):
    if dest.exists():
        raise ValueError(f"Choose a new staging directory: {dest}")
    if not os.access(readelf, os.X_OK):
        raise ValueError(f"Missing ELF reader: {readelf}")
    if strip is None:
        strip = readelf.with_name(readelf.name.removesuffix("readelf") + "strip")
    if not os.access(strip, os.X_OK):
        raise ValueError(f"Missing target strip tool: {strip}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
        root = Path(tmp) / "root"
        program = root / "opt/chromium"
        program.mkdir(parents=True)
        for name in RUNTIME_FILES:
            shutil.copy2(out / name, program / name)
        subprocess.run([strip, "--strip-unneeded", program / "content_shell"], check=True)

        pending = [program / "content_shell"]
        seen = set()
        while pending:
            path = pending.pop()
            with path.open("rb") as handle:
                header = handle.read(20)
            if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 183:
                raise ValueError(f"Expected a little-endian AArch64 ELF: {path}")
            dynamic = subprocess.check_output([readelf, "-W", "-S", "-d", path], text=True)
            # Chromium's build sysroot removes code and data from some DSOs,
            # leaving only the symbol tables that the static linker needs.
            if "Section Headers:" in dynamic and not re.search(r"\]\s+\S+\s+PROGBITS\s", dynamic):
                raise ValueError(f"Link-only ELF: {path.name}. Supply the target's runtime library.")
            if path == program / "content_shell":
                rpath = re.search(r"\(RPATH\).*?\[([^\]]+)\]", dynamic)
                if not rpath or "$ORIGIN" not in rpath.group(1).split(":"):
                    raise ValueError("content_shell needs $ORIGIN in DT_RPATH for its private libraries")
            for soname in re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic):
                if soname in seen:
                    continue
                if Path(soname).name != soname:
                    raise ValueError(f"Unexpected library name in {path}: {soname}")
                seen.add(soname)
                base = guest / "lib/aarch64-linux-gnu" / soname
                if base.is_file():
                    pending.append(base)
                    continue
                candidates = [out / soname, guest / "lib" / soname,
                              *(directory / soname for directory in library_dirs)]
                source = next((item for item in candidates if item.is_file()), None)
                if source is None:
                    raise FileNotFoundError(f"{path.name} needs {soname}")
                installed = program / soname
                shutil.copy2(source, installed)
                pending.append(installed)
        if not (guest / "lib/ld-linux-aarch64.so.1").is_file():
            raise FileNotFoundError("Guest runtime is missing /lib/ld-linux-aarch64.so.1")
        if font_package:
            install_chinese_font(root, font_package)
        root.rename(dest)

    files = [p for p in dest.rglob("*") if p.is_file()]
    print(f"Main ELF { (dest / 'opt/chromium/content_shell').stat().st_size:,} bytes")
    print(f"Staged guest files {sum(p.stat().st_size for p in files):,} bytes in {len(files)} files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path, help="Chromium out/LixinOS directory")
    parser.add_argument("dest", type=Path, help="New directory for the LixinOS root overlay")
    parser.add_argument("--guest", required=True, type=Path, help="Existing staged guest runtime root")
    parser.add_argument("--library-dir", action="append", type=Path, default=[],
                        help="Directory of target ARM64 shared libraries, repeatable")
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--strip", type=Path, help="Target strip tool, defaults to the companion of readelf")
    parser.add_argument("--font-package", type=Path, help="Debian fonts-wqy-microhei 0.2.0-beta-3.1 for Chinese fallback")
    args = parser.parse_args()
    stage(args.out, args.dest, args.guest, args.library_dir, args.readelf, args.strip, args.font_package)
