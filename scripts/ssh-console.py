"""Authenticated, reconnectable loopback SSH transport for the guest UART."""
import asyncio
import contextlib
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import sys
import tempfile

import asyncssh


def prepare_keys(directory):
    if not directory.exists():
        directory.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory.parent) as staging:
            keys = Path(staging) / "keys"
            keys.mkdir(mode=0o700)
            for name in ("host_ed25519", "id_ed25519"):
                subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                                "-f", str(keys / name)], check=True)
            try:
                keys.rename(directory)
            except OSError:
                if not directory.is_dir():
                    raise
    return (asyncssh.read_private_key(directory / "host_ed25519"),
            asyncssh.read_public_key(directory / "id_ed25519.pub"))


class Login(asyncssh.SSHServer):
    def __init__(self, public_key):
        self.public_key = public_key

    def begin_auth(self, username):
        return True

    def public_key_auth_supported(self):
        return True

    def validate_public_key(self, username, key):
        return username == "lixin" and key == self.public_key


class Endpoint:
    def __init__(self):
        self.client = None
        self.pending = bytearray()
        self.ready = asyncio.Event()
        self.ready.set()
        self.opened = False

    def output(self, data):
        if self.client is not None:
            try:
                self.client.stdout.write(bytes(data))
                return
            except (OSError, asyncssh.Error):
                self.client = None
        self.pending.extend(data)
        del self.pending[:-65536]


class Console:
    def __init__(self):
        self.vm = None
        self.endpoints = {1: Endpoint()}
        self.next_id = 2

    def select(self, command):
        words = command.split() if command is not None else []
        if not words:
            for number, endpoint in self.endpoints.items():
                if endpoint.client is None:
                    return number
        elif len(words) == 2 and words[0] == "attach" and words[1].isascii() and words[1].isdigit():
            number = int(words[1])
            if number not in self.endpoints:
                raise ValueError("Unknown terminal ID.")
            if self.endpoints[number].client is not None:
                raise ValueError("Terminal already attached.")
            return number
        elif words != ["new"]:
            raise ValueError("Use an interactive shell, new, or attach ID.")
        number = self.next_id
        if number >= 1 << 30:
            raise ValueError("Terminal IDs exhausted.")
        self.next_id += 1
        self.endpoints[number] = Endpoint()
        return number

    def frame(self, op, number, data=b""):
        self.vm.stdin.write(struct.pack("<BIH", op, number, len(data)) + data)

    async def input(self, process, number, endpoint):
        while True:
            try:
                data = await process.stdin.read(4096)
            except asyncssh.TerminalSizeChanged:
                continue
            if not data:
                return
            await endpoint.ready.wait()
            endpoint.ready.clear()
            self.frame(2, number, data)
            await self.vm.stdin.drain()

    async def attach(self, process):
        try:
            if process.subsystem is not None:
                raise ValueError("Use an interactive shell.")
            number = self.select(process.command)
        except ValueError as exc:
            process.stderr.write(f"{exc}\r\n".encode())
            process.exit(1)
            return
        endpoint = self.endpoints[number]
        endpoint.client = process
        tasks = []
        try:
            process.stdout.write(f"[LixinOS terminal {number} console attached]\r\n".encode())
            process.stdout.write(bytes(endpoint.pending))
            endpoint.pending.clear()
            if not endpoint.opened:
                self.frame(1, number)
                endpoint.opened = True
            tasks = [asyncio.create_task(self.input(process, number, endpoint)),
                     asyncio.create_task(process.wait_closed())]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (OSError, asyncssh.Error):
            pass
        finally:
            for task in tasks:
                task.cancel()
            if endpoint.client is process:
                endpoint.client = None
            process.exit(0)
            await asyncio.gather(*tasks, return_exceptions=True)

    async def relay_output(self):
        current = 1
        buf = bytearray()
        while data := await self.vm.stdout.read(8192):
            buf.extend(data)
            while buf:
                marker = buf.find(b"\xff")
                end = len(buf) if marker < 0 else marker
                endpoint = self.endpoints.get(current)
                if end:
                    if endpoint:
                        endpoint.output(buf[:end])
                    del buf[:end]
                if len(buf) < 2:
                    break
                op = buf[1]
                if op == 0:
                    if endpoint:
                        endpoint.output(b"\xff")
                    del buf[:2]
                    continue
                if len(buf) < 6:
                    break
                number, = struct.unpack_from("<I", buf, 2)
                del buf[:6]
                if op == 1:
                    current = number
                elif op == 2 and number in self.endpoints:
                    self.endpoints[number].ready.set()
                elif op == 3 and number in self.endpoints:
                    closed = self.endpoints.pop(number)
                    if closed.client:
                        closed.client.exit(0)


async def serve(command):
    directory = Path(os.environ.get("LIXINOS_SSH_DIR",
                                    Path(__file__).resolve().parents[1] / "data" / "ssh")).resolve()
    host_key, public_key = prepare_keys(directory)
    port = int(os.environ.get("LIXINOS_SSH_PORT", "2222"))
    if not 0 <= port <= 65535:
        raise ValueError("invalid LIXINOS_SSH_PORT")
    console = Console()
    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stopped.set)
    listener = await asyncssh.listen(
        "127.0.0.1", port, server_factory=lambda: Login(public_key),
        server_host_keys=[host_key], process_factory=console.attach, encoding=None,
        line_editor=False, config=None, agent_forwarding=False,
    )
    async with listener:
        console.vm = await asyncio.create_subprocess_exec(
            *command, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            start_new_session=True)
        print(f"ssh -i {shlex.quote(str(directory / 'id_ed25519'))} -p {listener.get_port()} "
              "lixin@127.0.0.1", flush=True)
        print(f"SSH host fingerprint {host_key.get_fingerprint()}", flush=True)
        relay = asyncio.create_task(console.relay_output())
        stop = asyncio.create_task(stopped.wait())
        try:
            await asyncio.wait([relay, stop], return_when=asyncio.FIRST_COMPLETED)
            if relay.done():
                relay.result()
                return await console.vm.wait()
            return 0
        finally:
            for endpoint in console.endpoints.values():
                if endpoint.client:
                    endpoint.client.exit(0)
            if console.vm.returncode is None:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(console.vm.pid, signal.SIGTERM)
                try:
                    await asyncio.wait_for(console.vm.wait(), 5)
                except asyncio.TimeoutError:
                    os.killpg(console.vm.pid, signal.SIGKILL)
                    await console.vm.wait()
            for task in (stop, relay):
                task.cancel()
            await asyncio.gather(stop, relay, return_exceptions=True)


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(serve(sys.argv[1:])))
    except (OSError, ValueError, asyncssh.Error, subprocess.CalledProcessError) as exc:
        sys.exit(f"LixinOS SSH: {exc}")
