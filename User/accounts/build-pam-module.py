"""Build upstream pam_unix with the privileged LixinOS shadow lookup."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

port = Path(__file__).resolve().parent
cache = port.parents[1] / "Kernel/target/user-build"
work = cache / "pam-port"
archive = work / "src.tar.gz"
src = work / "linux-pam-1.7.1"
stage = work / "cross"
if not archive.exists():
    subprocess.run(["curl","-fsSL","--retry","3",
                    "https://codeload.github.com/linux-pam/linux-pam/tar.gz/refs/tags/v1.7.1",
                    "-o",archive.with_suffix(".download")],check=True)
    archive.with_suffix(".download").replace(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != "82aadd97eb697965b577069c12046a4dd1be68361a9978c708698d2a1ee9b6d1":
    raise ValueError("unexpected Linux-PAM source archive")
if not src.exists():
    subprocess.run(["tar","-xf",archive,"-C",work],check=True)
patch = ["patch","-f","-s","-p1","-d",src,"-i",port/"pam-shadow.patch"]
if subprocess.run([*patch,"--dry-run","-R"],stdout=subprocess.DEVNULL).returncode:
    subprocess.run(patch,check=True)
os.environ["PATH"] = str(cache/"gtk3-port/tools/bin")+os.pathsep+os.environ["PATH"]
setup = ["meson","setup",stage,src,"--cross-file",cache/"gtk3-port/cross.ini","--prefix","/usr",
         "-Dauto_features=disabled","-Dpam_unix=enabled","-Dexamples=false","-Dxtests=false","-Dlckpwdf=false"]
if (stage/"build.ninja").exists(): setup.insert(2,"--reconfigure")
subprocess.run(setup,check=True)
objects = []
for command in json.loads((stage/"compile_commands.json").read_text()):
    name = command["output"]
    if not name.startswith(("modules/pam_unix/pam_unix.so.p/","libpam_internal/libpam_internal.a.p/")): continue
    cmd = shlex.split(command["command"])
    subprocess.run([*cmd,"-D__lixinos__","-fPIC"],cwd=stage,check=True)
    objects.append(stage/name)
lib = work/"package/lib/pam"
subprocess.run([cache/"cpython-port/cc","-fPIC","-nostdlib","-shared","-Wl,-z,text,-z,max-page-size=4096",
                "-Wl,--version-script="+str(src/"modules/modules.map"),*objects,lib/"libpam.so.0",
                cache/"dynlink-port/package/lib/libc.so","-lgcc","-o",lib/"pam_unix.so"],check=True)
subprocess.run(["aarch64-none-elf-strip",lib/"pam_unix.so"],check=True)
