#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--kernel rust|tcc] [--release latest|TAG | --source] [--window | --split | --cocoa | --terminal | --serial | --ssh]\n' "$0"
}

mode=
choice=
release=
source_boot=false
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
        --release)
            if [[ $# -lt 2 || -n $release ]] || "$source_boot"; then
                usage >&2
                exit 2
            fi
            release=$2
            shift
            ;;
        --source)
            if [[ -n $release ]] || "$source_boot"; then
                usage >&2
                exit 2
            fi
            source_boot=true
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
explicit_mode=$mode
mode=${mode:---window}
root=$(cd "$(dirname "$0")/.." && pwd)
rust_default=$root/data/lixinos.img
tcc_default=$root/data/tcc-os/system.img
if [[ -f $root/data/shared.img ]]; then
    rust_default=$root/data/shared.img
    tcc_default=$rust_default
fi
work_disk=${LIXINOS_WORKDIR:-"$HOME/Library/Application Support/LixinOS"}/daily.img
if [[ -f $work_disk && ( ! -f $root/kernel-artifacts/manifest.json || -n ${LIXINOS_WORKDIR:-} ) ]]; then
    rust_default=$work_disk
    tcc_default=$work_disk
elif [[ ! -f $work_disk && -f ${work_disk%/*}/.daily-configured &&
        ! -f $root/kernel-artifacts/manifest.json && -z ${LIXINOS_DISK:-} &&
        ( $choice != tcc || -z ${LIXINOS_TCC_DISK:-} ) ]]; then
    printf '找不到我的工作盘：%s。可用 workdisk.py init 从完整备份重新初始化。\n' "$work_disk" >&2
    exit 1
fi
tcc_disk=${LIXINOS_TCC_DISK:-${LIXINOS_DISK:-$tcc_default}}

tcc_loader=$root/data/tcc-os/loader.elf
if [[ $tcc_disk == "$work_disk" || $tcc_disk == "$root/data/shared.img" || -r ${tcc_disk%.*}.loader.elf ]]; then
    tcc_loader=${tcc_disk%.*}.loader.elf
fi
tcc_loader=${LIXINOS_TCC_LOADER:-$tcc_loader}

release_dir="$root/kernel-artifacts"
if "$source_boot"; then
    release_dir=
elif [[ -n $release ]]; then
    if [[ -n ${LIXINOS_KERNEL:-} || -n ${LIXINOS_TCC_LOADER:-} ]]; then
        echo '不能同时指定已发布版本和自定义引导镜像。' >&2
        exit 2
    fi
    if [[ $release == latest ]]; then
        release=$(python3 "$root/scripts/kernel-release.py" latest)
        [[ -n $release ]] || { echo '没有可用的本地内核发布版本。' >&2; exit 1; }
    fi
elif [[ ! -f "$release_dir/manifest.json" && -z ${LIXINOS_KERNEL:-} &&
        -z ${LIXINOS_TCC_LOADER:-} &&
        ( -z ${LIXINOS_DISK:-} || $LIXINOS_DISK -ef $rust_default ) &&
        ( -z ${LIXINOS_TCC_DISK:-} || $LIXINOS_TCC_DISK -ef $tcc_default ) ]]; then
    release=$(python3 "$root/scripts/kernel-release.py" latest)
fi

rust_label="Rust 原版内核"
tcc_label="TCC 自举内核（磁盘引导）"
if [[ -n $release ]]; then
    rust_label="Rust · $release"
    tcc_label="TCC · $release"
fi

cd "$root/Kernel"
tcc_ready=false
if [[ -s $tcc_disk && -r $tcc_disk && ( -n $release || ( -s $tcc_loader && -r $tcc_loader ) ) ]]; then
    tcc_ready=true
fi

read_preferences() {
    local values
    values=$(python3 ../scripts/disk_image.py --boot-preferences "$1") || return
    read -r preferred_mode preferred_zone preferred_kernel preferred_port preferred_model <<< "$values"
    preferences_disk=$1
}
preferences_disk=
menu_kernel=rust
# Custom images and noninteractive runs with an explicit disk are already configured.
if [[ -z $choice && -z ${LIXINOS_KERNEL:-} ]] && [[ -t 0 || -z ${LIXINOS_DISK:-} ]]; then
    if "$tcc_ready" && [[ ${LIXINOS_DISK:-$rust_default} -ef $tcc_disk ]]; then
        read_preferences "$tcc_disk"
        menu_kernel=$preferred_kernel
    fi
    case $mode in
        --window|--cocoa|--split|--terminal)
            alternate=
            if [[ $rust_default == "$root/data/shared.img" && $rust_default == "$tcc_default" && -z ${LIXINOS_DISK:-} &&
                  -z ${LIXINOS_TCC_DISK:-} && -z ${LIXINOS_TCC_LOADER:-} ]]; then
                alternate=$(python3 ../scripts/disk_image.py --local-alternative "$rust_default")
            fi
            if [[ -n $alternate ]]; then
                # A source-mode TCC loader belongs to its disk. Published TCC
                # kernels boot the selected data disk directly.
                if [[ -n $release ]]; then
                    tcc_ready=true
                else
                    tcc_loader=${alternate%.*}.loader.elf
                    tcc_ready=false
                    if [[ -s $tcc_loader && -r $tcc_loader ]]; then tcc_ready=true; fi
                fi
            fi
            disk_label=
            if [[ ${LIXINOS_DISK:-$rust_default} == "$work_disk" && $tcc_disk == "$work_disk" ]]; then disk_label=daily; fi
            choice=$(osascript "$root/scripts/choose-kernel.applescript" "$tcc_ready" "$rust_label" "$tcc_label" "${alternate##*/}" "$disk_label" "$menu_kernel") || {
                echo '无法打开内核选择窗口。可使用 --kernel rust 或 --kernel tcc 启动。' >&2
                exit 1
            }
            [[ $choice != cancel ]] || exit 0
            if [[ -n $alternate ]]; then
                export LIXINOS_DISK="$alternate"
                tcc_disk=$alternate
            fi
            ;;
        --serial|--ssh)
            if [[ -t 0 ]]; then
                menu_default=1
                if [[ $menu_kernel == tcc ]]; then menu_default=2; fi
                printf '%s\n' '选择 LixinOS 启动内核' "  1  $rust_label" >&2
                if "$tcc_ready"; then
                    echo "  2  $tcc_label" >&2
                else
                    echo '  2  TCC 自举内核（未就绪，见 Kernel/native/README.md）' >&2
                fi
                while true; do
                    printf '输入编号并回车，或按 q 回车退出 [%s] > ' "$menu_default" >&2
                    IFS= read -r answer || exit 0
                    case $answer in
                        '') choice=$menu_kernel; break ;;
                        1) choice=rust; break ;;
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
        if [[ -n $release ]]; then
            boot_files=$(python3 ../scripts/kernel-release.py boot-files "$release" tcc)
            tcc_loader="$boot_files/tcc-kernel.elf"
        fi
        export LIXINOS_KERNEL="$tcc_loader" LIXINOS_DISK="$tcc_disk"
        ;;
    '') ;; # Use the existing image override or the default Rust kernel.
    *) echo '无效的内核选择。' >&2; exit 1 ;;
