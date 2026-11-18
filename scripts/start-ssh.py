"""Install the console transport in a private build environment on first use."""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys

scripts = Path(__file__).resolve().parent
target = scripts.parent / "Kernel" / "target"
python = target / "ssh-venv" / "bin" / "python"
version = "2.24.1"

def ssh_python():
    target.mkdir(parents=True, exist_ok=True)
    with (target / "ssh-setup.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not python.exists():
            candidates = [sys.executable] + [shutil.which(f"python3.{n}") for n in range(14, 9, -1)]
            for candidate in filter(None, candidates):
                if subprocess.call([candidate, "-c", "import sys; sys.exit(sys.version_info < (3, 10))"]) == 0:
                    subprocess.run([candidate, "-m", "venv", python.parents[1]], check=True)
                    break
            else:
                sys.exit("SSH console requires Python 3.10 or newer (brew install python).")
        probe = f"import asyncssh,sys; sys.exit(asyncssh.__version__ != '{version}')"
        if subprocess.call([python, "-c", probe], stderr=subprocess.DEVNULL):
            subprocess.run([python, "-m", "pip", "install", f"asyncssh=={version}"], check=True)
    return python


if __name__ == "__main__":
    exe = ssh_python()
    os.execv(exe, [str(exe), str(scripts / "ssh-console.py"), *sys.argv[1:]])
