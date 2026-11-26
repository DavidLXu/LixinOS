"""Stage the system fonts, upstream licenses, Fontconfig defaults and guest CLI."""
from pathlib import Path
import runpy
import shutil
import tempfile

port = Path(__file__).resolve().parent
root = port.parents[1]
cache = root / "Kernel/target/user-build"
work = cache / "fonts-port"
gtk = cache / "gtk3-port/package/lib/gtk3"
tool = runpy.run_path(str(root / "User/gcc/package.py"))["tool"]
with tempfile.TemporaryDirectory(dir=work) as tmp:
    stage = Path(tmp) / "package"
    tool(stage, "/bin/fonts", work / "fonts", library="/lib/fonts",
         launcher_flag="-DLX_DBUS", build_dir=Path(tmp), deflate=True)
    fonts = stage / "usr/share/fonts"
    fonts.mkdir(parents=True)
    for file in ("FiraSans-Regular.ttf", "JetBrainsMono-Regular.ttf", "NotoSansMonoCJKsc-Regular.otf"):
        shutil.copy2(gtk / "fonts" / file, fonts / file)
    conf = stage / "etc/fonts"
    conf.mkdir(parents=True)
    shutil.copy2(port / "fonts.conf", conf)
    licenses = stage / "lib/fonts/licenses"
    licenses.mkdir()
    for file in ("Bundled-fonts.md", "Noto"):
        shutil.copy2(gtk / "licenses" / file, licenses)
    dest = work / "package"
    if dest.exists():
        shutil.rmtree(dest)
    stage.rename(dest)
