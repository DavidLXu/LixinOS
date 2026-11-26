"""Stage the translated game and import an installed SE data directory."""
from pathlib import Path
import argparse
import runpy
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "Kernel/target/user-build/nfs2-port"


def import_game(source, target):
    required = {"fedata", "gamedata", "install.win"}
    children = {p.name.lower(): p for p in source.iterdir()}
    if not required <= children.keys():
        raise ValueError("Expected FEDATA, GAMEDATA and install.win in the game directory")
    target.mkdir(parents=True)
    for name in sorted(required):
        entry = children[name]
        for item in [entry, *sorted(entry.rglob("*"))]:
            dest = target / item.relative_to(source).as_posix().lower()
            if item.is_symlink() or dest.exists():
                raise ValueError(f"Ambiguous or linked game asset: {item}")
            if item.is_dir():
                dest.mkdir()
            else:
                shutil.copyfile(item, dest)

    # The Glide-only wrapper leaves the software track slots empty. The
    # original software renderer reads the horizon and textures from TRACKS/PC.
    install = target / "install.win"
    lines = install.read_bytes().splitlines(keepends=True)
    for index in (5, 6):
        if index < len(lines) and lines[index].rstrip(b"\r\n") == b"3rn":
            lines[index] = lines[index].replace(b"3rn", b"3rn.\\gamedata\\tracks\\pc\\")
    install.write_bytes(b"".join(lines))


def package(source):
    with tempfile.TemporaryDirectory(dir=BASE) as tmp:
        stage = Path(tmp) / "package"
        packer = runpy.run_path(str(ROOT / "User/gcc/package.py"))
        packer["tool"](stage, "/bin/nfs2", BASE / "build/nfs2", "/lib/nfs2",
                       "-DNDEBUG", BASE / "build", deflate=True)
        lib = stage / "lib/nfs2"
        import_game(source, lib / "game")
        # The original program looks up its EXE on the CD and maps the audio DLL.
        for name in ("nfs2sen.exe", "eacsnd.dll"):
            shutil.copyfile(BASE / "recomp/nfs2se" / name, lib / "game" / name)
        shutil.copyfile(BASE / "sse2neon/LICENSE", lib / "SSE2NEON-LICENSE")
        shutil.copyfile(BASE.parent / "ra2-engine-port/sdl-src/LICENSE.txt", lib / "SDL-LICENSE")
        current = BASE / "package"
        previous = Path(tmp) / "previous"
        if current.exists():
            current.rename(previous)
        try:
            stage.rename(current)
        except OSError:
            if previous.exists():
                previous.rename(current)
            raise
    return current


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    print(package(parser.parse_args().game))
