"""Build a compressed Node launcher package from a statically linked guest ELF."""
import argparse
from pathlib import Path
import runpy
import shutil
import tempfile

port = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("elf", type=Path)
parser.add_argument("source", type=Path, help="extracted upstream Node.js source tree")
parser.add_argument("package", type=Path, help="new output directory")
parser.add_argument("--dynamic", action="store_true", help="install a PIE linked with the guest shared musl")
parser.add_argument("--command", choices=("node", "node26"), default="node")
args = parser.parse_args()
packager = runpy.run_path(str(port.parent / "gcc/package.py"))
tool = packager["tool"]
if args.package.exists():
    parser.error("choose a new package directory")
args.package.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=args.package.parent) as tmp:
    work = Path(tmp)
    stage = work / "package"
    library = stage / "lib" / args.command
    if args.dynamic:
        import struct
        with args.elf.open("rb") as stream:
            header = stream.read(64)
        if header[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", header, 16) != (3, 183):
            parser.error("expected an AArch64 position-independent executable")
        (stage/"bin").mkdir(parents=True)
        library.mkdir(parents=True)
        shutil.copyfile(args.elf, stage/"bin"/args.command)
    else:
        tool(stage, "/bin/" + args.command, args.elf, library="/lib/" + args.command, launcher_flag="-DLX_NODE",
         build_dir=work, deflate=True)
    shutil.copyfile(args.source / "LICENSE", library / "LICENSE")
    musl = packager["py"] / "musl-1.2.5/COPYRIGHT"
    shutil.copyfile(musl, library / "COPYRIGHT.musl")
    stage.rename(args.package)
