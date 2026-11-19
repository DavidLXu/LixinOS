"""Run the same CPython application, REPL and file tests under both LixinOS kernels."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import hashlib
import io
from pathlib import Path
import re
import runpy
import ssl
import subprocess
import tempfile
import threading
import time
import zipfile

native = runpy.run_path(str(Path(__file__).with_name("test-native-kernel.py")))
shell = native["shell"]
command = native["command"]


def exercise_pip(run):
    info = "lixin_demo-1.0.dist-info/"
    wheel = "lixin_demo-1.0-py3-none-any.whl"
    files = {
        "lixin_demo.py": b"answer = 42\n",
        info + "METADATA": b"Metadata-Version: 2.1\nName: lixin-demo\nVersion: 1.0\n",
        info + "WHEEL": b"Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n",
    }
    records = []
    for name, data in files.items():
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
        records.append(f"{name},sha256={digest},{len(data)}")
    files[info + "RECORD"] = ("\n".join([*records, info + "RECORD,,", ""])).encode()
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for name, data in files.items():
            output.writestr(name, data)
    encoded = base64.b64encode(archive.getvalue()).decode()
    for i in range(0, len(encoded), 240):
        run(f"echo {encoded[i:i+240]} {'>' if i == 0 else '>>'} wheel.b64")
    decode = "base64.b64decode(open('wheel.b64','rb').read())"
    run(f"python3 -c \"import base64; open('{wheel}','wb').write({decode})\"")
    run("pip --version", b"pip 26.1.1")
    run(f"python3 -m pip install --no-index ./{wheel}", b"Successfully installed lixin-demo-1.0")
    run('python3 -c "import lixin_demo; print(lixin_demo.answer)"', b"\n42\n")
    run("pip3 show lixin-demo", b"Location: /lib/python3.14/site-packages")
    run("pip check", b"No broken requirements found")
    run("pip uninstall -y lixin-demo", b"Successfully uninstalled lixin-demo-1.0")
    run("python3 -c \"from importlib.util import find_spec; print(find_spec('lixin_demo'))\"",
        b"\nNone\n")


def put_bytes(run, name, data):
    encoded = base64.b64encode(data).decode()
    for offset in range(0, len(encoded), 220):
        redirect = ">" if offset == 0 else ">>"
        run(f"echo {encoded[offset:offset+220]} {redirect} transfer.b64")
    run(f"""python3 -c "import base64; open('{name}','wb').write(base64.b64decode(open('transfer.b64','rb').read()))" """)


@contextmanager
def https_fixture(directory):
    cert, key = directory / "ca.pem", directory / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=LixinOS test",
                    "-addext", "subjectAltName=IP:10.0.2.2"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"LixinOS TLS")

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield cert.read_bytes(), server.server_port
        finally:
            server.shutdown()
            thread.join()


def exercise_runtime(run, fixture):
    cert, port = fixture
    put_bytes(run, "ca.pem", cert)
    program = """import os, ssl, subprocess, sys, threading, urllib.request, urllib.error
with open('random.bin', 'wb') as f:
    f.write(b'abcdef')
    f.seek(2)
    f.write(b'XY')
    f.truncate(5)
with open('random.bin', 'r+b') as f:
    assert f.read() == b'abXYe'
    f.seek(1)
    f.truncate(8)
    assert f.tell() == 1 and f.read() == b'bXYe' + bytes(3)
os.truncate('random.bin', 4)
with open('random.bin', 'rb') as f:
    assert f.seek(1000) == 1000 and f.read() == b''
    f.seek(0)
    assert f.read() == b'abXY'
with open('random.bin','a+b') as f:
    assert f.tell() == 4 and f.read() == b''
    f.seek(0)
    assert f.read(2) == b'ab'
r, w = os.pipe()
try:
    try:
        os.lseek(w, 0, 0)
    except OSError as e:
        import errno
        assert e.errno == errno.ESPIPE
    else:
        raise AssertionError('pipe unexpectedly accepted seek')
finally:
    os.close(r)
    os.close(w)
data = b'abc' * 6000
p = subprocess.run(['/bin/cat'], input=data, capture_output=True, check=True)
assert p.stdout == data and p.stderr == b''
p = subprocess.run([sys.executable, '-c', 'import sys;print(7,file=sys.stderr);sys.exit(3)'], capture_output=True)
assert p.returncode == 3 and p.stderr.strip() == b'7' and p.stdout == b''
ready, done = threading.Event(), threading.Event()
thread = threading.Thread(target=lambda: (ready.wait(), done.set()))
thread.start()
ready.set()
assert done.wait(3)
thread.join()
url = 'https://10.0.2.2:PORT/test'
ctx = ssl.create_default_context(cafile='ca.pem')
with urllib.request.urlopen(url, context=ctx, timeout=8) as r:
    assert r.status == 200 and r.read() == b'LixinOS TLS'
try:
    urllib.request.urlopen(url, timeout=8)
except urllib.error.URLError as e:
    assert isinstance(e.reason, ssl.SSLCertVerificationError), repr(e)
else:
    raise AssertionError('untrusted certificate was accepted')
