"""Split ELF load segments at the guest file limit and assemble the userland package."""
from pathlib import Path
import base64
import csv
import hashlib
import shutil
import struct
import subprocess
import sys
import zipfile
import sysconfig

port = Path(__file__).resolve().parent
build = Path(sys.argv[1]).resolve()
source = build / "Python-3.14.5"
package = build / "package"
parts = package / "lib/cpython-3.14.5"
parts.mkdir(parents=True, exist_ok=True)
image = (build / "cpython.elf").read_bytes()
entry, table = struct.unpack_from("<QQ", image, 24)
count, = struct.unpack_from("<H", image, 56)
segments = [struct.unpack_from("<IIQQQQQQ", image, table + 56*i) for i in range(count)]
rx, rw = (p for p in segments if p[0] == 1)
assert rx[1:4] == (5, 0, 0x78000000) and rw[1] == 6 and rw[3] % (2<<20) == 0
assert rx[3] <= entry < rx[3]+rx[5] <= rw[3] and rw[3]+rw[6]+(2<<20) < 0x80000000
lines = [f"#define {name} {value:#x}UL" for name, value in (
    ("IMAGE_BASE", rx[3]), ("TEXT_BYTES", rw[3]-rx[3]), ("ENTRY_PC", entry), ("PHNUM", count),
    ("STACK_TOP", ((rw[3]+rw[6]+4095) & ~4095) + (2<<20)))]
lines.append("static const struct piece pieces[] = {")
keep = set()
for seg in (rx, rw):
    for offset in range(0, seg[5], 1<<20):
        data = image[seg[2]+offset:seg[2]+min(seg[5], offset+(1<<20))]
        name = "segment-" + hashlib.sha256(data).hexdigest()[:20]
        (parts/name).write_bytes(data)
        keep.add(name)
        digest = 2166136261
        for b in data: digest = ((digest ^ b) * 16777619) & 0xffffffff
        lines.append(f'    {{"/lib/cpython-3.14.5/{name}", {seg[3]+offset:#x}, {len(data)}, {digest:#x}}},')
(build/"layout.h").write_text("\n".join([*lines, "};", ""]))
binary = package/"bin/python3"
binary.parent.mkdir(exist_ok=True)
compiler = ["aarch64-none-elf-gcc","-Os","-fno-builtin","-ffreestanding","-ffunction-sections",
    "-fdata-sections","-nostdlib","-Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-s",
    "-T",port.parent/"ls/link.ld","-I",build,port/"loader.c",port.parent/"tcc/api.c"]
for name in ("python3", "pip", "pip3"):
    subprocess.run([*compiler, *(["-DLX_PIP"] if name != "python3" else []), "-o", binary.with_name(name)], check=True)
stdlib = package/"lib/python3.14"
stdlib.mkdir(exist_ok=True)
packages = {"encodings","importlib","collections","json","re","annotationlib","string","tomllib",
            "sysconfig","zipfile","compression","http","pathlib","xml","xmlrpc","multiprocessing","asyncio","ctypes","pydoc_data","_pyrepl","urllib","html","email","unittest","concurrent","logging"}
for p in sorted((source/"Lib").rglob("*.py")):
    rel = p.relative_to(source/"Lib")
    if len(rel.parts) > 1 and (rel.parts[0] not in packages or "test" in rel.parts): continue
    dest = stdlib / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(p, dest)
site = stdlib/"site-packages"
with zipfile.ZipFile(next((source/"Lib/ensurepip/_bundled").glob("pip-*.whl"))) as wheel:
    wheel.extractall(site)
shutil.copyfile(port/"lixin_pip_http.py", stdlib/"lixin_pip_http.py")


def replace_once(path, original, replacement):
    text = path.read_text()
    if text.count(original) != 1:
        raise ValueError(f"unexpected upstream pip source in {path}")
    path.write_text(text.replace(original, replacement))


# Pip's standard resolver and installer use the guest adapter for all origins.
internal = site/"pip/_internal"
replace_once(internal/"network/session.py", '        self.mount("https://", secure_adapter)',
          '        from lixin_pip_http import GuestAdapter\n'
          '        secure_adapter = insecure_adapter = self._trusted_host_adapter = GuestAdapter()\n'
          '        self.mount("https://", secure_adapter)')
replace_once(internal/"cli/index_command.py", "import contextlib", "import contextlib\nimport sys")
replace_once(internal/"cli/index_command.py", '    try:\n        import ssl',
             '    if sys.platform == "lixinos":\n        return None  # GuestAdapter initializes TLS.\n    try:\n        import ssl')
replace_once(internal/"models/search_scope.py", "import logging", "import logging\nimport sys")
replace_once(internal/"models/search_scope.py", 'if not has_tls():',
          'if sys.platform != "lixinos" and not has_tls():')
