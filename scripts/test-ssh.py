"""Connect the Mac OpenSSH client to a real guest and resume a detached REPL."""
from contextlib import contextmanager
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
Serial = runpy.run_path(str(ROOT / "scripts/test-user.py"))["Serial"]


@contextmanager
def child(args, **opts):
    vm = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, **opts)
    try:
        yield vm
    finally:
        if vm.poll() is None:
            vm.terminate()
        vm.wait(timeout=10)


def test(directory):
    keys = directory / "keys"
    env = dict(os.environ, LIXINOS_DISK=str(directory / "test.img"),
               LIXINOS_SSH_DIR=str(keys), LIXINOS_SSH_PORT="0", DEEPSEEK_API_KEY="")
    with child([ROOT / "scripts/run-kernel.sh", "--ssh"], env=env) as server:
        startup = Serial(server).until(b"lixin@127.0.0.1", timeout=600)
        port = re.search(rb"-p (\d+) $", startup).group(1).decode()
        known = directory / "known_hosts"
        known.write_text(f"[127.0.0.1]:{port} " + (keys / "host_ed25519.pub").read_text())
        ssh = ["ssh", "-F", "/dev/null", "-oBatchMode=yes", "-oIdentitiesOnly=yes",
               "-oStrictHostKeyChecking=yes", f"-oUserKnownHostsFile={known}",
               "-p", port, "-i", str(keys / "id_ed25519")]
        wrong = subprocess.run([*ssh, "other@127.0.0.1"], capture_output=True, timeout=10)
        assert wrong.returncode != 0 and b"Permission denied" in wrong.stderr, wrong.stderr
        other_key = directory / "unauthorized"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", other_key], check=True)
        other = ssh.copy()
        other[-1] = str(other_key)
        rejected = subprocess.run([*other, "lixin@127.0.0.1"], capture_output=True, timeout=10)
        assert rejected.returncode != 0 and b"Permission denied" in rejected.stderr
        ssh += ["-tt", "lixin@127.0.0.1"]
        with child(ssh) as client:
            console = Serial(client)
            console.until(b"lixin:/$ ")
            console.expect(b"mkdir demo\r", b"lixin:/$ ")
            console.expect(b"cd demo\r", b"lixin:/demo$ ")
            console.expect(b"v=first\r", b"lixin:/demo$ ")
            with child(ssh) as second:
                other = Serial(second)
                other.until(b"lixin:/$ ")
                other.expect(b"v=second\r", b"lixin:/$ ")
                console.send(b"echo $")
                console.until(b"echo $")
                other.expect(b"echo $v > shared\r", b"lixin:/$ ")
                console.expect(b"v\r", b"lixin:/demo$ ", b"\nfirst\n")
                console.expect(b"cat /shared\r", b"lixin:/demo$ ", b"\nsecond\n")
                busy = subprocess.run([*ssh, "attach", "1"], input=b"", capture_output=True, timeout=10)
                assert busy.returncode != 0 and b"already attached" in busy.stdout + busy.stderr
                console.expect(b"python\r", b">>> ")
                other.send(b"echo $v\r")
                console.expect(b"x = 40\r", b">>> ")
                client.terminate()
                client.wait(timeout=5)
                with child([*ssh, "attach", "1"]) as resumed:
                    console = Serial(resumed)
                    console.until(b"console attached]")
                    console.expect(b"x+2\r", b">>> ", b"\n42\n")
                    console.expect(b"\x04", b"lixin:/demo$ ")
                    assert b"\nsecond\n" in other.until(b"lixin:/$ ")
                    other.send(b"exit\r")
                    assert second.wait(timeout=5) == 0
        with child(ssh) as client:
            console = Serial(client)
            console.until(b"console attached]")
            console.send(b"shutdown\r")
            console.until(b"Shutting down")
            assert server.wait(timeout=10) == 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        test(Path(temp))
    print("Verified SSH public-key login, independent terminals and shared files, REPL reconnect, and guest shutdown.")
