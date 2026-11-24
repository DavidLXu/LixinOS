#!/usr/bin/env python3
"""Serve one bounded shell job in a fresh, networkless LixinOS VM."""
import base64
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

import disk_image as disk

ROOT = Path(__file__).resolve().parents[1]
FRAME = 12 << 20
DATA = 8 << 20
FILES = 1024
OUTPUT = 64 << 10
PROMPT = b"lixin:/home/lixin$ "


def relative(name):
    return (isinstance(name, str) and 0 < len(name.encode()) <= 480 and '\0' not in name
            and all(s not in ('', '.', '..', '.codex', '.git', '.agents') for s in name.split('/')))


def make_disk(path, files, cwd, cmd, marker):
    rows = {'/': None, '/home': None, '/home/lixin': None, '/workspace': None,
            '/job.c': (ROOT / 'User/codex-exec/worker.c').read_bytes(),
            '/job.cwd': ('/workspace' + ('/' + cwd if cwd else '')).encode(),
            '/job.cmd': cmd.encode(), '/job.marker': marker}
    rows.update(('/workspace/' + k, v) for k, v in files.items())
    body = bytearray(struct.pack('<I', len(rows)))
    for name, value in sorted(rows.items()):
        encoded = name.encode()
        body += struct.pack('<4I', len(encoded), 0xffffffff if value is None else len(value), 1, 1)
        body += encoded
        if value is not None:
            body += value
    header = b'LIXINFS3' + struct.pack('<QII', 0, len(body), disk.checksum(body))
    with path.open('xb') as out:
        out.truncate(disk.IMAGE_SIZE)
        out.write(disk.HEADER)
        out.write((header + struct.pack('<I', disk.checksum(header))).ljust(512, b'\0'))
        out.write(body)


