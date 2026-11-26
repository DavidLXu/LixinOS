"""Install the RA2 ports together, optionally importing a local retail copy."""
import argparse
import os
from pathlib import Path
import runpy
import shutil
import tempfile

ROOT=Path(__file__).resolve().parents[3]
CACHE=ROOT/"Kernel/target/user-build/ra2-engine-port"
installer=runpy.run_path(str(ROOT/"User/gcc/install.py"))

def install(disk, retail=None):
    names=["ra2-engine","ra2-recomp","ra2-recomp-check"]
    if (CACHE/"yr-recomp-package").is_dir():
        names.append("yr-recomp")
    edits=[installer["package_edit"](CACHE/(name+"-package"),name) for name in names]
    with tempfile.TemporaryDirectory(prefix="lixin-ra2-content-") as tmp:
        if retail:
            package=Path(tmp)/"package"
            dest=package/"lib/ra2-content"
            dest.mkdir(parents=True)
            # Import the retail root, including audio, mission archives and movies.
            seen=set()
            for file in retail.iterdir():
                if file.is_symlink() or not file.is_file() or file.name.startswith("."):
                    continue
                lower=file.name.lower()
                if lower in seen:
                    raise ValueError("Ambiguous Windows filename: "+file.name)
                seen.add(lower)
                # The upstream POSIX file opens use these retail spellings.
                name={"maps01.mix":"Maps01.mix","maps02.mix":"MAPS02.MIX"}.get(lower,lower)
                try: os.link(file,dest/name)
                except OSError: shutil.copyfile(file,dest/name)
            files={p.name.lower() for p in dest.iterdir()}
            if "ra2.mix" not in files:
                raise ValueError("Selected folder is missing ra2.mix")
            missing={"ra2md.mix","language.mix","multimd.mix"}-files
            if missing:
                print("RA2 Engine still needs:",", ".join(sorted(missing)))
            edits.append(installer["package_edit"](package,"ra2-content"))
        def update(rows):
            for edit in edits: edit(rows)
        return installer["edit_files"](disk,update)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk",type=Path)
    parser.add_argument("--retail",type=Path)
    args=parser.parse_args()
    print("Installed RA2 packages in snapshot",install(args.disk,args.retail))
