"""Package the statically linked Writer executable and its matching UNO registries."""
from pathlib import Path
import runpy
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "Kernel/target/user-build/libreoffice"
INST = WORK / "guest/instdir"
PACK = runpy.run_path(str(ROOT / "User/gcc/package.py"))["tool"]


def package():
    binary = INST / "program/soffice.bin"
    required = ("program/fundamentalrc", "program/bootstraprc", "program/unorc",
                "program/services.rdb", "program/types.rdb", "program/types/offapi.rdb",
                "program/services/services.rdb", "share/registry/main.xcd")
    for name in required:
        if not (INST / name).is_file():
            raise FileNotFoundError(INST / name)
    with tempfile.TemporaryDirectory(dir=WORK) as tmp:
        stage = Path(tmp) / "package"
        PACK(stage, "/bin/writer.win3", binary, "/lib/writer", "-DNDEBUG", WORK, deflate=True)
        shutil.copyfile(stage / "bin/writer.win3", stage / "bin/writer")
        resources = stage / "lib/writer"
        for path in (INST / "program").rglob("*"):
            if path.is_file() and (path.suffix == ".rdb" or path.name.endswith("rc") or
                                   path.suffix in (".res", ".ini")):
                dest = resources / path.relative_to(INST)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, dest)
        bootstrap = resources / "program/bootstraprc"
        lines = bootstrap.read_text().splitlines()
        bootstrap.write_text("\n".join("UserInstallation=file:///home/lixin/.config/lixinos-writer" if line.startswith("UserInstallation=") else line
                                        for line in lines) + "\n")
        for tree in ("share", "presets"):
            shutil.copytree(INST / tree, resources / tree)
        for name in ("COPYING", "COPYING.LGPL", "COPYING.MPL"):
            shutil.copyfile(WORK / "libreoffice-25.2.7.2" / name, resources / name)
        dest = WORK / "package"
        if dest.exists():
            old = Path(tmp) / "previous"
            dest.rename(old)
            try:
                stage.rename(dest)
            except OSError:
                old.rename(dest)
                raise
        else:
            stage.rename(dest)
    print(dest)


if __name__ == "__main__":
    package()
