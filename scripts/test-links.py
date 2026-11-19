"""Boot a disposable QEMU guest and navigate real local and HTTP documents."""
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import argparse
import runpy
import ssl
import subprocess
import time
from tempfile import TemporaryDirectory
from threading import Thread

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]
ready = b"Q: quit"


def verify_https(guest, root):
    guest.send(b"links https://example.com\r")
    page = guest.until(ready, timeout=40)
    assert b"Example Domain" in page and b"documentation" in page, page
    guest.expect(b"q", b"lixin:/pages$ ")

    cert, key = root / "test.pem", root / "test.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-subj", "/CN=10.0.2.2", "-days", "2", "-out", cert,
                    "-keyout", key], check=True, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    with ThreadingHTTPServer(("127.0.0.1", 0), SimpleHTTPRequestHandler) as server:
        server.socket = context.wrap_socket(server.socket, server_side=True)
        Thread(target=server.serve_forever, daemon=True).start()
        try:
            guest.expect(f"links https://10.0.2.2:{server.server_port}/\r".encode(),
                         b"lixin:/pages$ ", b"TLS certificate is not trusted")
        finally:
            server.shutdown()


def verify_google(guest):
    guest.send(b"links https://google.com\r")
    page = guest.until(ready, timeout=60)
    assert b"Google Search" in page, page
    for _ in range(32):
        if b"\x1b[7m_" in page:
            break
        guest.send(b"\t")
        page = guest.until(ready)
    else:
        raise AssertionError("Google search field missing")
    guest.send(b"\r")
    guest.until(b"q: ")
    guest.send(b"site:iana.org example domains\r")
    guest.until(ready, timeout=60)
    # Let asynchronous updates finish, then inspect the selected result URLs.
    time.sleep(10)
    for _ in range(48):
        guest.send(b"\t")
        page = guest.until(ready, timeout=60)
        if b"/httpservice/retry/enablejs" in page:
            raise AssertionError("Google returned its JavaScript compatibility page")
        if b"https://www.iana.org/" in page or b"https://iana.org/" in page:
            guest.expect(b"q", b"lixin:/pages$ ")
            return
    raise AssertionError("No IANA search result was visible")


def test(root, online, google=False):
    web = root / "web"
    web.mkdir()
    (web/"index.html").write_text('<title>Network index</title><h1>Served by Mac</h1><a href="two.html">Next over TCP</a>')
    (web/"two.html").write_text('<title>Remote two</title><p>Arrived over HTTP</p>')
    with ThreadingHTTPServer(("127.0.0.1",0),partial(SimpleHTTPRequestHandler,directory=str(web))) as server:
        Thread(target=server.serve_forever,daemon=True).start()
        try:
            with serial["boot"](root/"disk.img") as guest:
                guest.expect(b"links2 --version\r",prompt,b"Links 2.30")
                guest.expect(b"mkdir pages\r",prompt)
                guest.expect(b"cd pages\r",b"lixin:/pages$ ")
                # Create the first document with the guest's editor.
                guest.expect(b"nano hello.html\r",b"^X Exit")
                guest.expect(b'<title>Local index</title><h1>Hello &amp; welcome</h1><a href="two.html">Next</a>\x0f',b"Saved")
                guest.expect(b"\x18",b"lixin:/pages$ ")
                guest.expect(b'''echo '<title>Second</title><p>Local destination</p>' > two.html\r''',b"lixin:/pages$ ")
                guest.expect(b"links hello.html\r",ready,b"Hello & welcome")
                guest.expect(b"\r",ready,b"Local destination")
                guest.expect(b"b",ready,b"Local index")
                guest.expect(b"g",b"Open: ")
                guest.expect(f"http://10.0.2.2:{server.server_port}/index.html\r".encode(),ready,b"Served by Mac")
                guest.expect(b"\r",ready,b"Arrived over HTTP")
                guest.expect(b"b",ready,b"Network index")
                guest.expect(b"q",b"lixin:/pages$ ")
                if online:
                    verify_https(guest, root)
                if google:
                    verify_google(guest)
                guest.send(b"shutdown\r")
                assert guest.vm.wait(timeout=10)==0
        finally:
            server.shutdown()


if __name__=="__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--online", action="store_true", help="also fetch example.com and reject an untrusted certificate")
    parser.add_argument("--google", action="store_true", help="live compatibility check requiring an actual search result")
    args = parser.parse_args()
    with TemporaryDirectory() as tmp:
        test(Path(tmp), args.online,args.google)
    if args.online:
        print("Verified public DNS, HTTPS and certificate rejection.")
    print("Verified Links 2.30 ELF, guest-edited HTML, entity decoding, local and HTTP links, back and quit.")
