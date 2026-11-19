"""Exercise the guest agent using a local model fixture and a copy of the TCC disk."""

import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
native = runpy.run_path(str(root / "scripts/test-native-kernel.py"))
bridge = runpy.run_path(str(root / "scripts/agent-bridge.py"))
program = """try:
    open('forbidden.txt','w')
except OSError:
    print(int(input())*2)
"""


def model(instruction, data):
    if "question" in data:
        if not data["observations"]:
            return {"queries": [{"tool": "system"}]}
        assert "linux_kernel=false" in data["observations"][0]
        return {"answer": "The guest is LixinOS."}
    if data.get("task") == "command fixture":
        steps = [
            ("exec", ["bash", "-c", "echo local > local.txt"]),
            ("exec", ["cat", "local.txt"]),
            ("exec", ["bash", "-c", "echo forbidden > /parity-escaped"]),
            ("finish", ["Local command checks complete."]),
        ]
        seen = data["observations"]
        if len(seen) == 2:
            assert "local" in seen[-1]["detail"], seen
        if len(seen) == 3:
            assert seen[-1]["status"] == "failed", seen
        cmd, args = steps[len(seen)]
        return {"tool": cmd, "args": args}
    if data.get("task") == "tune fixture":
        steps = [
            ("read", ["settings.py"]),
            ("write", ["settings.py", 'print("35 384")\n']),
            ("test", ["settings.py", "", "35 384\n"]),
            ("finish", ["Tune the defaults."]),
        ]
        cmd, args = steps[len(data["observations"])]
        return {"tool": cmd, "args": args}
    if "workspace" in data:
        system = data["task"] == "system fixture"
        actions = (
            [
                ("mkdir", ["/parity-system"]),
                ("write", ["/parity-system/test.c", "int main(void){return 7;}"]),
                ("compile", ["/parity-system/test", "/parity-system/test.c"]),
                ("finish", ["Review the compiled program."]),
            ]
            if system
            else [
                ("write", ["../outside.py", "print(0)"]),
                ("write", ["number.py", "print(42)"]),
                ("test", ["number.py", "", "42\n"]),
                ("finish", ["Review the project file."]),
            ]
        )
        observations = data["observations"]
        if len(observations) and not system:
            assert "path outside" in observations[0], observations
        if len(observations) == 3:
            assert ("using compile" if system else "test passed") in observations[-1], (
                observations
            )
        tool, args = actions[len(observations)]
        return {"tool": tool, "args": args}
    if "tests" not in data:
        return {"tests": [{"stdin": "21\n", "stdout": "42\n"}]}
    if "improve" in data.get("task", ""):
        return {"script": program + "# improved\n"}
    if not data["previous_script"]:
        return {"script": "while True: pass"}
    assert "timed out" in data["failure"], data
    return {"script": program}


def main():
    seed = Path(os.environ.get("LIXINOS_TEST_SEED", root / "data/tcc-os/system.img"))
    loader = Path(os.environ.get("LIXINOS_KERNEL", root / "data/tcc-os/loader.elf"))
    shell = native["shell"]
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "agent.img"
        subprocess.run(["cp", "-c", seed, disk], check=True)
        launch = [
            sys.executable,
            __file__,
            "--fixture",
            "qemu-system-aarch64",
            "-machine",
            "virt",
            "-accel",
            "hvf",
            "-cpu",
            "host",
            "-m",
            "1536M",
            "-display",
            "none",
            "-serial",
            "stdio",
            "-monitor",
            "none",
            "-global",
            "virtio-mmio.force-legacy=false",
            "-drive",
            f"if=none,id=d,format=raw,file={disk}",
            "-device",
            "virtio-blk-device,drive=d,bus=virtio-mmio-bus.0",
            "-object",
            "rng-random,id=r,filename=/dev/urandom",
            "-device",
            "virtio-rng-device,rng=r,bus=virtio-mmio-bus.5",
            "-kernel",
            str(loader),
        ]
        with native["guest"]["boot"](disk, launch) as guest:
            run = lambda cmd, output=b"", status=0: native["command"](
                guest, cmd, status, output
            )
            run("agent inspect identity", b"The guest is LixinOS.")
            run("agent parity-double.py generate", b"all 1 tests passed")
            run("cat forbidden.txt", b"file not found", 1)
            run(
                "agent improve parity-double.py improve", b"activated /parity-double.py"
            )
            run("mkdir parity-project")
            for answer in (b"n", b"y"):
                guest.send(b"agent run /parity-project project\r")
                guest.until(b"to disk? [y/N] ", timeout=45)
                guest.expect(
                    answer, shell, b"discarded" if answer == b"n" else b"applied"
                )
                if answer == b"n":
                    run("cat parity-project/number.py", b"file not found", 1)
            run("cat outside.py", b"file not found", 1)
            run("python parity-project/number.py", b"\n42\n")
            run("tcc /src/agent/run.c -o /bin/agent-run")
            guest.send(b'agent system "system fixture"\r')
            guest.until(b"to disk? [y/N] ", timeout=45)
            guest.expect(b"y", shell, b"applied staged system changes")
            run("/parity-system/test", status=7)
            prompt = b"\x1b[96magent> \x1b[0m"
            guest.expect(b"agent chat /parity-project\r", prompt)
            guest.expect(
                b"command fixture\r", prompt, b"Local command checks complete."
            )
            guest.expect(b"/export\r", prompt, b"agent-conversation.txt")
            guest.expect(b"/export\r", prompt, b"agent-conversation-2.txt")
            guest.expect(b"/exit\r", shell)
            run("cat parity-project/local.txt", b"local")
            run("cat /parity-escaped", b"file not found", 1)
            guest.send(b'agent tune "tune fixture"\r')
            guest.until(b"to disk? [y/N] ", timeout=45)
            guest.expect(b"v", b"to disk? [y/N] ", b'print("35 384")')
            guest.expect(b"y", shell, b"applied staged scripts")
            run("cat /agent-policy/settings.py", b'print("35 384")')
            run("mv parity-double.py parity-moved.py")
            run("touch parity-plain.py")
            run("rm parity-plain.py")
            native["shutdown"](guest)
        rust_disk = Path(tmp) / "rust-rollback.img"
        subprocess.run(["cp", "-c", disk, rust_disk], check=True)
        with native["guest"]["boot"](
            rust_disk, [root / "scripts/run-kernel.sh", "--kernel", "rust", "--serial"]
        ) as guest:
            run("agent rollback parity-moved.py")
            run("cat parity-moved.py", b"print(int(input())*2)")
            run("agent system-rollback")
            run("ls /parity-system", status=1)
            native["shutdown"](guest)
        with native["native_boot"](loader, disk, prompt=shell) as guest:
            run("agent rollback-tune", b"restored previous version")
            run("cat /agent-policy/settings.py", b'print("50 512")')
            run("agent rollback parity-moved.py", b"restored previous version")
            run("agent system-rollback", b"restored the previous system files")
            run("ls /parity-system", status=1)
            native["shutdown"](guest)
    print(
        "Verified TCC model bridge, isolated tests and timeout, replaceable runner, staged approvals, guest compilation and rollback under both kernels."
    )


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        sys.exit(bridge["launch"](sys.argv[2:], model))
    main()