def run_vm(image, kernel, package, run, compile_shell, marker, timeout, peer):
    args = [shutil.which('qemu-system-aarch64'), '-nodefaults', '-no-user-config',
            '-machine', 'virt', '-accel', 'hvf', '-cpu', 'host', '-m', '1536M', '-smp', '1',
            '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot',
            '-chardev', 'stdio,id=tty,signal=off', '-serial', 'chardev:tty',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'if=none,id=fs,format=raw,file={image}',
            '-device', 'virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0',
            '-object', 'rng-random,id=rng,filename=/dev/urandom',
            '-device', 'virtio-rng-device,rng=rng,bus=virtio-mmio-bus.5',
            '-kernel', str(kernel)]
    if package:
        args += ['-fw_cfg', f'name=opt/lixinos/cpython,file={package}']
    vm = subprocess.Popen(args, cwd=image.parent, env={'PATH': '/usr/bin:/bin'},
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    pending = bytearray()

    def send(line):
        vm.stdin.write(line + b'\r')
        vm.stdin.flush()

    def until(token, deadline, limit=OUTPUT):
        while token not in pending:
            wait = deadline - time.monotonic()
            if wait <= 0:
                raise TimeoutError('isolated VM timed out')
            watched = [vm.stdout] if peer is None else [vm.stdout, peer]
            ready, _, _ = select.select(watched, [], [], wait)
            if not ready:
                raise TimeoutError('isolated VM timed out')
            if peer in ready:
                raise ConnectionError('shell client disconnected or sent a second frame')
            chunk = os.read(vm.stdout.fileno(), 4096)
            if not chunk:
                raise ValueError('isolated VM exited before completing the job')
            pending.extend(chunk)
            if len(pending) > limit:
                raise ValueError('isolated VM serial output limit exceeded')
        end = pending.index(token)
        data = bytes(pending[:end]).replace(b'\r', b'')
        del pending[:end + len(token)]
        return data

    try:
        until(PROMPT, time.monotonic() + 20)
        if compile_shell and not run:
            send(b'tcc /src/tinycc/bootstrap.c -o /bin/tcc')
            until(PROMPT, time.monotonic() + 40)
            send(b'echo $?')
            if b'\n0\n' not in until(PROMPT, time.monotonic() + 5):
                raise ValueError('could not bootstrap isolated guest compiler')
        if compile_shell:
            send(b'tcc /job.c -o /bin/codex-job')
            until(PROMPT, time.monotonic() + 30)
            send(b'echo $?')
            if b'\n0\n' not in until(PROMPT, time.monotonic() + 5):
                raise ValueError('could not build isolated guest shell')
            for dest in (b'/bin/bash', b'/bin/sh'):
                send(b'cp /bin/codex-job ' + dest)
                until(PROMPT, time.monotonic() + 10)
                send(b'echo $?')
                if b'\n0\n' not in until(PROMPT, time.monotonic() + 5):
                    raise ValueError('could not install isolated script interpreter')
        output = b''
        if run:
            send(b'/bin/codex-job --codex-job')
            output = until(marker, time.monotonic() + timeout).partition(b'\n')[2]
            until(PROMPT, time.monotonic() + 5)
        send(b'q')
        if vm.wait(timeout=15) != 0:
            raise ValueError('isolated VM failed to save')
        return output
    finally:
        if vm.poll() is None:
            vm.kill()
        vm.wait()


def job(request, release, flavor, peer=None):
    encoded = request['files']
    cwd, cmd = request['cwd'], request['cmd']
    if (not isinstance(encoded, dict) or len(encoded) > FILES or (cwd and not relative(cwd))
            or not isinstance(cmd, str) or '\0' in cmd or len(cmd.encode()) > 8192):
        raise ValueError('invalid workspace or command')
    files = {}
    for name, value in encoded.items():
        if not relative(name):
            raise ValueError('invalid workspace path')
        files[name] = None if value is None else base64.b64decode(value, validate=True)
    if cwd and (cwd not in files or files[cwd] is not None):
        raise ValueError('invalid working directory')
    for name in files:
        parent = name.rpartition('/')[0]
        if parent and (parent not in files or files[parent] is not None):
            raise ValueError('invalid workspace parent')
    if sum(len(v) for v in files.values() if v is not None) > DATA:
        raise ValueError('workspace exceeds 8 MiB')
    timeout = max(1, min(120, int(request['timeout'])))
    def artifacts(variant):
        result = subprocess.check_output([sys.executable, ROOT / 'scripts/kernel-release.py',
                                          'boot-files', release, variant], text=True)
        return Path(result.strip())
    rust = artifacts('release')
    marker = ('end-' + secrets.token_hex(16)).encode()
    with tempfile.TemporaryDirectory(prefix='lixinos-codex-') as temp:
        image = Path(temp) / 'job.img'
        make_disk(image, files, cwd, cmd, marker)
        output = run_vm(image, rust/'rust-release.elf', rust/'cpython.pkg', flavor == 'rust',
                        True, marker, timeout, peer)
        if flavor == 'tcc':
            output = run_vm(image, artifacts('tcc')/'tcc-kernel.elf', None, True, False, marker, timeout, peer)
        rows, _ = disk.disk_entries(image)
        status = rows.get('/job.status', (None, None))[1]
        if status is None or len(status) != 1 or rows.get('/workspace') is None or rows['/workspace'][1] is not None:
            raise ValueError('isolated command produced an incomplete volume')
        final = {name[11:]: v for name, (_, v) in rows.items() if name.startswith('/workspace/')}
        if len(final) > FILES or sum(len(v) for v in final.values() if v is not None) > DATA:
            raise ValueError('result exceeds workspace limit')
        changed = {}
        for name, value in final.items():
            if name in files and value == files[name]:
                continue
            if not relative(name):
                raise ValueError('unsupported output path')
            changed[name] = None if value is None else base64.b64encode(value).decode()
        return dict(status=status[0], output=output.decode(errors='replace'), changed=changed,
                    removed=sorted(files.keys()-final.keys()))


@contextmanager
def slot():
    cache = Path.home()/'Library/Caches/LixinOS/shell-workers'
    cache.mkdir(mode=0o700, parents=True, exist_ok=True)
    for index in range(2):
        with (cache / str(index)).open('ab') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                continue
            yield
            return
    raise ValueError('both isolated shells are busy')


def deadline_expired(signum, frame):
    raise TimeoutError('incomplete shell request')


def main():
    signal.signal(signal.SIGALRM, deadline_expired)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    try:
        with slot():
            signal.alarm(30)
            size, = struct.unpack('>I', sys.stdin.buffer.read(4))
            if not 0 < size <= FRAME:
                raise ValueError('invalid frame length')
            raw = sys.stdin.buffer.read(size)
            if len(raw) != size:
                raise ValueError('incomplete request')
            signal.alarm(0)
            result = job(json.loads(raw), os.environ.get('LIXINOS_SHELL_RELEASE') or 'latest',
                         os.environ.get('LIXINOS_SHELL_FLAVOR', 'rust'), sys.stdin.buffer)
    except (ValueError, KeyError, TypeError, OSError, struct.error, subprocess.SubprocessError) as exc:
        result = {'error': str(exc)[:300]}
    finally:
        signal.alarm(0)
    body = json.dumps(result, separators=(',', ':')).encode()
    sys.stdout.buffer.write(struct.pack('>I', len(body)) + body)


if __name__ == '__main__':
    main()
