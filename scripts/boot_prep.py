#!/usr/bin/env python3
"""Single-process fast launch preparation for scripts/run-kernel.sh."""
import argparse
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from zoneinfo import ZoneInfo

sys.dont_write_bytecode = True
SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import disk_image  # noqa: E402


def _load_kernel_release():
    spec = importlib.util.spec_from_file_location("kernel_release", SCRIPTS / "kernel-release.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def qemu_supports_virgl(qemu_path):
    p = Path(qemu_path)
    if not p.is_file():
        return False
    resolved = str(p.resolve())
    st = p.stat()
    key = [resolved, st.st_size, st.st_mtime_ns]
    cache_dir = ROOT / "data/kernel-cache"
    cache_file = cache_dir / ".qemu-gl-cache.json"
    entries = {}
    if cache_file.is_file():
        try:
            entries = json.loads(cache_file.read_text())
            hit = entries.get(resolved)
            if isinstance(hit, list) and len(hit) == 3 and hit[:2] == key[1:]:
                return bool(hit[2])
        except (OSError, ValueError):
            entries = {}
    res = subprocess.run([resolved, "-device", "help"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    supported = b'name "virtio-gpu-gl-device",' in res.stdout
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
        entries[resolved] = [st.st_size, st.st_mtime_ns, supported]
        tmp = cache_dir / f".qemu-gl-cache.{os.getpid()}.tmp"
        tmp.write_text(json.dumps(entries))
        os.replace(tmp, cache_file)
    except OSError:
        pass
    return supported


def clock_offset(timezone):
    try:
        return datetime.now(ZoneInfo(timezone)).strftime("%z") or "+0000"
    except Exception:
        return "+0000"


def emit(pairs):
    for k, v in pairs.items():
        print(f"{k}={shlex.quote(str(v))}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--phase", choices=("early", "launch"), required=True)
    p.add_argument("--disk", default="")
    p.add_argument("--release", default="")
    p.add_argument("--auto-release", action="store_true")
    p.add_argument("--choice", default="")
    p.add_argument("--explicit-mode", default="")
    p.add_argument("--mode", default="--window")
    p.add_argument("--gpu", default="auto")
    p.add_argument("--qemu", default="")
    p.add_argument("--custom-kernel", default="")
    p.add_argument("--release-dir", default="")
    p.add_argument("--timezone", default="")
    p.add_argument("--source-boot", action="store_true")
    p.add_argument("--read-prefs-disk", default="")
    args = p.parse_args()

    kr = _load_kernel_release()

    if args.phase == "early":
        out = {}
        rel = args.release
        if rel == "latest" or args.auto_release:
            rel = kr.latest_tag()
        out["release"] = rel
        if args.read_prefs_disk:
            vals = disk_image.boot_preferences(Path(args.read_prefs_disk)).split()
            out["preferred_mode"], out["preferred_zone"], out["preferred_kernel"], out["preferred_port"], out["preferred_model"] = vals
            out["preferences_disk"] = args.read_prefs_disk
        emit(out)
        return 0

    disk = Path(args.disk)
    pref_mode, pref_zone, pref_kernel, pref_port, pref_model = disk_image.boot_preferences(disk).split()
    mode = args.explicit_mode or f"--{pref_mode}"
    timezone = args.timezone or pref_zone

    release = args.release
    if release == "latest" or (not release and args.auto_release):
        resolved = kr.latest_tag()
        if release == "latest" and not resolved:
            sys.stderr.write("没有可用的本地内核发布版本。\n")
            return 1
        release = resolved
    custom_kernel = args.custom_kernel
    has_gpu_abi = True
    use_release = False
    release_dir = args.release_dir

    if args.choice == "tcc" and release:
        bdir, has_gpu_abi = kr.boot_files_info(release, "tcc")
        custom_kernel = str(bdir / "tcc-kernel.elf")

    profile = "release"
    if mode in ("--window", "--cocoa", "--split"):
        profile = "split" if mode == "--split" else "retina"
    elif mode == "--terminal":
        profile = "window"
    variant = "ssh" if mode == "--ssh" else profile

    kernel_image = custom_kernel or f"target/aarch64-unknown-none-softfloat/{profile}/lixin-kernel"
    cpython_package = "target/user-build/cpython-port/cpython.pkg"
    if not custom_kernel:
        if release:
            bdir, has_gpu_abi = kr.boot_files_info(release, variant)
            release_dir = str(bdir)
            use_release = True
        elif release_dir and (Path(release_dir) / "manifest.json").is_file():
            use_release = True
        if use_release:
            kernel_image = f"{release_dir}/rust-{variant}.elf"
            cpython_package = f"{release_dir}/cpython.pkg"

    qemu = args.qemu or shutil.which("qemu-system-aarch64") or ""
    gpu_mode = args.gpu
    if mode in ("--window", "--cocoa", "--split"):
        if gpu_mode == "auto":
            if custom_kernel and not release:
                gpu_mode = "ramfb"
            elif release and not has_gpu_abi:
                gpu_mode = "ramfb"
        if gpu_mode in ("auto", "virgl"):
            gl_qemu = qemu
            bundled = ROOT / "Kernel/target/user-build/virtio-gpu-host/qemu-11.1.0-vee2/bin/qemu-system-aarch64"
            if not args.qemu and os.access(bundled, os.X_OK):
                gl_qemu = str(bundled)
            if gl_qemu and qemu_supports_virgl(gl_qemu):
                qemu = gl_qemu
                gpu_mode = "virgl"
            elif gpu_mode == "virgl":
                sys.stderr.write("VirGL requires a GL-enabled QEMU. Set LIXINOS_QEMU to its executable.\n")
                return 2
            else:
                gpu_mode = "ramfb"
                sys.stderr.write("QEMU 未提供 VirGL，使用 ramfb 启动。\n")

    # Prepare disk only when kernel image is already available (release or custom);
    # source builds prepare after cargo build.
    ram = "640M"
    disk_prepared = False
    if custom_kernel or use_release:
        kpath = Path(kernel_image)
        if not kpath.is_absolute():
            kpath = ROOT / "Kernel" / kpath
        if not os.access(kpath, os.R_OK):
            sys.stderr.write(f"Cannot read kernel image: {kernel_image}\n")
            return 1
        if not custom_kernel:
            disk_image.prepare(disk, False)
        else:
            kbytes = kpath.read_bytes()
            if b"LIXDISK4" in kbytes:
                disk_image.prepare(disk, False)
                ram = "1536M"
            else:
                disk_image.prepare(disk, True)
        with disk.open("rb") as f:
            magic = f.read(8)
        if magic == b"LIXBLD01":
            if custom_kernel:
                kbytes = kpath.read_bytes()
                if b"LIXBLD01" not in kbytes:
                    sys.stderr.write("This TCC image needs a loader with build-volume support.\n")
                    return 1
                ram = "24G"
            else:
                ram = "12G"
        disk_prepared = True

    emit({
        "release": release,
        "mode": mode,
        "preferred_mode": pref_mode,
        "preferred_zone": pref_zone,
        "preferred_kernel": pref_kernel,
        "preferred_port": pref_port,
        "preferred_model": pref_model,
        "timezone": timezone,
        "clock_offset": clock_offset(timezone),
        "qemu": qemu,
        "gpu_mode": gpu_mode,
        "profile": profile,
        "use_release": "true" if use_release else "false",
        "release_dir": release_dir,
        "kernel_image": kernel_image,
        "cpython_package": cpython_package,
        "custom_kernel": custom_kernel,
        "ram": ram,
        "disk_prepared": "true" if disk_prepared else "false",
    })
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"Cannot prepare LixinOS launch: {exc}\n")
        sys.exit(1)
