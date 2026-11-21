"""Exercise the installed App Store on an offline disk clone with a fixture model."""
import argparse
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SERIAL = runpy.run_path(str(HERE / "test-user.py"))


def reply(instruction, data):
    if instruction.startswith("Suggest three"):
        return {"tool": "finish", "args": ["用 K 键切换开关", "画一颗跳动的星星", "做一张翻面的单词卡"]}
    steps = [
        ("apply_patch", ["source", "e.value == ' '", "(e.value == ' ' || e.value == 'k')"]),
        ("apply_patch", ["tests", "          32\n", "          107\n"]),
        ("check_app", []), ("finish", ["K now toggles the switch."]),
    ]
    observations = data["observations"]
    if observations and observations[-1]["status"] != "ok":
        print("Workbench tool failed:", observations[-1], file=sys.stderr, flush=True)
        return {"tool": "finish", "args": ["The fixture saw a failed workbench tool."]}
    tool, args = steps[len(observations)]
    return {"tool": tool, "args": args}


def exercise(guest):
    prompt = b"$ "
    guest.until(prompt)

    def run(command, expected):
        guest.send(command.encode() + b"\r")
        output = re.sub(rb"\x1b\[[0-9;]*m", b"", guest.until(prompt, timeout=240))
        assert expected in output, output[-1200:]
        return output

    def store(*args, expected=b"DETAIL\t"):
        return run(shlex.join(["python3", "-I", "/lib/app-store/store.py", *args]), expected)

    def version(output):
        fields = re.search(rb"DETAIL\t[^\n]+", output)[0].decode().split("\t")
        assert re.fullmatch("[0-9a-f]{64}", fields[4]), fields
        return fields[4]

    run("agent desktop-work / store-ideas 123 | cat", "IDEA\t用 K 键切换开关".encode())
    store("package", "app-store", expected=b"/bin/app-store.win3")
    store("new", "port", "Toggle on K", "/missing.c", expected=b"ERROR\t")
    created = store("new", "port", "Toggle on K", "/src/desktop/app-template.c", expected=b"PROJECT\t")
    key = re.search(rb"PROJECT\t(port-[a-f0-9]{10})", created)[1].decode()
    first = version(store("send", key, "Add a K key"))
    run("grep 107 /work/store/" + key + "/tests.json", b"107\n")
    store("publish", key, first, expected=b"ACTIVE ")
    run("stat /bin/" + key + ".win2", b"regular file")
    run("echo '/* second revision */' >> /work/store/" + key + "/app.c", b"")
    second = version(store("check", key, ""))
    assert first != second
    store("publish", key, second, expected=b"ACTIVE ")
    store("rollback", key, first, expected=b"ACTIVE ")
    store("remove", key, second, expected=b"ERROR\t")
    store("remove", key, first)
    run("ls /work/store/" + key, b"app.c")
    run("stat /bin/" + key + ".win2", b"[exit 1]")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=60) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", required=True, type=Path, help="idle image with the store installed")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lixin-store-test-", dir="/tmp") as tmp:
        folder = Path(tmp)
        disk = folder / "disk.img"
        subprocess.run(["cp", "-c", str(options.disk), str(disk)], check=True)
        qemu = shutil.which("qemu-system-aarch64")
        if not qemu:
            raise RuntimeError("QEMU is required")
        wrapper = folder / "qemu-system-aarch64"
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join(
            [sys.executable, str(Path(__file__).resolve()), "--fixture", qemu]) + ' "$@"\n')
        wrapper.chmod(0o700)
        launch = ["env", f"PATH={folder}:{os.environ['PATH']}", HERE / "run-kernel.sh",
                  "--kernel", options.kernel, "--release", options.release, "--serial"]
        with SERIAL["boot"](disk, launch, prompt=b"LixinOS Bash (C shell)") as guest:
            exercise(guest)
    print(f"{options.kernel}: ideas, port, native build, install, rollback and removal passed")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
        sys.exit(runpy.run_path(str(HERE / "agent-bridge.py"))["launch"](sys.argv[2:], reply))
    main()