# LixinOS mounts its prefix at /. POSIX normpath preserves exactly two leading
# slashes; pip correctly rejects those paths when checking wheel containment.
replace_once(stdlib/"sysconfig/__init__.py", "        res[key] = os.path.normpath(_subst_vars(value, vars))",
             "        path = _subst_vars(value, vars)\n"
             "        if path.startswith('//'): path = '/' + path.lstrip('/')\n"
             "        res[key] = os.path.normpath(path)")
replace_once(stdlib/"subprocess.py", "            if (_USE_POSIX_SPAWN", """            if sys.platform == 'lixinos':
                import _lixin_process
                if (preexec_fn or pass_fds or env is not None or start_new_session or
                        process_group != -1 or uid is not None or gid is not None or gids or umask != -1):
                    raise NotImplementedError("unsupported LixinOS child process attributes")
                cwd = os.fsdecode(cwd) if cwd is not None else os.getcwd()
                executable = os.fsdecode(executable)
                if shell and executable == '/bin/sh':
                    executable = '/bin/bash'
                from shutil import which
                if not os.path.dirname(executable):
                    executable = which(executable, path='/bin')
                    if executable is None:
                        raise FileNotFoundError(errno.ENOENT, 'executable not found', args[0])
                if not os.path.isfile(os.path.join(cwd, executable)):
                    raise FileNotFoundError(errno.ENOENT, 'executable not found', executable)
                self.pid = _lixin_process.spawn(executable, cwd, list(map(os.fsdecode, args)),
                                                p2cread, c2pwrite, errwrite)
                self._child_created = True
                self._close_pipe_fds(p2cread, p2cwrite, c2pread, c2pwrite, errread, errwrite)
                return
            if (_USE_POSIX_SPAWN""")
# RECORD describes the installed, ported files, including their hashes and sizes.
record = next(site.glob("pip-*.dist-info/RECORD"))
with record.open(newline="") as stream:
    rows = list(csv.reader(stream))
for row in rows:
    if row[0] in ("pip/_internal/network/session.py", "pip/_internal/cli/index_command.py",
                  "pip/_internal/models/search_scope.py"):
        data = (site/row[0]).read_bytes()
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
        row[1:] = ["sha256=" + digest, str(len(data))]
with record.open("w", newline="") as stream:
    csv.writer(stream).writerows(rows)

# Installed runtime configuration comes from the target, without host SDK paths.
config = sysconfig._parse_makefile(str(build/"target/Makefile"))
runtime_keys = ("VERSION", "LDVERSION", "ABIFLAGS", "SOABI", "EXT_SUFFIX", "MULTIARCH",
                "PLATLIBDIR", "LIBDIR", "LIBRARY", "LDLIBRARY", "Py_ENABLE_SHARED")
config = {k: config[k] for k in runtime_keys if k in config}
with (build/"target/pyconfig.h").open() as header:
    sysconfig.parse_config_h(header, config)
(stdlib/"_lixin_sysconfig.py").write_text("build_time_vars = " + repr(config) + "\n")
(package/"tmp").mkdir(exist_ok=True)
shutil.copyfile(build/"zlib-1.3.1/LICENSE", parts/"LICENSE.zlib")
shutil.copyfile(build.parent/"bearssl-0.6/LICENSE.txt", parts/"LICENSE.BearSSL")
shutil.copyfile(site/"pip/_vendor/certifi/cacert.pem", parts/"roots.pem")
shutil.copyfile(build/"openssl-3.5.9/LICENSE.txt", parts/"LICENSE.openssl")
shutil.copyfile(source/"LICENSE",parts/"LICENSE.python")
shutil.copyfile(build/"musl-1.2.5/COPYRIGHT",parts/"COPYRIGHT.musl")

for old in parts.glob("segment-*"):
    if old.name not in keep:
        old.unlink()
# LIXINFS1 is also the boot package format. Both the disk and the optional
# firmware payload go through the kernel's existing filesystem decoder.
entries = sorted(package.rglob("*"))
bundle = build / "cpython.pkg"
with bundle.with_suffix(".tmp").open("wb") as stream:
    stream.write(struct.pack("<I", len(entries) + 1))
    stream.write(struct.pack("<II", 1, 0xffffffff) + b"/")
    for entry in entries:
        name = ("/" + entry.relative_to(package).as_posix()).encode()
        size = entry.stat().st_size if entry.is_file() else 0xffffffff
        if size != 0xffffffff and size > (100 << 20):
            raise ValueError(f"file exceeds the guest limit: {entry}")
        stream.write(struct.pack("<II", len(name), size) + name)
        if entry.is_file():
            stream.write(entry.read_bytes())
bundle.with_suffix(".tmp").replace(bundle)
