#!/usr/bin/env python3
"""Serve one bounded shell job in a fresh, networkless LixinOS VM."""
import base64
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import runpy
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
CACHE_DIR = Path.home() / 'Library/Caches/LixinOS/shell-workers'
FRAME = 12 << 20
DATA = 8 << 20
FILES = 1024
OUTPUT = 64 << 10
PROMPT = b"lixin:/home/lixin$ "
DEFAULT_STAMP = struct.pack('<II', 1, 1)


def relative(name):
    return (isinstance(name, str) and 0 < len(name.encode()) <= 480 and '\0' not in name
            and all(s not in ('', '.', '..', '.codex', '.git', '.agents') for s in name.split('/')))


def write_volume(path, rows, trailer=b''):
    body = bytearray(struct.pack('<I', len(rows)))
    for name, (stamp, value) in sorted(rows.items()):
        encoded = name.encode()
        body += struct.pack('<II', len(encoded), 0xffffffff if value is None else len(value))
        body += stamp
        body += encoded
        if value is not None:
            body += value
    if trailer:
        body += trailer
    header = b'LIXINFS3' + struct.pack('<QII', 0, len(body), disk.checksum(body))
    with path.open('xb') as out:
        out.truncate(disk.IMAGE_SIZE)
        out.write(disk.HEADER)
        out.write((header + struct.pack('<I', disk.checksum(header))).ljust(512, b'\0'))
        out.write(body)


def make_disk(path, files, cwd, cmd, marker):
    rows = {
        '/': (DEFAULT_STAMP, None),
        '/home': (DEFAULT_STAMP, None),
        '/home/lixin': (DEFAULT_STAMP, None),
        '/workspace': (DEFAULT_STAMP, None),
        '/job.c': (DEFAULT_STAMP, (ROOT / 'User/codex-exec/worker.c').read_bytes()),
        '/job.cwd': (DEFAULT_STAMP, ('/workspace' + ('/' + cwd if cwd else '')).encode()),
        '/job.cmd': (DEFAULT_STAMP, cmd.encode()),
        '/job.marker': (DEFAULT_STAMP, marker),
    }
    rows.update(('/workspace/' + k, (DEFAULT_STAMP, v)) for k, v in files.items())
    write_volume(path, rows)


def run_vm(image, kernel, package, run, compile_shell, marker, timeout, peer, bootstrap_tcc=False):
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
        if bootstrap_tcc:
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


def resolve_boot_files(release, variant):
    kr = runpy.run_path(str(ROOT / 'scripts/kernel-release.py'))
    directory, _ = kr['boot_files_info'](release, variant)
    return Path(directory)


def prune_base_rows(rows):
    """Drop unrelated OS source trees and GUI-only binaries not usable in a headless worker VM."""
    skip_bins = {
        '/bin/desktop', '/bin/links', '/bin/lust', '/bin/text-editor',
        '/bin/paint', '/bin/kilo', '/bin/dte', '/bin/doom', '/bin/classicube',
        '/lib/agent-test.bin', '/lib/foreground.elf',
    }
    keep_src = ('/src/tcc', '/src/tinycc/builtin', '/src/tinycc/include')
    def keep(k):
        if k.startswith('/workspace/') or k in skip_bins:
            return False
        if k in ('/job.c', '/job.cwd', '/job.cmd', '/job.marker', '/job.status'):
            return False
        if k.startswith('/src/'):
            return k in ('/src/tinycc',) or any(k == p or k.startswith(p + '/') for p in keep_src)
        return True
    return {k: v for k, v in rows.items() if keep(k)}


def cached_base_disk(rust_dir, flavor, peer):
    """Build once and cache a compact prepared worker volume with /bin/codex-job precompiled."""
    worker_src = (ROOT / 'User/codex-exec/worker.c').read_bytes()
    rust_elf = rust_dir / 'rust-release.elf'
    pkg_file = rust_dir / 'cpython.pkg'
    key_material = f"v3|{rust_dir.name}|{flavor}|{rust_elf.stat().st_mtime_ns}|{pkg_file.stat().st_mtime_ns}|".encode() + worker_src
    key = hashlib.sha256(key_material).hexdigest()[:16]
    CACHE_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
    base_path = CACHE_DIR / f'base-{flavor}-{key}.img'
    if base_path.is_file():
        return base_path
    lock_path = CACHE_DIR / f'base-{flavor}-{key}.lock'
    with lock_path.open('ab') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if base_path.is_file():
            return base_path
        with tempfile.TemporaryDirectory(prefix='lixinos-base-', dir=CACHE_DIR) as temp:
            raw_stage = Path(temp) / 'raw.img'
            compact_stage = Path(temp) / 'base.img'
            make_disk(raw_stage, {}, '', '', b'base-init')
            run_vm(raw_stage, rust_elf, pkg_file, run=False, compile_shell=True,
                   marker=b'base-init', timeout=60, peer=peer,
                   bootstrap_tcc=(flavor == 'tcc'))
            rows, trailer = disk.disk_entries(raw_stage)
            write_volume(compact_stage, prune_base_rows(rows), trailer)
            os.replace(compact_stage, base_path)
    return base_path


def make_job_disk_from_base(path, base_path, files, cwd, cmd, marker):
    rows, trailer = disk.disk_entries(base_path)
    for drop in list(rows):
        if drop.startswith('/workspace/') or drop in ('/job.c', '/job.cwd', '/job.cmd', '/job.marker', '/job.status'):
            del rows[drop]
    rows['/workspace'] = (DEFAULT_STAMP, None)
    rows['/job.cwd'] = (DEFAULT_STAMP, ('/workspace' + ('/' + cwd if cwd else '')).encode())
    rows['/job.cmd'] = (DEFAULT_STAMP, cmd.encode())
    rows['/job.marker'] = (DEFAULT_STAMP, marker)
    for k, v in files.items():
        rows['/workspace/' + k] = (DEFAULT_STAMP, v)
    write_volume(path, rows, trailer)


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
    rust = resolve_boot_files(release, 'release')
    base_disk = cached_base_disk(rust, flavor, peer)
    kernel = (resolve_boot_files(release, 'tcc') / 'tcc-kernel.elf') if flavor == 'tcc' else (rust / 'rust-release.elf')
    package = None if flavor == 'tcc' else (rust / 'cpython.pkg')
    marker = ('end-' + secrets.token_hex(16)).encode()
    with tempfile.TemporaryDirectory(prefix='lixinos-codex-') as temp:
        image = Path(temp) / 'job.img'
        make_job_disk_from_base(image, base_disk, files, cwd, cmd, marker)
        output = run_vm(image, kernel, package, run=True, compile_shell=False,
                        marker=marker, timeout=timeout, peer=peer)
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
    CACHE_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
    for index in range(4):
        with (CACHE_DIR / str(index)).open('ab') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                continue
            yield
            return
    raise ValueError('all isolated shells are busy')


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
