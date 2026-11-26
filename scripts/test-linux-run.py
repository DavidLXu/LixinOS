"""Boot a private LixinOS disk and exercise Debian's stock arm64 interpreter and libc."""
import argparse
import hashlib
from pathlib import Path
import runpy
import socket
import socketserver
import threading
import subprocess
import tempfile

from disk_image import edit_files, prepare

ROOT = Path(__file__).resolve().parents[1]
NATIVE = runpy.run_path(str(ROOT / "scripts/test-native-kernel.py"))
URL = "https://deb.debian.org/debian/pool/main/h/hello/hello_2.10-3_arm64.deb"
DIGEST = "f7b20d304db0f3a18da28eb2000de2b0205eaa340c1d0fecec3b7c50acc932dc"


def stock_hello(directory):
    deb = directory / "hello.deb"
    subprocess.run(["curl", "-fsSL", "--retry", "2", URL, "-o", deb], check=True)
    if hashlib.sha256(deb.read_bytes()).hexdigest() != DIGEST:
        raise ValueError("unexpected Debian hello package")
    tarball = subprocess.check_output(["tar", "-xOf", deb, "data.tar.xz"])
    return subprocess.check_output(["tar", "-xJOf", "-", "./usr/bin/hello"], input=tarball)


def check_tcp_peek(run):
    class Peer(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(30)
            if self.request.recv(1) == b"g":
                self.request.sendall(b"AB")
                self.request.shutdown(socket.SHUT_WR)

    with socketserver.TCPServer(("127.0.0.1", 0), Peer) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            port = server.server_address[1]
            run(f"linux-run /bin/linux-chromium-probe tcp-peek {port}",
                b"TCP peek preserved bytes, EAGAIN and EOF")
        finally:
            server.shutdown()
            worker.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=ROOT / "Kernel/target/linux-user/package")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    parser.add_argument("--check-descriptor-capacity", action="store_true",
                        help="also exercise kernels with the enlarged descriptor table")
    parser.add_argument("--check-shared-mappings", action="store_true",
                        help="also exercise kernels that support 128 simultaneous surface mappings")
    parser.add_argument("--check-shared-space", action="store_true", help="query actual shared memory backing space")
    parser.add_argument("--check-record-locks", action="store_true", help="exercise SQLite byte-range file locks across processes")
    parser.add_argument("--check-tcp-peek", action="store_true", help="check TCP receive and MSG_PEEK on a local connection")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lixinos-glibc-") as directory:
        disk = Path(directory) / "test.img"
        prepare(disk)
        def boot(kernel):
            cmd = ["env", f"LIXINOS_TCC_DISK={disk}", ROOT / "scripts/run-kernel.sh",
                   "--kernel", kernel, "--release", args.release, "--serial"]
            return NATIVE["guest"]["boot"](disk, cmd, prompt=b"LixinOS Bash (C shell)")
        def shell(serial):
            serial.until(b"$ ")
            serial.expect(b"cd /\r", NATIVE["shell"])

        # Rust seeds the empty disk with a shell that both released kernels can boot.
        with boot("rust") as serial:
            shell(serial)
            NATIVE["command"](serial, "mkdir /linux-test")
            NATIVE["shutdown"](serial)

        external = stock_hello(Path(directory))
        def stage(rows):
            for path in args.package.rglob("*"):
                name = "/" + path.relative_to(args.package).as_posix()
                rows[name.encode()] = bytes(8), path.read_bytes() if path.is_file() else None
            rows[b"/linux-test/gnu-hello"] = bytes(8), external
        edit_files(disk, stage)

        for kernel in (("rust", "tcc") if args.kernel == "both" else (args.kernel,)):
            with boot(kernel) as serial:
                shell(serial)
                def run(command, expected=b"", status=0):
                    NATIVE["command"](serial, command, status=status, expected=expected)
                run("linux-run /bin/linux-pthread-probe", b"Linux pthread: TLS, condition and join passed")
                run("linux-run /bin/linux-hello", b"\nHello from glibc 2.36: LixinOS\n")
                run("linux-run --processes /bin/linux-chromium-probe", b"\nChromium startup primitives passed\n")
                if args.check_record_locks:
                    run("linux-run --processes --state /linux-test /bin/linux-chromium-probe record-locks",
                        b"SQLite byte ranges, shared readers, conflicts, blocking and close passed")
                if args.check_tcp_peek:
                    check_tcp_peek(run)
                if args.check_shared_space:
                    run("linux-run --processes /bin/linux-chromium-probe shared-space",
                        b"Shared-memory capacity and allocated blocks passed")
                if args.check_shared_mappings:
                    run("linux-run --processes /bin/linux-chromium-probe shared-mappings",
                        b"128 independent shared mappings and fragmented multi-block allocation passed")
                if args.check_descriptor_capacity:
                    run("linux-run --processes --state /linux-test /bin/linux-chromium-probe descriptors",
                        b"72 independent file descriptions and inherited shared offset passed")
                run("linux-run /bin/linux-hello QEMU status-test > /linux-test/output", status=37)
                run("cat /linux-test/output", b"\nHello from glibc 2.36: QEMU\n")
                run("linux-run /lib/ld-linux-aarch64.so.1 --list /bin/linux-hello",
                    b"libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6")
                run("linux-run /linux-test/gnu-hello", b"\nHello, world!\n")
                run("linux-run /linux-test/gnu-hello --greeting=QEMU-ARM64", b"\nQEMU-ARM64\n")
                lib = "/lib/aarch64-linux-gnu/libc.so.6"
                run(f"mv {lib} /linux-test/lib.saved")
                run("linux-run /bin/linux-hello", b"error while loading shared libraries: libc.so.6", 127)
                run(f"mv /linux-test/lib.saved {lib}")
                run("linux-run /bin/linux-hello restored", b"\nHello from glibc 2.36: restored\n")
                NATIVE["shutdown"](serial)
                print(f"{kernel}: Chromium startup primitives, glibc, Debian hello, stdout, statuses 0/37/127 and restored libc passed", flush=True)


if __name__ == "__main__":
    main()
