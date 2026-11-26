"""Package the image, its notices and only the current generation of segments."""
from pathlib import Path
import runpy
import shutil
import sys
import tempfile

image=Path(sys.argv[1]).resolve()
name=sys.argv[2]
root=Path(__file__).resolve().parents[3]
base=image.parent.parent
recomp=base.parent / "ra2-recomp"
sdk=base.parent
destination=Path(sys.argv[3]).resolve() if len(sys.argv)>3 else base/(name+"-package")
packer=runpy.run_path(str(root/"User/gcc/package.py"))
with tempfile.TemporaryDirectory(dir=base) as tmp:
    stage=Path(tmp)/"package"
    packer["tool"](stage,"/bin/"+name,image,"/lib/"+name,"-DNDEBUG",image.parent,deflate=True)
    lib=stage/"lib"/name
    notices={"SDL-LICENSE":base/"sdl-src/LICENSE.txt"}
    if name=="ra2-engine":
        notices.update({"ENGINE-LICENSE":base/"upstream/LICENSE","THIRD_PARTY_NOTICES.md":base/"upstream/THIRD_PARTY_NOTICES.md"})
    else:
        notices.update({"RECOMP-LICENSE":recomp/"upstream/LICENSE","RECOMP-NOTICE":recomp/"upstream/NOTICE.md",
                        "PCRECOMP-LICENSE":recomp/"pcrecomp/LICENSE","PCRECOMP-NOTICE":recomp/"pcrecomp/NOTICE",
                        "SDL-TTF-LICENSE":recomp/"ttf/LICENSE.txt","FREETYPE-LICENSE":sdk/"lite-xl-port/freetype-VER-2-14-1/docs/FTL.TXT"})
    for filename,src in notices.items():
        shutil.copyfile(src,lib/filename)
    if name=="ra2-recomp-check":
        shutil.copyfile(base/"fixture/fixture.exe",lib/"fixture.exe")
    backup=Path(tmp)/"previous"
    if destination.exists():
        destination.rename(backup)
    try:
        stage.rename(destination)
    except OSError:
        if backup.exists():
            backup.rename(destination)
        raise
print(destination)
