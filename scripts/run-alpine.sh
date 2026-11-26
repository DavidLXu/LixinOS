#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--window | --serial] [--ram SIZE] [--cpus N]\n' "$0"
    printf '  --window    Open the QEMU display window (default).\n'
    printf '  --serial    Run on this terminal through the serial console.\n'
    printf '  --ram SIZE  Guest memory, e.g. 2G or 2048M (default 2G).\n'
    printf '  --cpus N    Virtual CPUs (default 4).\n'
}

mode=window
ram=2G
cpus=4
while (( $# )); do
    case $1 in
        --window|--serial)
            mode=${1#--}
            ;;
        --ram)
            if [[ $# -lt 2 || -z ${2:-} ]]; then usage >&2; exit 2; fi
            ram=$2
            shift
            ;;
        --cpus)
            if [[ $# -lt 2 || ! ${2:-} =~ ^[1-9][0-9]*$ ]]; then usage >&2; exit 2; fi
            cpus=$2
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

root=$(cd "$(dirname "$0")/.." && pwd)
disk=${ALPINE_DISK:-$root/alpine.img}
if [[ ! -f $disk ]]; then
    echo "找不到 Alpine 磁盘镜像：$disk" >&2
    exit 1
fi

qemu=${QEMU:-qemu-system-aarch64}
if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "找不到 $qemu。请先安装 QEMU。" >&2
    exit 1
fi
firmware_dir=$("$qemu" -L help | tail -n 1)
code=$firmware_dir/edk2-aarch64-code.fd
vars_template=$firmware_dir/edk2-arm-vars.fd
if [[ ! -f $code || ! -f $vars_template ]]; then
    echo "QEMU 缺少 aarch64 UEFI 固件：$firmware_dir" >&2
    exit 1
fi

vars=${ALPINE_VARS:-$root/data/alpine-vars.fd}
mkdir -p "${vars%/*}"
if [[ ! -s $vars ]]; then
    cp "$vars_template" "$vars"
fi

accel=hvf
cpu=host
if ! sysctl -n kern.hv_support 2>/dev/null | grep -qx 1; then
    accel=tcg
    cpu=max
fi

common=(
    -machine "virt,accel=$accel"
    -cpu "$cpu"
    -smp "$cpus"
    -m "$ram"
    -name "Alpine Linux"
    -drive "if=pflash,format=raw,readonly=on,file=$code"
    -drive "if=pflash,format=raw,file=$vars"
    -drive "if=virtio,format=qcow2,file=$disk"
    -netdev user,id=net0
    -device virtio-net-pci,netdev=net0
    -device virtio-gpu-pci
    -device virtio-keyboard-pci
    -device virtio-tablet-pci
)

if [[ $mode == serial ]]; then
    exec "$qemu" "${common[@]}" -nographic
fi

serial_log=${ALPINE_SERIAL_LOG:-$root/data/alpine-serial.log}
: > "$serial_log"
monitor_args=(-monitor none)
if [[ -n ${ALPINE_MONITOR:-} ]]; then
    monitor_args=(-monitor "unix:$ALPINE_MONITOR,server=on,wait=off")
fi
exec "$qemu" "${common[@]}" \
    -display cocoa \
    "${monitor_args[@]}" \
    -serial "file:$serial_log"
