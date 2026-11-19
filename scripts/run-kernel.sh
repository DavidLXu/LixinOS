#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--kernel rust|tcc] [--window | --split | --cocoa | --terminal | --serial | --ssh]\n' "$0"
}

mode=
choice=
while (( $# )); do
    case $1 in
        --kernel)
            if [[ -n $choice || ( ${2:-} != rust && ${2:-} != tcc ) ]]; then
                usage >&2
                exit 2
            fi
            choice=$2
            shift
            ;;
        --window|--split|--cocoa|--terminal|--serial|--ssh)
            if [[ -n $mode ]]; then usage >&2; exit 2; fi
            mode=$1
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
mode=${mode:---window}
root=$(cd "$(dirname "$0")/.." && pwd)
tcc_loader="$root/data/tcc-os/loader.elf"
tcc_disk=${LIXINOS_TCC_DISK:-$root/data/tcc-os/system.img}

cd "$root/Kernel"
tcc_ready=false
if [[ -s $tcc_loader && -r $tcc_loader && -s $tcc_disk && -r $tcc_disk ]]; then
    tcc_ready=true
fi

# Custom images and noninteractive runs with an explicit disk are already configured.
if [[ -z $choice && -z ${LIXINOS_KERNEL:-} ]] && [[ -t 0 || -z ${LIXINOS_DISK:-} ]]; then
    case $mode in
        --window|--cocoa|--split|--terminal)
            choice=$(osascript "$root/scripts/choose-kernel.applescript" "$tcc_ready") || {
                echo '无法打开内核选择窗口。可使用 --kernel rust 或 --kernel tcc 启动。' >&2
                exit 1
            }
            [[ $choice != cancel ]] || exit 0
            ;;
        --serial|--ssh)
            if [[ -t 0 ]]; then
                printf '%s\n' '选择 LixinOS 启动内核' '  1  Rust 原版（默认）' >&2
                if "$tcc_ready"; then
                    echo '  2  TCC 自举内核（C / 汇编）' >&2
                else
                    echo '  2  TCC 自举内核（未就绪，见 Kernel/native/README.md）' >&2
                fi
                while true; do
                    printf '输入编号并回车，或按 q 回车退出 [1] > ' >&2
                    IFS= read -r answer || exit 0
                    case $answer in
                        1|'') choice=rust; break ;;
                        2) if "$tcc_ready"; then choice=tcc; break; fi ;;
                        q|Q) exit 0 ;;
                    esac
                    echo '请选择可用的内核。' >&2
                done
            fi
            ;;
    esac
fi

case $choice in
    rust) unset LIXINOS_KERNEL ;;
    tcc)
        if ! "$tcc_ready"; then
            echo '缺少 TCC 引导程序或系统磁盘，请按 Kernel/native/README.md 制盘。' >&2
            exit 1
        fi
        export LIXINOS_KERNEL="$tcc_loader" LIXINOS_DISK="$tcc_disk"
        ;;
    '') ;; # Use the existing image override or the default Rust kernel.
    *) echo '无效的内核选择。' >&2; exit 1 ;;
esac

machine=virt
case $mode in
    --window|--cocoa|--split)
        # Let Cocoa size the window to the first guest framebuffer before enabling dragging.
        display=(-name LixinOS -display cocoa,zoom-to-fit=off -serial null
                 -device ramfb -device virtio-keyboard-device,bus=virtio-mmio-bus.1
                 -device virtio-tablet-device,bus=virtio-mmio-bus.3)
        profile=retina
        if [[ $mode == --split ]]; then
            profile=split
            machine=virt,gic-version=3
        fi
        features=(--features "$profile")
        ;;
    --terminal)
        display=(-display none -serial stdio)
        profile=window
        features=(--features window-console)
        ;;
    --serial|--ssh)
        display=(-display none -serial stdio)
        profile=release
        features=()
        if [[ $mode == --ssh ]]; then features=(--features ssh-mux); fi
        ;;
esac

kernel_image=${LIXINOS_KERNEL:-target/aarch64-unknown-none-softfloat/$profile/lixin-kernel}
release_dir="$root/kernel-artifacts"
cpython_package=target/user-build/cpython-port/cpython.pkg
if [[ -z ${LIXINOS_KERNEL:-} && -f "$release_dir/manifest.json" ]]; then
    variant=$profile
    if [[ $mode == --ssh ]]; then variant=ssh; fi
    kernel_image="$release_dir/rust-$variant.elf"
    cpython_package="$release_dir/cpython.pkg"
