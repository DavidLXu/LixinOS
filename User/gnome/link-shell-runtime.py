"""Link GNOME Shell with the target functions exposed by its typelibs."""
import ast
import configparser
import os
from pathlib import Path
import shlex
import subprocess
import sys

from importlib import import_module

registry = import_module("gen-static-modules")
PORT = Path(__file__).resolve().parent
BUILD = PORT.parent.parent / "Kernel/target/user-build/gnome-port"
STAGE = BUILD / "shell-cross"
METADATA = BUILD / "shell-native-gi"
CLIENTS = (
    "accountsservice",
    "gdm",
    "gnome-bg-4",
    "gweather4",
    "ibus-1.0",
    "libgeoclue-2.0",
    "upower-glib",
)


def archive_paths(flags):
    directories = [Path(arg[2:]) for arg in flags if arg.startswith("-L")]
    paths = []
    for arg in flags:
        if arg.endswith(".a"):
            paths.append(STAGE / arg)
        elif arg.startswith("-l"):
            for directory in directories:
                archive = STAGE / directory / f"lib{arg[2:]}.a"
                if archive.is_file():
                    paths.append(archive)
                    break
    archives = set()
    for archive in paths:
        if archive.is_file():
            with archive.open("rb") as stream:
                if stream.read(8) in (b"!<arch>\n", b"!<thin>\n"):
                    archives.add(archive.resolve())
    return sorted(archives)


def link_runtime():
    cross = configparser.ConfigParser()
    cross.read(BUILD / "cross.ini")
    env = {**os.environ, "PKG_CONFIG_LIBDIR": ast.literal_eval(cross["properties"]["pkg_config_libdir"])}
    env.pop("PKG_CONFIG_PATH", None)
    extra = shlex.split(subprocess.check_output(
        ["pkg-config", "--static", "--libs", *CLIENTS], env=env, text=True
    ))
    commands = subprocess.check_output(["ninja", "-t", "commands", "src/gnome-shell"],
                                       cwd=STAGE, text=True)
    link = shlex.split(commands.splitlines()[-1])
    output = link.index("-o") + 1
    if link[output] != "src/gnome-shell":
        sys.exit("Unexpected Ninja entry for gnome-shell")
    link[output] = "src/gnome-shell-lixinos"
    group_end = link.index("-Wl,--end-group")
    link[group_end:group_end] = extra

    modules = METADATA / "modules.c"
    object_file = modules.with_suffix(".o")
    registry.generate("aarch64-none-elf-nm", archive_paths(link),
                      sorted((METADATA / "gir").glob("*.gir")), modules)
    subprocess.run([BUILD / "cc", "-Os", "-ffunction-sections", "-fdata-sections",
                    "-I", PORT, "-c", modules, "-o", object_file], check=True)
    link.insert(output + 1, str(object_file))
    subprocess.run(link, cwd=STAGE, check=True)


if __name__ == "__main__":
    link_runtime()
