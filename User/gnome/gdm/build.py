"""Build the local GDM seat backend against upstream libgdm and Linux-PAM."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess

port = Path(__file__).resolve().parent
root = port.parents[2]
cache = root / "Kernel/target/user-build"
gnome = cache/"gnome-port"
py = cache/"cpython-port"
pam = cache/"pam-port"
dyn = cache/"dynlink-port"
work = gnome/"gdm-backend"
pkg = work/"package"
for d in (pkg/"bin", pkg/"lib/gdm"):
    d.mkdir(parents=True,exist_ok=True)
os.environ["PATH"]=str(next(cache.glob("arm-gnu-toolchain-*-aarch64-none-elf"))/"bin")+":"+os.environ["PATH"]
os.environ["PKG_CONFIG_LIBDIR"]=str(gnome/"sysroot/lib/pkgconfig")+":"+str(cache/"gtk3-port/sysroot/lib/pkgconfig")
os.environ["PKG_CONFIG_PATH"]=""
def flags(flag):
    return [s for s in shlex.split(subprocess.check_output(["pkg-config","--static",flag,"gdm"],text=True)) if s!="-pthread"]

# These three SDK archives normally target static applications. Build their
# pointer tables as PIC so the dynamic loader never has to modify executable pages.
cc=py/"cc"
pcre=cache/"lite-xl-port";ffi=gnome/"ffi-cross"
subprocess.run([cc,"-fPIC","-Os","-DPCRE2_STATIC","-DPCRE2_CODE_UNIT_WIDTH=8","-DHAVE_CONFIG_H",
                "-I"+str(pcre/"pcre2-build"),"-I"+str(pcre/"pcre2-pcre2-10.46/src"),"-c",
                pcre/"pcre2-pcre2-10.46/src/pcre2_compile.c","-o",work/"pcre2.o"],check=True)
subprocess.run([cc,"-fPIC","-Os","-DHAVE_CONFIG_H","-I"+str(ffi),"-I"+str(ffi/"include"),
                "-I"+str(gnome/"libffi-3.4.8/include"),"-c",gnome/"libffi-3.4.8/src/types.c",
                "-o",work/"ffi.o"],check=True)
subprocess.run([cc,"-Wall","-Os","-fPIE","-I"+str(pam/"include"),"-I"+str(gnome/"gdm-cross/libgdm"),
                "-I"+str(py/"musl-1.2.5/src/process"),*flags("--cflags"),"-nostdlib",
                "-Wl,-pie,-z,text,-z,max-page-size=4096,-rpath,/lib/pam,--dynamic-linker,/lib/ld-musl-aarch64.so.1",
                py/"sysroot/lib/Scrt1.o",py/"sysroot/lib/crti.o",port/"manager.c",port/"frontend.c",
                root/"User/rust/spawn.c",work/"pcre2.o",work/"ffi.o","-L"+str(dyn/"zlib-pic"),
                "-Wl,--start-group",*flags("--libs"),pam/"package/lib/pam/libpam.so.0",
                dyn/"package/lib/libc.so","-lgcc","-Wl,--end-group",py/"sysroot/lib/crtn.o",
                "-o",pkg/"bin/gdm"],check=True)
subprocess.run(["aarch64-none-elf-strip",pkg/"bin/gdm"],check=True)
shutil.copyfile(port/"system.conf",pkg/"lib/gdm/system.conf")
shutil.copyfile(gnome/"gdm-49.0/COPYING",pkg/"lib/gdm/COPYING")
