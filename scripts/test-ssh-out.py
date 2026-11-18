"""Exercise the native EL0 SSH client against an isolated SSH server on the Mac."""
import asyncio
from contextlib import contextmanager
import os
from pathlib import Path
import runpy
import secrets
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
serial = runpy.run_path(str(ROOT / "scripts/test-user.py"))


async def serve(keyfile, port):
    import asyncssh

    class Login(asyncssh.SSHServer):
        def password_auth_supported(self):
            return True

        def validate_password(self, username, password):
            return username == "guest" and secrets.compare_digest(password, os.environ["TEST_PASS"])

    async def shell(proc):
        if not proc.get_terminal_type() or proc.command:
            proc.exit(1)
            return
        proc.stdout.write("remote$ ")
        async for line in proc.stdin:
            if line.strip() == "hostname":
                p = await asyncio.create_subprocess_exec("hostname", stdout=asyncio.subprocess.PIPE)
                data, _ = await p.communicate()
                proc.stdout.write(data.decode() + "remote$ ")
            elif line.strip() == "exit":
                proc.exit(0)
                return

    async with await asyncssh.listen("127.0.0.1", port, server_factory=Login,
                                     server_host_keys=[keyfile], process_factory=shell) as listener:
        print(listener.get_port(), flush=True)
        await listener.wait_closed()


@contextmanager
def fixture(key, port, secret):
    python = runpy.run_path(str(ROOT / "scripts/start-ssh.py"))["ssh_python"]()
    with subprocess.Popen([python, __file__, "--serve", key, str(port)],
                          env=dict(os.environ, TEST_PASS=secret), stdout=subprocess.PIPE) as proc:
        try:
            yield int(proc.stdout.readline())
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test(tmp):
    key = tmp / "key"
    other = tmp / "other"
    for path in [key, other]:
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", path], check=True)
    expected = subprocess.check_output(["ssh-keygen", "-lf", key.with_suffix(".pub")]).split()[1]
    password = secrets.token_hex(16)
    disk = tmp / "ssh.img"
    prompt = serial["SHELL"]
    with fixture(key, 0, password) as port:
        cmd = f"ssh -p {port} guest@10.0.2.2\r".encode()
        with serial["boot"](disk) as s:
            for line in ["#include <lixin.h>", "#include <stdio.h>",
                         "int main(void){unsigned char a[5000],b[5000];",
                         "if(lx_random(a,5000)||lx_random(b,5000))return 1;",
                         "if(!memcmp(a,b,5000)||!memcmp(a+4096,b+4096,904))return 2;",
                         'puts("fresh random bytes");return 0;}']:
                s.expect(f"echo '{line}' >> entropy.c\r".encode(), prompt)
            s.expect(b"tcc entropy.c -o entropy\r", prompt)
            s.expect(b"./entropy\r", prompt, b"\nfresh random bytes\n")
            s.expect(cmd, b"Trust this host (yes/no)? ", expected)
            s.expect(b"no\r", prompt, b"host not trusted")
            s.expect(cmd, b"Trust this host (yes/no)? ", expected)
            s.expect(b"yes\r", b"password: ")
            s.send(password.encode() + b"\r")
            assert password.encode() not in s.until(b"remote$ ")
            s.expect(b"hostname\r", b"remote$ ", subprocess.check_output(["hostname"]).strip())
            s.expect(b"exit\r", prompt)
            s.send(b"shutdown\r")
            assert s.vm.wait(timeout=10) == 0
        with serial["boot"](disk) as s:
            s.send(cmd)
            assert b"Trust this host" not in s.until(b"password: ")
            s.expect(b"wrong\r", prompt, b"ssh:")
            s.expect(b"echo $?\r", prompt, b"\n255\n")
            s.expect(cmd, b"password: ")
            s.expect(password.encode() + b"\r", b"remote$ ")
            s.expect(b"~.", prompt, b"Disconnected.")
    with fixture(other, port, password), serial["boot"](disk) as s:
        s.send(cmd)
        transcript = s.until(prompt)
        assert b"host key changed" in transcript and b"password:" not in transcript
    print("Verified native SSH, OpenSSH-compatible fingerprint, PTY, remote hostname, exit, "
          "password rejection, reboot persistence and changed-key rejection.")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--serve":
        asyncio.run(serve(sys.argv[2], int(sys.argv[3])))
    else:
        with tempfile.TemporaryDirectory() as tmp:
            test(Path(tmp))
