"""Build the upstream IBus service, Compose engine and a native input client."""
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess

port = Path(__file__).resolve().parent
root = port.parents[1]
cache = root/"Kernel/target/user-build"
build = cache/"gnome-port"
stage = build/"ibus-cross"
src = build/"ibus-1.5.35"
work = build/"ibus-runtime"
work.mkdir(exist_ok=True)
os.environ["PATH"] = os.pathsep.join([str(cache/"gtk3-port/tools/bin"),
    str(next(cache.glob("arm-gnu-toolchain-*-aarch64-none-elf"))/"bin"),os.environ["PATH"]])
subprocess.run(["bash",port/"build-shell-clients.sh","ibus"],check=True)
# The release package provides the public Vala bindings. Compile main.vala
# against these bindings, and link it against the port's own libibus.
for filename in ("ibus-1.0.vapi","ibus-1.0.deps"):
    with (work/filename).open("wb") as f:
        subprocess.run(["tar","-xOf",build/"ibus-dev-1.5.33-r0.apk","usr/share/vala/vapi/"+filename],stdout=f,check=True)
subprocess.run(["valac","-C","--directory",work,"--basedir",src/"engine","--vapidir",work,
                "--pkg","ibus-1.0",src/"engine/main.vala"],check=True)
commands=json.loads((stage/"compile_commands.json").read_bytes())
cmd=shlex.split(next(c["command"] for c in commands if c["file"].endswith("/bus/main.c")))
link=shlex.split(subprocess.check_output(["ninja","-t","commands","bus/ibus-daemon"],cwd=stage,text=True).splitlines()[-1])
subprocess.run(link,cwd=stage,check=True)
lib=link[link.index("-Wl,--as-needed"):]
package=build/"ibus-package"
tool=runpy.run_path(str(port.parent/"gcc/package.py"))["tool"]
for name,source in (("ibus-daemon",None),("ibus-engine-simple",work/"main.c"),("ibus-input",port/"ibus-input.c")):
    elf=stage/"bus/ibus-daemon"
    if source:
        obj=work/(name+".o")
        compile=cmd.copy()
        for opt,val in [("-o",obj),("-c",source),("-MF",str(obj)+".d")]:
            compile[compile.index(opt)+1]=str(val)
        subprocess.run(compile,cwd=stage,check=True)
        elf=work/(name+".elf")
        subprocess.run([link[0],"-o",elf,obj,*lib],cwd=stage,check=True)
    tool(package,"/bin/"+name,elf,library="/lib/ibus/"+name,launcher_flag="-DLX_DBUS",build_dir=work,deflate=True)
component=package/"lib/ibus/component"
component.mkdir(exist_ok=True)
(component/"simple.xml").write_text((src/"engine/simple.xml.in").read_text().replace("@libexecdir@","/bin"))
(package/"lib/ibus/COPYING").write_bytes((src/"COPYING").read_bytes())

launcher=work/"ibus-run.elf"
subprocess.run([build/"cc","-Os","-I"+str(cache/"cpython-port/musl-1.2.5/src/process"),
                port/"startgnome.c",root/"User/rust/spawn.c","-o",launcher],check=True)
tool(package,"/bin/ibus-run",launcher,library="/lib/ibus/ibus-run",
     launcher_flag="-DLX_DBUS",build_dir=work,deflate=True)
