"""Package LixinOS FreeCAD components using the existing guest ELF loader."""
from pathlib import Path
import runpy
import shutil

root = Path(__file__).resolve().parents[2]
cache = root / "Kernel/target/user-build"
build = cache / "freecad-port"
package = cache / "freecad-package"
tool = runpy.run_path(str(root / "User/gcc/package.py"))["tool"]
tool(package, "/bin/freecad-sketch-probe", build / "sketch-probe", library="/lib/freecad",
     launcher_flag="-DLX_DBUS", build_dir=build, deflate=True)
source = build / "FreeCAD-4fd3bf320d9566a27e60069fc8387448aaa3a094"
shutil.copy2(source / "LICENSE", package / "lib/freecad/LICENSE-FreeCAD")
tool(package, "/bin/freecad-part.win3", build / "part-window", library="/lib/freecad",
     launcher_flag="-DLX_DBUS", build_dir=build, deflate=True)
for license_name in ("LICENSE_LGPL_21.txt", "OCCT_LGPL_EXCEPTION.txt"):
    shutil.copy2(build / "OCCT-7_9_2" / license_name, package / "lib/freecad" / license_name)