print('runtime checks passed')
"""
    put_bytes(run, "runtime_test.py", program.replace("PORT", str(port)).encode())
    run("python3 runtime_test.py", b"\nruntime checks passed\n")


def exercise(s, label, fixture):
    command(s, "python3 --version", expected=b"Python 3.14.5")
    work = "cpython-" + label
    command(s, "mkdir " + work)
    prompt = f"lixin:/{work}$ ".encode()
    command(s, "cd " + work, prompt=prompt)

    def run(line, expected=b"", status=0):
        command(s, line, status, expected, prompt)

    exercise_runtime(run, fixture)
    exercise_pip(run)
    run("echo 'def square(x): return x*x' > helper.py")
    run("echo 'from helper import square' > app.py")
    run("echo 'import sys; print(sys.implementation.name, sys.platform, square(int(sys.argv[1])))' >> app.py")
    run("python3 app.py 11", b"\ncpython lixinos 121\n")
    run('python3 -c "import math,json; print(json.dumps([math.sqrt(49)]))"', b"\n[7.0]\n")
    run('python3 -c "import ssl; print(len(ssl.RAND_bytes(32)), len(ssl.create_default_context().get_ca_certs()) > 0)"',
        b"\n32 True\n")
    run("echo 'print(6 * 7)' | python3 -", b"\n42\n")
    run('python3 -c "with open(\'note\', \'w\') as f: f.write(\'saved\')"')
    run('python3 -c "with open(\'note\', \'a\') as f: f.write(\'!\')"')
    run('python3 -c "import os; print(open(\'note\').read(), \'note\' in os.listdir(\'.\'))"', b"\nsaved! True\n")
    run('python3 -c "import time; t=time.monotonic(); time.sleep(.02); print(time.monotonic()-t>=.019)"', b"\nTrue\n")
    # Two guest processes keep independent interpreter heaps and thread-local state.
    run("echo 'import os, time' > worker.py")
    run("echo 'while not os.path.exists(\"go\"): time.sleep(.01)' >> worker.py")
    run("echo 'with open(\"done\", \"w\") as f: f.write(str(sum(range(10))))' >> worker.py")
    run("python3 worker.py &", b"[pid ")
    run("python3 -c \"open('go','w').close()\"")
    run("echo 'import os, time' > wait.py")
    run("echo 'for _ in range(200):' >> wait.py")
    run("echo '    if os.path.exists(\"done\") and os.stat(\"done\").st_size: break' >> wait.py")
    run("echo '    time.sleep(.01)' >> wait.py")
    run("echo 'print(open(\"done\").read())' >> wait.py")
    run("python3 wait.py", b"\n45\n")
    run("echo 'raise ValueError(123)' > fail.py")
    s.send(b"python3 fail.py\r")
    trace = re.sub(rb"\x1b\[[;0-9]*m", b"", s.until(prompt))
    assert b"Traceback (most recent call last):" in trace and b'/fail.py", line 1' in trace
    assert b"ValueError: 123" in trace, trace
    s.expect(b"echo $?\r", prompt, b"\n1\n")
    s.expect(b"python3\r", b">>> ", b"Python 3.14.5")
    s.expect(b"from helper import square\r", b">>> ")
    s.expect(b"square(12)\r", b">>> ", b"\n144\n")
    s.expect(b"while True: pass\r", b"... ")
    s.send(b"\r")
    time.sleep(.2)
    s.expect(b"\x03", b">>> ", b"KeyboardInterrupt")
    s.expect(b"square(13)\r", b">>> ", b"\n169\n")
    s.expect(b"\x04", prompt)
    run('python -c "import sys; print(sys.implementation.name)"', b"micropython")
    command(s, "cd /", prompt=shell)


def main():
    with tempfile.TemporaryDirectory() as temporary, https_fixture(Path(temporary)) as fixture:
        directory = Path(temporary)
        disk = directory / "guest.img"
        with native["guest"]["boot"](disk, [native["root"] / "scripts/run-kernel.sh",
                                           "--kernel", "rust", "--serial"]) as s:
            exercise(s, "rust", fixture)
            command(s, "tcc /src/tinycc/bootstrap.c -o /bin/tcc")
            command(s, "tcc --bootloader @/src/kernel/loader-sources -o /boot/loader.elf")
            command(s, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
            native["shutdown"](s)
        loader = directory / "loader.elf"
        loader.write_bytes(native["read_file"](disk, "/boot/loader.elf"))
        with native["native_boot"](loader, disk) as s:
            s.until(shell)
            command(s, "tcc -DNDEBUG -I/src/micropython -I/src/tinycc/sdk @/src/micropython/sources.txt -o /bin/python")
            exercise(s, "tcc", fixture)
            command(s, "cat /cpython-rust/random.bin", expected=b"abXY")
            command(s, "cat /cpython-rust/note", expected=b"saved!")
            native["shutdown"](s)
    print("Verified native ARM64 CPython, wheel installation and removal with pip, scripts, local imports, REPL, file seek/truncate, Ctrl+C, threaded events, subprocess pipes, TLS verification, sleep and tracebacks on both kernels.")


if __name__ == "__main__":
    main()
