"""Link the upstream Writer component set against LixinOS's static GTK and musl."""
from pathlib import Path
from subprocess import check_output, run
from xml.etree import ElementTree as ET
import shlex
import sys

PORT = Path(__file__).resolve().parent
SDK = PORT.parents[1] / "Kernel/target/user-build"
GUEST = SDK / "libreoffice/guest"
SRC = GUEST.parent / "libreoffice-25.2.7.2"
OBJ = GUEST / "writer-link"
ARCHIVES = [GUEST / line for line in (PORT / "archives.txt").read_text().splitlines()]


def targets():
    work = GUEST / "workdir"
    for lib in ARCHIVES:
        if lib.parent == GUEST / "instdir/program":
            yield str(work / "LinkTarget/Library" / (lib.name + ".exports"))
        elif lib.is_relative_to(work / "LinkTarget/StaticLibrary"):
            yield str(lib)
    yield from sorted({"ExternalProject_" + a.relative_to(work / "UnpackedTarball").parts[0]
                       for a in ARCHIVES if a.is_relative_to(work / "UnpackedTarball")})


def registry():
    ns = "http://openoffice.org/2010/uno-components"
    ET.register_namespace("", ns)
    services = GUEST / "instdir/program/services"
    core = {c.get("uri") for c in ET.parse(services.with_suffix(".rdb")).getroot()}
    libraries = {p.name for p in ARCHIVES if p.parent == GUEST / "instdir/program"}
    root = ET.Element("{" + ns + "}components")
    for p in sorted((GUEST / "workdir/ComponentTarget").rglob("*.component")):
        for component in ET.parse(p).getroot():
            if component.get("uri") in libraries - core:
                root.append(component)
    if not any(c.get("uri") == "libswlo.a" for c in root):
        raise ValueError("missing Writer UNO registry")
    services.mkdir(exist_ok=True)
    ET.ElementTree(root).write(services / "services.rdb", encoding="UTF-8", xml_declaration=True)


def link():
    OBJ.mkdir(exist_ok=True)
    native = SDK / "gnome-port"
    cc, cxx = str(native / "cc"), str(native / "cxx")
    macros = ["-DLINUX", "-DUNX", "-DGCC", "-DCPPU_ENV=gcc3", "-DLIBO_INTERNAL_ONLY",
              "-DDISABLE_DYNLOADING", "-D_GNU_SOURCE", "-Os", "-ffunction-sections", "-fdata-sections"]
    headers = [GUEST / "config_host", SRC / "include"]
    headers.extend(GUEST / "workdir/UnoApiHeadersTarget" / group / "normal" for group in ("offapi", "udkapi"))
    flags = macros + ["-I" + str(h) for h in headers]
    extra = OBJ / "ui-constructors.list"
    framework = ET.parse(GUEST / "workdir/ComponentTarget/framework/util/fwk.component")
    # Visible Writer frames instantiate their individual menu, status bar and
    # toolbar controllers through UNO as well as the factories that own them.
    controllers = [node.attrib["constructor"] for node in framework.getroot().iter()
                   if "constructor" in node.attrib]
    extra.write_text("\n".join(controllers) + "\n")
    generated = check_output([sys.executable, SRC / "solenv/bin/native-code.py", "-g", "core",
                              "-g", "edit", "-g", "writer", "-c", extra], text=True)
    # The common upstream edit group also enables Calc's six spreadsheet exporters.
    (OBJ / "maps.cpp").write_text("".join(line for line in generated.splitlines(True)
                                         if "Calc_" not in line))
    def compile(compiler, source, out, extra=()):
        run([compiler, *flags, *extra, "-c", source, "-o", OBJ / out], check=True)
        return OBJ / out
    objects = [
        compile(cc, SRC / "desktop/source/app/main.c", "main.o", ["-DNOTEST_xmlCleanupParser"]),
        compile(cxx, OBJ / "maps.cpp", "maps.o"),
        compile(cc, PORT.parent / "freecad/emutls.c", "emutls.o", ["-DLIBSTDCXX_ONCE_TLS"]),
        compile(cc, PORT.parent / "freecad/once-tls.S", "once.o"),
        compile(cc, PORT / "uno-slots.S", "uno-slots.o"),
        compile(cc, PORT / "readonly.c", "readonly.o"),
        GUEST / "window.o",
    ]
    gtk = shlex.split(check_output([PORT / "pkg-config", "--static", "--libs", "gtk+-3.0"], text=True))
    run([cxx, "-nodefaultlibs", "-Wl,--wrap=__emutls_get_address,--start-group", *objects,
         *ARCHIVES, *gtk, "-lz", "-ldl", "-lstdc++", native / "runtime/rt/liblx.a",
         "-lc", "-lgcc", "-Wl,--end-group", "-o", GUEST / "instdir/program/soffice.bin"], check=True)
    registry()


if __name__ == "__main__":
    if sys.argv[1:] == ["--targets"]:
        print("\n".join(targets()))
    else:
        link()
