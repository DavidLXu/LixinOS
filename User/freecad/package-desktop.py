"""Stage upstream FreeCAD, its selected workbenches and guest runtime resources."""
from pathlib import Path
import os
import runpy
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
cache = root / "Kernel/target/user-build"
build = cache / "freecad-port"
desktop = build / "original-desktop"
package = build / "desktop-package"
tool = runpy.run_path(str(root / "User/gcc/package.py"))["tool"]
with tempfile.TemporaryDirectory(dir=build) as temp:
    work = Path(temp)
    stage = work / "package"
    tool(stage, "/lib/freecad-desktop/FreeCAD", desktop / "bin/FreeCAD", library="/lib/freecad-desktop",
         launcher_flag="-DLX_DBUS", build_dir=work, deflate=True)
    launcher = work / "startfreecad"
    subprocess.run([cache / "gnome-port/cc", "-Os", "-ffunction-sections", "-fdata-sections",
                    "-I", cache / "cpython-port/musl-1.2.5/src/process",
                    root / "User/freecad/startfreecad.c", root / "User/rust/spawn.c",
                    "-o", launcher], check=True)
    relink = runpy.run_path(str(root / "User/gnome/package-mutter.py"))["compositor"]
    mutter = relink(cache, work / "mutter", build / "freecad-threads.o")
    tool(stage, "/lib/freecad-desktop/mutter", mutter,
         library="/lib/freecad-desktop", launcher_flag="-DLX_DBUS", build_dir=work, deflate=True)
    tool(stage, "/bin/freecad", launcher, library="/lib/freecad-desktop",
         launcher_flag="-DLX_DBUS", build_dir=work, deflate=True)
    shutil.copy2(stage / "bin/freecad", stage / "bin/startfreecad")
    home = stage / "lib/freecad-desktop"
    # Let upstream choose the Python modules and resources belonging to each workbench.
    install_root = work / "install"
    for target in ("App", "Gui", "Gui/Stylesheets", "Mod/Part/Gui", "Mod/Sketcher/Gui",
                   "Mod/Material/Gui", "Mod/Measure/Gui", "Mod/Part", "Mod/Sketcher", "Mod/Measure", "Mod/Material"):
        subprocess.run(["cmake", "-DCMAKE_INSTALL_PREFIX=/", "-DCMAKE_INSTALL_LOCAL_ONLY=1",
                        "-P", desktop / "src" / target / "cmake_install.cmake"],
                       env={**os.environ, "DESTDIR": str(install_root)}, check=True)
    for name in ("Mod", "share", "Ext"):
        if (install_root / name).is_dir():
            shutil.copytree(install_root / name, home / name, dirs_exist_ok=True)
    shutil.copytree(desktop / "Ext", home / "Ext", dirs_exist_ok=True)
    stdlib = cache / "cpython-port/package/lib/python3.14"
    shutil.copytree(stdlib, home / "lib/python3.14",
                    ignore=shutil.ignore_patterns("site-packages", "__pycache__"))
    (home / "lib/python3.14/site-packages").mkdir()
    shutil.copytree(build / "pivy-build/pivy", home / "lib/python3.14/site-packages/pivy",
                    ignore=shutil.ignore_patterns("__pycache__"))
    site = home / "lib/python3.14/site-packages"
    shiboken = build / "pyside-sdk/lib/python3.14/site-packages/shiboken6"
    shutil.copytree(shiboken, site / "shiboken6", ignore=shutil.ignore_patterns("*.a", "*.pyi", "__pycache__"))
    pyside = build / "pyside-build/PySide6"
    (site / "PySide6").mkdir()
    for python in pyside.glob("*.py"):
        shutil.copy2(python, site / "PySide6" / python.name)
    shutil.copytree(pyside / "support", site / "PySide6/support")
    fonts = stage / "lib/freecad-desktop/fonts"
    fonts.mkdir()
    shutil.copy2(cache / "gtk3-port/package/lib/gtk3/fonts/FiraSans-Regular.ttf", fonts)
    shutil.copy2(root / "User/freecad/test-workbenches.FCMacro", stage / "lib/freecad-desktop")
    # Replace only this generated package after staging has succeeded.
    if package.exists():
        shutil.rmtree(package)
    stage.rename(package)
