"""Exercise native AF_UNIX streams, memfd and SCM_RIGHTS on both kernels."""
import argparse
from pathlib import Path
import runpy
import tempfile

from disk_image import prepare

HERE = Path(__file__).resolve().parent
NATIVE = runpy.run_path(str(HERE / "test-native-kernel.py"))
PUT_BYTES = runpy.run_path(str(HERE / "test-cpython.py"))["put_bytes"]
PROGRAM = r"""
import errno
import fcntl
import mmap
import os
import select
import socket
import struct
import subprocess
import sys

def credentials(peer, pid):
    assert struct.unpack("3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)) == (pid, 0, 0)

def client(address):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(address)
        expect = address.encode() if address.startswith("\0") else address
        assert connection.getpeername() == expect
        credentials(connection, int(sys.argv[2]))
        assert connection.sendmsg([b"pi", b"ng"]) == 4
        connection.shutdown(socket.SHUT_WR)
        assert connection.recv(4) == b"pong"

def expect_errno(code, action, *args):
    try:
        action(*args)
    except OSError as error:
        assert error.errno == code, error
    else:
        raise AssertionError(f"expected errno {code}")

def pair():
    left, right = socket.socketpair()
    with left, right:
        credentials(right, os.getpid())
        assert fcntl.fcntl(left, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        left.setblocking(False)
        expect_errno(errno.EAGAIN, left.recv, 1)
        assert right.sendmsg([b"lix", b"inos"]) == 7
        a, b = bytearray(3), bytearray(4)
        assert left.recvmsg_into([a, b])[:3] == (7, [], 0)
        assert a + b == b"lixinos"
        total = 0
        for _ in range(16):
            try:
                total += left.send(b"x" * 1024)
            except BlockingIOError:
                break
        else:
            raise AssertionError("stream never applied backpressure")
        assert total >= 1024
        expect_errno(errno.EAGAIN, left.send, b"x")
        left.shutdown(socket.SHUT_WR)
        while total:
            data = right.recv(1024)
            assert data and data == b"x" * len(data)
            total -= len(data)
        assert right.recv(1) == b""
        right.close()
        assert left.recv(1) == b""

def named(address):
    expect = address.encode() if address.startswith("\0") else address
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(address)
        assert listener.getsockname() == expect
        with socket.socket(socket.AF_UNIX) as duplicate:
            expect_errno(errno.EADDRINUSE, duplicate.bind, address)
        listener.listen(1)
        listener.setblocking(False)
        expect_errno(errno.EAGAIN, listener.accept)
        with subprocess.Popen([sys.executable, __file__, address.encode().hex(), str(os.getpid())]) as process:
            poll = select.poll()
            poll.register(listener, select.POLLIN)
            assert poll.poll(5000) == [(listener.fileno(), select.POLLIN)]
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                assert connection.getsockname() == expect
                credentials(connection, process.pid)
                assert connection.recv(4) == b"ping"
                assert connection.recv(1) == b""
                connection.sendall(b"pong")
            assert process.wait(timeout=5) == 0
    with socket.socket(socket.AF_UNIX) as reopened:
        expect_errno(errno.ECONNREFUSED, reopened.connect, address)
        reopened.bind(address)

def share(sock, payload, *fds):
    packed = struct.pack("i" * len(fds), *fds)
    assert sock.sendmsg([payload], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, packed)]) == len(payload)

def memory_client(address):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(address)
        fd = os.memfd_create("wayland-pool", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        os.posix_fallocate(fd, 0, 3 * 4096)
        assert os.fstat(fd).st_size == 3 * 4096
        assert os.write(fd, b"head") == 4
        alias = fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 9)
        assert alias >= 9 and fcntl.fcntl(alias, fcntl.F_GETFD) == fcntl.FD_CLOEXEC
        assert os.lseek(alias, 0, os.SEEK_CUR) == 4
        pool = mmap.mmap(fd, 8192, access=mmap.ACCESS_WRITE, offset=4096, trackfd=False)
        with pool:
            assert pool[:8] == b"\0" * 8
            pool[4092:4100] = b"client!!"
            connection.sendall(b"plain")
            share(connection, b"F", alias)
            os.close(alias)
            os.close(fd)
            assert connection.recv(1) == b"K"
            assert pool[4092:4100] == b"server!!"

def shared_memory():
    address = "/tmp/lixin-shm"
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(address)
        listener.listen(1)
        listener.settimeout(5)
        with subprocess.Popen([sys.executable, __file__, "memfd", address]) as process:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                assert connection.recv(5) == b"plain"
                payload, control, flags, _ = connection.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
                assert payload == b"F" and len(control) == 1 and not (flags & socket.MSG_CTRUNC)
                level, kind, raw = control[0]
                assert (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
                fd, = struct.unpack("i", raw)
                assert fcntl.fcntl(fd, fcntl.F_GETFD) == fcntl.FD_CLOEXEC
                assert os.fstat(fd).st_size == 3 * 4096
                assert os.lseek(fd, 0, os.SEEK_CUR) == 4
                assert os.lseek(fd, 0, os.SEEK_SET) == 0 and os.read(fd, 4) == b"head"
                with mmap.mmap(fd, 4096, access=mmap.ACCESS_READ, trackfd=False) as header:
                    assert header[:4] == b"head"
                with mmap.mmap(fd, 8192, access=mmap.ACCESS_WRITE, offset=4096, trackfd=False) as pool:
                    expect_errno(errno.EBUSY, os.ftruncate, fd, 2 * 4096)
                    os.close(fd)
                    assert pool[4092:4100] == b"client!!"
                    pool[4092:4100] = b"server!!"
                    connection.sendall(b"K")
            assert process.wait(timeout=5) == 0

def ancillary_lifetime():
    left, right = socket.socketpair()
    with left, right:
        for _ in range(40):
            fd = os.memfd_create("drop")
            share(left, b"0", fd)
            share(left, b"1", fd)
            os.close(fd)
            assert right.recv(2) == b"0"
            data, control, flags, _ = right.recvmsg(1)
            assert data == b"1" and not control and flags & socket.MSG_CTRUNC
        fd = os.memfd_create("close")
        share(left, b"r", fd)
        os.close(fd)
        right.shutdown(socket.SHUT_RD)
        expect_errno(errno.EPIPE, left.send, b"x")

if len(sys.argv) > 1:
    if sys.argv[1] == "memfd":
        memory_client(sys.argv[2])
    else:
        client(bytes.fromhex(sys.argv[1]).decode())
else:
    pair()
    named("/tmp/lixin-local-stream")
    named("\0lixin-local-stream")
    shared_memory()
    ancillary_lifetime()
    print("Unix stream checks passed")
"""


def check(release):
    with tempfile.TemporaryDirectory(prefix="lixin-local-") as temp:
        disk = Path(temp) / "test.img"
        prepare(disk)
        for kernel in ("rust", "tcc"):
            launch = ["env", f"LIXINOS_TCC_DISK={disk}", HERE / "run-kernel.sh",
                      "--kernel", kernel, "--release", release, "--serial"]
            with NATIVE["guest"]["boot"](disk, launch, prompt=b"Kernel ") as serial:
                serial.until(b"$ ")
                serial.expect(b"cd /\r", NATIVE["shell"])
                command = NATIVE["command"]
                if kernel == "rust":
                    PUT_BYTES(lambda line: command(serial, line), "local-sockets.py", PROGRAM.encode())
                command(serial, "python3 /local-sockets.py", expected=b"\nUnix stream checks passed\n")
                NATIVE["shutdown"](serial)
            print(f"{kernel}: Unix stream checks passed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", default="latest")
    check(parser.parse_args().release)
