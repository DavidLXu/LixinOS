"""Bundle Mesa's transitive static objects for external EGL and GLES callers."""
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile

stage, source, prefix = (Path(x).resolve() for x in sys.argv[1:])
command = subprocess.check_output(
    ["ninja", "-t", "commands", "src/egl/lixin-egl-check"], cwd=stage, text=True
).splitlines()[-1]
parts = shlex.split(command)
assert parts[parts.index("-o") + 1] == "src/egl/lixin-egl-check"
archives = list(dict.fromkeys(Path(p) for p in parts
                             if p.endswith(".a") and not Path(p).is_absolute()))
gles = [p for p in archives if p.name == "libGLESv2.a"]
gbm = [p for p in archives if p.name == "libgbm.a"]
assert len(gles) == len(gbm) == 1 and any(p.name == "libEGL.a" for p in archives)

# The MRI script uses paths relative to the Meson build directory.
bundle = "lixinos-libEGL.a"
script = [f"create {bundle}", *(f"addlib {p}" for p in archives if p != gles[0]), "save", "end"]
(stage / bundle).unlink(missing_ok=True)
subprocess.run(["aarch64-none-elf-ar", "-M"], cwd=stage, input="\n".join(script) + "\n",
               text=True, check=True)
libs = prefix / "lib"
shutil.copy2(stage / bundle, libs / "libEGL.a")
shutil.copy2(stage / gles[0], libs / gles[0].name)
shutil.copy2(stage / gbm[0], libs / gbm[0].name)
shutil.copy2(source / "src/gbm/main/gbm.h", prefix / "include/gbm.h")
for name in ("EGL", "GLES2", "GLES3", "KHR"):
    shutil.copytree(source / "include" / name, prefix / "include" / name, dirs_exist_ok=True)

version = (source / "VERSION").read_text().strip()
(libs / "pkgconfig/egl.pc").write_text(f"""prefix=${{pcfiledir}}/../..
libdir=${{prefix}}/lib
includedir=${{prefix}}/include

Name: egl
Description: Mesa EGL with softpipe and VirtIO VirGL for LixinOS
Version: {version}
Requires.private: libdrm zlib
Libs: -L${{libdir}} -lEGL
Libs.private: -lstdc++ -lm
Cflags: -I${{includedir}}
""")
(libs / "pkgconfig/glesv2.pc").write_text(f"""prefix=${{pcfiledir}}/../..
libdir=${{prefix}}/lib
includedir=${{prefix}}/include

Name: glesv2
Description: Mesa OpenGL ES for LixinOS
Version: {version}
Requires.private: egl
Libs: -L${{libdir}} -lGLESv2
Cflags: -I${{includedir}}
""")
(libs / "pkgconfig/gbm.pc").write_text(f"""prefix=${{pcfiledir}}/../..
libdir=${{prefix}}/lib
includedir=${{prefix}}/include

Name: gbm
Description: Mesa Generic Buffer Manager
Version: {version}
Requires.private: egl
Libs: -L${{libdir}} -lgbm
Cflags: -I${{includedir}}
""")

# Package the same statically linked executable for an existing work disk.
pack = runpy.run_path(str(Path(__file__).resolve().parents[1] / "gcc/package.py"))["tool"]
with tempfile.TemporaryDirectory(dir=stage.parent) as temp:
    temp = Path(temp)
    package = temp / "package"
    pack(package, "/bin/lixin-egl-check", stage / "src/egl/lixin-egl-check",
         library="/lib/mesa", launcher_flag="-DLX_DBUS", build_dir=temp, deflate=True)
    destination = stage.parent / "mesa-package"
    if destination.exists():
        shutil.rmtree(destination)
    package.rename(destination)