esac

disk=${LIXINOS_DISK:-$rust_default}
# Only the selected disk supplies preferences. Explicit launcher options win.
if [[ $preferences_disk != "$disk" ]]; then read_preferences "$disk"; fi
if [[ -z $explicit_mode ]]; then mode=--$preferred_mode; fi
timezone=${LIXINOS_TIMEZONE:-$preferred_zone}
if [[ $mode == --ssh ]]; then export LIXINOS_SSH_PORT=${LIXINOS_SSH_PORT:-$preferred_port}; fi
if [[ $preferred_model != auto ]]; then export DEEPSEEK_MODEL=${DEEPSEEK_MODEL:-$preferred_model}; fi

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
        display=(-display none -chardev stdio,id=console,signal=off -serial chardev:console)
        profile=window
        features=(--features window-console)
        ;;
    --serial|--ssh)
        display=(-display none -chardev stdio,id=console,signal=off -serial chardev:console)
        profile=release
        features=()
        if [[ $mode == --ssh ]]; then features=(--features ssh-mux); fi
        ;;
esac

kernel_image=${LIXINOS_KERNEL:-target/aarch64-unknown-none-softfloat/$profile/lixin-kernel}
cpython_package=target/user-build/cpython-port/cpython.pkg
use_release=false
if [[ -z ${LIXINOS_KERNEL:-} ]]; then
    variant=$profile
    if [[ $mode == --ssh ]]; then variant=ssh; fi
    if [[ -n $release ]]; then
        release_dir=$(python3 ../scripts/kernel-release.py boot-files "$release" "$variant")
        use_release=true
    elif [[ -n $release_dir && -f "$release_dir/manifest.json" ]]; then
        use_release=true
    fi
    if "$use_release"; then
        kernel_image="$release_dir/rust-$variant.elf"
        cpython_package="$release_dir/cpython.pkg"
    fi
fi
if [[ -n ${LIXINOS_KERNEL:-} ]] || "$use_release"; then
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
printf 'LixinOS 启动盘：%s\n' "$disk" >&2
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
        if ! LC_ALL=C grep -aq LIXBLD01 "$kernel_image"; then
            echo "This TCC image needs a loader with build-volume support." >&2
            exit 1
        fi
        ram=24G
    else
        ram=12G
    fi
fi
clock_offset=$(TZ="$timezone" date +%z)
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
    -netdev user,id=outbound,net=10.0.2.0/24,host=10.0.2.2,guestfwd=tcp:10.0.2.100:22-cmd:../scripts/gpu20-tunnel.sh,guestfwd=tcp:10.0.2.101:8080-cmd:../scripts/map-tile.py \
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
