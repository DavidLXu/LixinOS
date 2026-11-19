"""Connect the graphical guest's explicit copy/paste requests to macOS."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
from threading import Lock, Thread


LIMIT = 4096


def serve(connection, send):
    with connection.makefile("rb") as incoming:
        while line := incoming.readline(2 * LIMIT + 32):
            if not line.endswith(b"\n"):
                return
            operation, separator, value = line[:-1].partition(b"\t")
            if not separator:
                continue
            if operation == b"C":
                try:
                    text = bytes.fromhex(value.decode("ascii"))
                    if 0 < len(text) <= LIMIT:
                        subprocess.run(["pbcopy"], input=text, timeout=2, check=True)
                except (OSError, ValueError, subprocess.SubprocessError):
                    continue
            elif operation == b"P" and 0 < len(value) <= 20 and value.isdigit():
                try:
                    text = subprocess.check_output(["pbpaste"], timeout=2)[:LIMIT]
                except (OSError, subprocess.SubprocessError):
                    continue
                send(value + b"\t" + text.hex().encode() + b"\n")


def console(connection, guest, command):
    lock = Lock()

    def send(frame):
        with lock:
            connection.sendall(frame)

    watcher = None
    thread = None
    if sys.platform == "darwin" and any(arg.startswith("cocoa") for arg in command):
        try:
            watcher = subprocess.Popen(
                ["swift", str(Path(__file__).with_name("window-size.swift")), str(guest.pid)],
                stdout=subprocess.PIPE,
            )
        except OSError:
            pass
    if watcher is not None:
        def send_sizes():
            assert watcher.stdout is not None
            try:
                for size in watcher.stdout:
                    send(b"R\t" + size)
            except OSError:
                pass
        thread = Thread(target=send_sizes, daemon=True)
        thread.start()
    try:
        serve(connection, send)
    finally:
        if watcher is not None:
            watcher.terminate()
            watcher.wait()
            thread.join(timeout=1)


def launch(command):
    with tempfile.TemporaryDirectory(prefix="lixinos-clip-", dir="/tmp") as directory:
        path = Path(directory) / "clipboard.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(path))
            listener.listen(1)
            listener.settimeout(0.25)
            device = [
                "-chardev", f"socket,id=clip-host,path={path}",
                "-device", "virtio-serial-device,id=clip-serial,bus=virtio-mmio-bus.6,max_ports=1",
                "-device", "virtconsole,bus=clip-serial.0,chardev=clip-host",
            ]
            with subprocess.Popen([*command, *device]) as guest:
                try:
                    while True:
                        try:
                            connection, _ = listener.accept()
                            break
                        except socket.timeout:
                            if guest.poll() is not None:
                                return guest.returncode
                    with connection:
                        try:
                            console(connection, guest, command)
                        except (BrokenPipeError, ConnectionResetError):
                            pass
                    return guest.wait()
                finally:
                    if guest.poll() is None:
                        guest.terminate()
                        try:
                            guest.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            guest.kill()
                            guest.wait()


def terminate(number, _frame):
    raise SystemExit(128 + number)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(launch(sys.argv[1:]))