fi
if [[ -n ${LIXINOS_KERNEL:-} || -f "$release_dir/manifest.json" ]]; then
    [[ -r "$kernel_image" ]] || { printf 'Cannot read kernel image: %s\n' "$kernel_image" >&2; exit 1; }
else
    if ! rustup_bin=$(command -v rustup); then
        rustup_bin="${CARGO_HOME:-$HOME/.cargo}/bin/rustup"
    fi
    if [[ ! -x "$rustup_bin" ]]; then
        printf '%s\n' 'Install rustup to build the LixinOS kernel. See README.md for setup.' >&2
        exit 1
    fi

    # Bash 3.2 with nounset requires this form for an empty array.
    "$rustup_bin" run stable cargo build --locked --target-dir target --profile "$profile" ${features[@]+"${features[@]}"}
fi
disk=${LIXINOS_DISK:-../data/lixinos.img}
# Keep existing standalone kernels on the layout they were built to read.
ram=640M
if [[ -z ${LIXINOS_KERNEL:-} ]]; then
    python3 ../scripts/disk_image.py "$disk"
elif LC_ALL=C grep -aq LIXDISK4 "$kernel_image"; then
    python3 ../scripts/disk_image.py "$disk"
    ram=1536M
else
    python3 ../scripts/disk_image.py --previous-layout "$disk"
fi
if [[ $(head -c8 "$disk") == LIXBLD01 ]]; then
    if [[ -n ${LIXINOS_KERNEL:-} ]]; then
        echo "The dedicated Rust build volume requires the Rust kernel." >&2
        exit 1
    fi
    ram=12G
fi
clock_offset=$(TZ="${LIXINOS_TIMEZONE:-Asia/Shanghai}" date +%z)
launcher=("$(command -v qemu-system-aarch64)")
if [[ $mode == --terminal ]]; then
    launcher=(python3 ../scripts/open-terminal.py "${launcher[@]}")
fi
if [[ "$profile" == retina || "$profile" == split ]]; then
    launcher=(python3 ../scripts/clipboard-bridge.py "${launcher[@]}")
fi
vm_args=(
    -machine "$machine" -accel hvf -cpu host -smp 1 -m "$ram" \
    "${display[@]}" -monitor none \
    -global virtio-mmio.force-legacy=false \
    -fw_cfg "name=opt/lixinos/clock-offset,string=$clock_offset" \
    -drive "if=none,id=fs,format=raw,cache=writethrough,file=${disk//,/,,}" \
    -device virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0 \
    -object rng-random,id=entropy,filename=/dev/urandom \
    -device virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5 \
    -netdev user,id=outbound,net=10.0.2.0/24,host=10.0.2.2,guestfwd=tcp:10.0.2.100:22-cmd:../scripts/gpu20-tunnel.sh \
    -device virtio-net-device,netdev=outbound,bus=virtio-mmio-bus.4 \
    -kernel "$kernel_image"
)
if [[ -n ${LIXINOS_KERNEL:-} ]]; then
    vm_args+=(-fw_cfg "name=opt/lixinos/mode,string=${mode#--}")
else
    vm_args+=(-fw_cfg "name=opt/lixinos/cpython,file=${cpython_package//,/,,}")
fi
if [[ ${DEEPSEEK_API_KEY+x} != x ]]; then
    if DEEPSEEK_API_KEY=$(/usr/bin/security find-generic-password \
        -a "$(id -un)" -s LixinOS.DeepSeek -w 2>/dev/null); then
        export DEEPSEEK_API_KEY
    fi
fi
if [[ -n ${DEEPSEEK_API_KEY:-} ]]; then
    if [[ $mode == --terminal ]]; then
        exec python3 ../scripts/agent-bridge.py --terminal "${launcher[@]}" "${vm_args[@]}"
    fi
    launcher=(python3 ../scripts/agent-bridge.py "${launcher[@]}")
fi
if [[ $mode == --ssh ]]; then
    exec python3 ../scripts/start-ssh.py "${launcher[@]}" "${vm_args[@]}"
fi
exec "${launcher[@]}" "${vm_args[@]}"
