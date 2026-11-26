"""Install a target CPython C extension from a wheel on private copies of a daily disk."""
import argparse
import os
import base64
import hashlib
import io
from pathlib import Path
import runpy
import subprocess
import tempfile
import zipfile

from disk_image import edit_files

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "Kernel/target/user-build"
THREADS = runpy.run_path(str(ROOT / "scripts/test-kernel-threads.py"))
INSTALL = runpy.run_path(str(ROOT / "User/gcc/install.py"))["package_edit"]
TAG = "cp314-cp314-lixinos_0_1_0_aarch64"
NAME = f"lxwheel-1.0-{TAG}.whl"


def wheel(work):
    py = BUILD / "cpython-port"
    src = work / "extension.c"
    src.write_text("""#include <Python.h>
static PyObject *answer(PyObject *self, PyObject *args) {
    long a, b;
    if (!PyArg_ParseTuple(args, "ll", &a, &b)) return NULL;
    return PyLong_FromLong(a + b);
}
static PyMethodDef methods[] = {{"add", answer, METH_VARARGS, NULL}, {0}};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "lxwheel", NULL, -1, methods};
PyMODINIT_FUNC PyInit_lxwheel(void) { return PyModule_Create(&module); }
""")
    extension = work / "lxwheel.so"
    subprocess.run([py/"cc", "-I"+str(py/"target"), "-I"+str(py/"Python-3.14.5/Include"),
                    "-fPIC", "-shared", "-nostdlib", "-Wl,-z,max-page-size=4096",
                    src, "-o", extension], check=True)
    info = "lxwheel-1.0.dist-info/"
    files = {"lxwheel.so": extension.read_bytes(),
             info+"METADATA": b"Metadata-Version: 2.1\nName: lxwheel\nVersion: 1.0\n",
             info+"WHEEL": f"Wheel-Version: 1.0\nRoot-Is-Purelib: false\nTag: {TAG}\n".encode()}
    records = []
    for path, data in files.items():
        digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
        records.append(f"{path},sha256={digest},{len(data)}")
    files[info+"RECORD"] = ("\n".join([*records, info+"RECORD,,", ""])).encode()
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        for path, data in files.items():
            archive.writestr(path, data)
    return buf.getvalue()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for flag in ("disk", "rust", "tcc"):
        p.add_argument("--"+flag, type=Path, required=True)
    p.add_argument("--kernel", choices=("rust", "tcc"), action="append")
    args = p.parse_args()
    toolchain = next((ROOT/"Kernel/target/user-build").glob("arm-gnu-toolchain-*-aarch64-none-elf"))
    os.environ["PATH"] = str(toolchain/"bin")+os.pathsep+os.environ["PATH"]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        data = wheel(work)
        source = work/"source.img"
        subprocess.run(["cp", "-c", args.disk, source], check=True)
        def stage(rows):
            for pkg, name in (("package", "dynlink"), ("python-package", "cpython-native")):
                INSTALL(BUILD/"dynlink-port"/pkg, name)(rows)
            rows[("/tmp/"+NAME).encode()] = bytes(8), data
        edit_files(source, stage)
        for tag, kernel, ram in (("rust", args.rust, "12G"), ("tcc", args.tcc, "24G")):
            if args.kernel and tag not in args.kernel:
                continue
            disk = work/(tag+".img")
            subprocess.run(["cp", "-c", source, disk], check=True)
            with THREADS["boot"](disk, THREADS["args"](kernel,disk,2,ram),prompt=b"$ ") as guest:
                for command, expect in [
                    ("pip3 --version", b"python 3.14"),
                    (f"pip install --disable-pip-version-check --no-index --no-deps --no-compile --target /tmp/wheel /tmp/{NAME}", b"Successfully installed lxwheel-1.0"),
                    ('python3 -c "import sys; sys.path.insert(0,\'/tmp/wheel\'); import lxwheel; print(lxwheel.__file__,lxwheel.add(20,22))"', b"/tmp/wheel/lxwheel.so 42")]:
                    guest.send(command.encode()+b"\r")
                    out = guest.until(b"$ ", timeout=600)
                    print(tag, out.decode(errors="replace"), flush=True)
                    assert expect in out
                    guest.send(b"echo $?\r")
                    assert b"\n0\n" in guest.until(b"$ ")
                THREADS["native"]["shutdown"](guest, timeout=240)


if __name__ == "__main__":
    main()
