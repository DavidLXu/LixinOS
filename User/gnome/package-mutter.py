"""Relink the configured guest compositor with an application's pthread runtime."""
import shlex
import subprocess

def compositor(cache, destination, threads):
    graph = cache / "gnome-port/mutter-cross"
    ninja = cache / "gtk3-port/tools/bin/ninja"
    commands = subprocess.check_output([ninja, "-t", "commands", "src/mutter"],
                                       cwd=graph, text=True)
    link = shlex.split(commands.splitlines()[-1])
    output = link.index("-o") + 1
    assert link[output] == "src/mutter"
    link[output:output+1] = [str(destination), str(threads)]
    subprocess.run(link, cwd=graph, check=True)
    return destination
