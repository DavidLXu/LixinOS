from pathlib import Path
import runpy
import shutil
import sys
import tempfile

root, work, package = map(Path, sys.argv[1:])
source = work.parent / "mujoco-4.0.1"
with tempfile.TemporaryDirectory(dir=work) as temporary:
    stage = Path(temporary)
    runpy.run_path(str(root / "User/gcc/package.py"))["tool"](
        stage, "/bin/simulate.win3", work / "simulate", library="/lib/simulate",
        launcher_flag="-DLX_DBUS", build_dir=work, deflate=True)
    assets = stage / "lib/simulate"
    for model in ("humanoid", "slider_crank"):
        shutil.copy(source / "model" / model / (model + ".xml"), assets)
    for name in ("triple.xml", "triple.mjb", "pendulum.xml"):
        shutil.copy(root / "User/mujoco" / name, assets)
    shutil.copy(source / "LICENSE", assets / "LICENSE-MuJoCo")
    shutil.copy(work.parent.parent / "mujoco-package/lib/mujoco/LICENSE-libccd", assets)
    shutil.copy(work.parent.parent / "gnome-port/mesa-24.3.4/licenses/MIT", assets / "LICENSE-Mesa")
    for dep, license in (("qhull","COPYING.txt"),("tinyxml2","LICENSE.txt"),
                         ("tinyobjloader","LICENSE"),("miniz","LICENSE"),("lodepng","LICENSE"),
                         ("MarchingCubeCpp","README.md")):
        shutil.copy(work / "deps" / dep / license, assets / ("LICENSE-" + dep))
    shutil.copytree(stage, package, dirs_exist_ok=True)
    for image in (package / "lib/simulate/images").iterdir():
        if not (assets / "images" / image.name).exists(): image.unlink()
