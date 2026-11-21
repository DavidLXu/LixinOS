"""Round-trip a project over a loopback SFTP server from a disposable QEMU guest."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import secrets
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
fixture = runpy.run_path(str(ROOT / "scripts/test-ssh-out.py"))["fixture"]
boot = runpy.run_path(str(ROOT / "scripts/test-user.py"))["boot"]


def test(tmp, disk, kernel, release):
    key = tmp / "key"
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key], check=True)
    project = tmp / "remote" / "project"
    (project / "sub").mkdir(parents=True)
    source = b'#include <stdio.h>\nint main(void){puts("SFTP compiled");return 0;}\n'
    (project / "main.c").write_bytes(source)
    (project / "sub/data.bin").write_bytes(os.urandom(512 * 1024 + 71))
    (project / "empty").touch()
    (project / "name with spaces").write_bytes(b"spaces")
    password = secrets.token_hex(16)
    os.environ["LIXINOS_TCC_DISK"] = str(disk)
    with fixture(key, 0, password) as port, boot(disk, [ROOT / "scripts/run-kernel.sh",
            "--serial", "--kernel", kernel, "--release", release], prompt=b"$ ") as s:
        command = f"sftp -P {port} guest@10.0.2.2\r".encode()
        s.expect(command, b"Trust this host (yes/no)? ")
        s.expect(b"yes\r", b"password: ")
        s.expect(password.encode()+b"\r", b"sftp> ")
        s.expect(b"get -r project sftp-local\r", b"sftp> ", b"Transfer complete.")
        s.expect(b'get "project/name with spaces" "sftp spaced"\r', b"sftp> ", b"Transfer complete.")
        s.expect(b"get missing-file sftp-local/main.c\r", b"sftp> ", b"sftp: ")
        s.expect(b"quit\r", b"$ ")
        s.expect(b"tcc sftp-local/main.c -o app-sftp\r", b"$ ")
        s.expect(b"./app-sftp\r", b"$ ", b"\nSFTP compiled\n")
        s.expect(b"echo '// edited in LixinOS' >> sftp-local/main.c\r", b"$ ")
        s.expect(command, b"password: ")
        s.expect(password.encode()+b"\r", b"sftp> ")
        s.expect(b"put -r sftp-local returned\r", b"sftp> ", b"Transfer complete.")
        s.expect(b"put sftp-local/main.c returned\r", b"sftp> ", b"Transfer complete.")
        s.expect(b"quit\r", b"$ ")
        dest = project.parent / "returned"
        assert (dest / "main.c").read_bytes() == source+b"// edited in LixinOS\n"
        assert (dest / "empty").read_bytes() == b""
        digest = hashlib.sha256((project / "sub/data.bin").read_bytes()).hexdigest()
        assert hashlib.sha256((dest / "sub/data.bin").read_bytes()).hexdigest() == digest
        print(kernel, "SFTP binary round trip SHA-256", digest, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("disk", type=Path, help="disposable seeded guest image")
    parser.add_argument("--kernel", choices=["rust","tcc"], default="rust")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        test(Path(work), args.disk.resolve(), args.kernel, args.release)
