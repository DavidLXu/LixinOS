"""Add the upstream GTK IBus input context to the GTK 3 static SDK."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import json
import os
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
cache = ROOT / "Kernel/target/user-build"
gtk = cache / "gtk3-port"
gnome = cache / "gnome-port"
ibus = gnome / "ibus-cross"
stage = gtk / "gtk-build"
sdk = gtk / "sysroot"
work = gtk / "builtin-ibus"
work.mkdir(exist_ok=True)
os.environ["PKG_CONFIG_LIBDIR"] = str(sdk / "lib/pkgconfig")
os.environ["PKG_CONFIG_PATH"] = ""
os.environ["PATH"] = str(next(cache.glob("arm-gnu-toolchain-*-aarch64-none-elf")) / "bin") + os.pathsep + os.environ["PATH"]
if not (ibus / "compile_commands.json").exists():
    subprocess.run(["bash",ROOT/"User/gnome/build-shell-clients.sh","ibus"],check=True)
sources = gnome / "ibus-1.5.35"
commands = json.loads((ibus/"compile_commands.json").read_text())
units = [u for u in commands if u["output"].startswith("src/libibus-1.0.a.p/")]
gtk_commands = json.loads((stage/"compile_commands.json").read_text())
module = next(u for u in gtk_commands if u["file"].endswith("/gtkimmodule.c"))


def compile_unit(command, cwd, name, source=None, extra=()):
    args = shlex.split(command)
    obj = work / (name+".o")
    for opt,value in (("-o",str(obj)),("-MQ",str(obj)),("-MF",str(obj)+".d")):
        if opt in args:
            args[args.index(opt)+1] = value
    if source:
        args[args.index("-c")+1] = str(source)
    args[0] = str(gtk/"cc")
    args = [a.replace(str(gnome/"sysroot"),str(sdk)) if a.startswith("-I") else a for a in args]
    subprocess.run([*args,*extra],cwd=cwd,check=True)
    return obj


def build_core(unit):
    return compile_unit(unit["command"],ibus,"lxibus-"+Path(unit["output"]).stem)


with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(build_core,units))

original = (stage/module["file"]).read_text()
anchor = "#undef do_builtin"
if original.count(anchor)!=1:
    raise ValueError("GTK builtin registry has changed")
source = work / "gtkimmodule.c"
source.write_text(original.replace(anchor,'  if (g_getenv ("IBUS_ADDRESS") || g_strcmp0 (g_getenv ("GTK_IM_MODULE"), "ibus") == 0)\n    do_builtin (ibus);\n\n'+anchor))
objects.append(compile_unit(module["command"],stage,"gtkimmodule.c",source))
flags = shlex.split(subprocess.check_output(["pkg-config","--cflags","gtk+-3.0"],text=True))
flags += ["-I"+str(sources/"src"),"-I"+str(ibus/"src"),"-I"+str(ibus),"-DHAVE_CONFIG_H"]
renames = ["-Dim_module_"+f+"=_gtk_immodule_ibus_"+f for f in ("create","list","init","exit")]
renames += ["-Dg_module_check_init=lx_ibus_module_check_init","-Dim_get_context_id=lx_ibus_get_context_id"]
for name in ("ibusimcontext","iconwidget","ibusim"):
    obj = work/("lxibus-"+name+".o")
    source = sources/"client/gtk2"/(name+".c")
    if name == "ibusimcontext":
        candidate = work/source.name
        with (ROOT/"User/gtk3/ibus-private-address.patch").open() as patch:
            subprocess.run(["patch","-s","-o",candidate,source],stdin=patch,check=True)
        source = candidate
    subprocess.run([gtk/"cc","-Os","-I"+str(sources/"client/gtk2"),"-ffunction-sections","-fdata-sections",*flags,*renames,"-c",
                    source,"-o",obj],check=True)
    objects.append(obj)
# Replace the original Meson registry object and index all IBus objects in the
# same archive. Applications already linking libgtk-3 acquire the module.
subprocess.run(["aarch64-none-elf-ar","rcs",sdk/"lib/libgtk-3.a",*objects],check=True)
