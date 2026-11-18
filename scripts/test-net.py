"""Exercise the native guest TCP path against a temporary Mac listener."""
from pathlib import Path
import os
import runpy
import socket
import tempfile
from concurrent.futures import ThreadPoolExecutor

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]
program = """#include <stdio.h>
#include <lixin.h>
int main(void) {
    static unsigned char buf[5003];
    int total = 19001, pos = 0, n, i;
    if (lx_tcp_connect("999.0.0.1", 80) >= 0) return 1;
    int fd = lx_tcp_connect("PEER", PORT);
    if (fd < 0) return 2;
    while (pos < total) {
        int want = total-pos < sizeof(buf) ? total-pos : sizeof(buf);
        for (i=0; i<want; i++) buf[i]=(pos+i)%251;
        n=lx_tcp_write(fd,buf,want);
        if (n == LX_TCP_AGAIN) continue;
        if (n <= 0) return 3;
        pos+=n;
    }
    pos=0;
    for (;;) {
        n=lx_tcp_read(fd,buf,sizeof(buf));
        if (n == LX_TCP_AGAIN) continue;
        if (n < 0) return 4;
        if (!n) break;
        for (i=0; i<n; i++) if (buf[i]!=(pos+i+1)%251) return 5;
        pos+=n;
    }
    if (pos!=total || lx_tcp_close(fd) || lx_tcp_read(fd,buf,1)!=-1) return 6;
    puts("verified 19001 bytes each direction");
    return 0;
}"""


def peer(listener):
    conn, _ = listener.accept()
    with conn:
        conn.settimeout(10)
        data = bytearray()
        while len(data) < 19001:
            chunk = conn.recv(1301)
            assert chunk
            data.extend(chunk)
        assert data == bytes(i % 251 for i in range(19001))
        conn.sendall(bytes((i+1) % 251 for i in range(len(data))))
        conn.shutdown(socket.SHUT_WR)
        assert conn.recv(1) == b""


with tempfile.TemporaryDirectory() as directory, socket.socket() as listener:
    host = os.environ.get("LIXINOS_TEST_HOST", "127.0.0.1")
    guest_peer = "10.0.2.2" if host == "127.0.0.1" else host
    listener.bind((host, 0))
    listener.listen()
    listener.settimeout(12)
    port = listener.getsockname()[1]
    with serial["boot"](Path(directory) / "net.img") as s:
        s.expect(b"netinfo\r", prompt, b"IPv4 10.0.2.15/24  gateway 10.0.2.2")
        s.expect(b"tcc /src/net/tcpcheck.c -o /bin/tcpcheck\r", prompt)
        s.expect(f"tcpcheck {guest_peer} {port}\r".encode(), prompt, b"TCP connected")
        conn, _ = listener.accept()
        with conn:
            conn.settimeout(5)
            assert conn.recv(1) == b""
        with socket.socket() as unused:
            unused.bind((host, 0))
            closed_port = unused.getsockname()[1]
            s.expect(f"tcpcheck {guest_peer} {closed_port}\r".encode(), prompt, b"TCP connection failed")
        for line in program.replace("PEER", guest_peer).replace("PORT", str(port)).splitlines():
            s.expect(f"echo '{line}' >> net.c\r".encode(), prompt)
        s.expect(b"tcc net.c -o /bin/net-test\r", prompt)
        for _ in range(2):
            with ThreadPoolExecutor(max_workers=1) as pool:
                result = pool.submit(peer, listener)
                s.expect(b"net-test\r", prompt, b"\nverified 19001 bytes each direction\n")
                result.result()
        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=10) == 0
print("Verified guest-compiled TCP client over virtio-net, binary transfers, EOF, close and reconnect.")
