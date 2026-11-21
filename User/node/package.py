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
args = parser.parse_args()
packager = runpy.run_path(str(port.parent / "gcc/package.py"))
tool = packager["tool"]
if args.package.exists():
    parser.error("choose a new package directory")
args.package.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=args.package.parent) as tmp:
    work = Path(tmp)
    stage = work / "package"
    tool(stage, "/bin/node", args.elf, library="/lib/node", launcher_flag="-DLX_NODE",
         build_dir=work, deflate=True)
    shutil.copyfile(args.source / "LICENSE", stage / "lib/node/LICENSE")
    musl = packager["py"] / "musl-1.2.5/COPYRIGHT"
    shutil.copyfile(musl, stage / "lib/node/COPYRIGHT.musl")
    stage.rename(args.package)
