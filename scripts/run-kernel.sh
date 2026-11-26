#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--kernel rust|tcc] [--release latest|TAG | --source] [--ram SIZE] [--window | --split | --cocoa | --terminal | --serial | --ssh]\n' "$0"
    printf '  --ram SIZE   Guest memory for this boot, e.g. 4G, 5G or 4096M.\n'
}

mode=
choice=
release=
source_boot=false
requested_ram=
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
        --ram)
            if [[ $# -lt 2 || -z ${2:-} || -n $requested_ram ]]; then
                usage >&2
                exit 2
            fi
            requested_ram=$2
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
auto_release=false
if "$source_boot"; then
    release_dir=
elif [[ -n $release ]]; then
    if [[ -n ${LIXINOS_KERNEL:-} || -n ${LIXINOS_TCC_LOADER:-} ]]; then
        echo '不能同时指定已发布版本和自定义引导镜像。' >&2
        exit 2
    fi
elif [[ ! -f "$release_dir/manifest.json" && -z ${LIXINOS_KERNEL:-} &&
        -z ${LIXINOS_TCC_LOADER:-} &&
        ( -z ${LIXINOS_DISK:-} || $LIXINOS_DISK -ef $rust_default ) &&
        ( -z ${LIXINOS_TCC_DISK:-} || $LIXINOS_TCC_DISK -ef $tcc_default ) ]]; then
    auto_release=true
fi

cd "$root/Kernel"
tcc_ready=false
if [[ -s $tcc_disk && -r $tcc_disk && ( -n $release || "$auto_release" == true || ( -s $tcc_loader && -r $tcc_loader ) ) ]]; then
    tcc_ready=true
fi

preferences_disk=
menu_kernel=rust
explicit_release=$release
# Only run the early helper when the interactive menu needs tag labels or saved preferences.
if [[ -z $choice && -z ${LIXINOS_KERNEL:-} ]] && [[ -t 0 || -z ${LIXINOS_DISK:-} ]]; then
    early_args=(--phase early)
    if [[ -n $release ]]; then early_args+=(--release "$release"); fi
    if "$auto_release"; then early_args+=(--auto-release); fi
    if "$tcc_ready" && [[ ${LIXINOS_DISK:-$rust_default} -ef $tcc_disk ]]; then
        early_args+=(--read-prefs-disk "$tcc_disk")
    fi
    prep_out=$(python3 "$root/scripts/boot_prep.py" "${early_args[@]}") || exit $?
    eval "$prep_out"
    if [[ $explicit_release == latest && -z $release ]]; then
        echo '没有可用的本地内核发布版本。' >&2; exit 1
    fi
    auto_release=false
    [[ -z ${preferred_kernel:-} ]] || menu_kernel=$preferred_kernel
    tcc_ready=false
    if [[ -s $tcc_disk && -r $tcc_disk && ( -n $release || ( -s $tcc_loader && -r $tcc_loader ) ) ]]; then
        tcc_ready=true
    fi
    rust_label="Rust 原版内核"
    tcc_label="TCC 自举内核（磁盘引导）"
    if [[ -n $release ]]; then
        rust_label="Rust · $release"
        tcc_label="TCC · $release"
    fi
    case $mode in
        --window|--cocoa|--split|--terminal)
            alternate=
            if [[ $rust_default == "$root/data/shared.img" && $rust_default == "$tcc_default" && -z ${LIXINOS_DISK:-} &&
                  -z ${LIXINOS_TCC_DISK:-} && -z ${LIXINOS_TCC_LOADER:-} ]]; then
                alternate=$(python3 ../scripts/disk_image.py --local-alternative "$rust_default")
            fi
            if [[ -n $alternate ]]; then
                if [[ -n $release ]]; then
                    tcc_ready=true
                else
                    tcc_loader=${alternate%.*}.loader.elf
                    tcc_ready=false
                    if [[ -s $tcc_loader && -r $tcc_loader ]]; then tcc_ready=true; fi
                fi
            fi
            picker_args=(--phase picker --disk="${alternate:-${LIXINOS_DISK:-$rust_default}}" --ram="$requested_ram")
            if "$tcc_ready"; then
                picker_args+=(--picker-tcc-disk="${alternate:-$tcc_disk}" --release="$release" --custom-kernel="$tcc_loader")
            fi
            profiles=$(python3 "$root/scripts/boot_prep.py" "${picker_args[@]}") || exit $?
            # NUL framing preserves spaces, quotes and newlines in image paths.
            fields=()
            while IFS= read -r -d '' field; do fields+=("$field"); done < <(
                swift "$root/scripts/choose-kernel.swift" "$rust_label" "$tcc_label" "$profiles" \
                    "$menu_kernel" "$work_disk" "$release" "$requested_ram" "${alternate##*/}" "$root/scripts/boot_prep.py"
            )
            [[ ${fields[0]:-} != cancel ]] || exit 0
            if [[ ${#fields[@]} != 4 ]]; then
                echo '无法打开启动窗口。可使用 --kernel rust 或 --kernel tcc 启动。' >&2
                exit 1
            fi
            choice=${fields[0]} requested_ram=${fields[1]}
            export LIXINOS_DISK="${fields[2]}"
            if [[ $choice == tcc ]]; then tcc_disk=$LIXINOS_DISK; tcc_loader=${fields[3]}; fi
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
        if [[ ! ( -s $tcc_disk && -r $tcc_disk && ( -n $release || "$auto_release" == true || ( -s $tcc_loader && -r $tcc_loader ) ) ) ]]; then
            echo '缺少 TCC 引导程序或系统磁盘，请按 Kernel/native/README.md 制盘。' >&2
            exit 1
        fi
        export LIXINOS_KERNEL="$tcc_loader" LIXINOS_DISK="$tcc_disk"
        ;;
    '') ;; # Use the existing image override or the default Rust kernel.
    *) echo '无效的内核选择。' >&2; exit 1 ;;
esac

disk=${LIXINOS_DISK:-$rust_default}
prep_args=(
    --phase=launch
    --ram="$requested_ram"
    --disk="$disk"
    --release="$release"
    --choice="$choice"
    --explicit-mode="$explicit_mode"
    --mode="$mode"
    --gpu="${LIXINOS_GPU:-auto}"
    --qemu="${LIXINOS_QEMU:-}"
    --custom-kernel="${LIXINOS_KERNEL:-}"
    --release-dir="$release_dir"
    --timezone="${LIXINOS_TIMEZONE:-}"
)
if "$auto_release"; then prep_args+=(--auto-release); fi
if "$source_boot"; then prep_args+=(--source-boot); fi
prep_out=$(python3 "$root/scripts/boot_prep.py" "${prep_args[@]}") || exit $?
eval "$prep_out"
if [[ -n $custom_kernel ]]; then export LIXINOS_KERNEL="$custom_kernel"; fi
if [[ $mode == --ssh ]]; then export LIXINOS_SSH_PORT=${LIXINOS_SSH_PORT:-$preferred_port}; fi
if [[ $preferred_model != auto ]]; then export DEEPSEEK_MODEL=${DEEPSEEK_MODEL:-$preferred_model}; fi

machine=virt
case $mode in
    --window|--cocoa|--split)
        cocoa=cocoa,zoom-to-fit=off
        case $gpu_mode in
            ramfb) gpu_device=ramfb ;;
            virtio) gpu_device=virtio-gpu-device,bus=virtio-mmio-bus.7 ;;
            virgl)
                gpu_device=virtio-gpu-gl-device,bus=virtio-mmio-bus.7
                cocoa+=,gl=es
                ;;
            *) echo "LIXINOS_GPU must be auto, ramfb, virtio or virgl" >&2; exit 2 ;;
        esac
        display=(-name LixinOS -display "$cocoa" -serial null
                 -device "$gpu_device" -device virtio-keyboard-device,bus=virtio-mmio-bus.1
                 -device virtio-tablet-device,bus=virtio-mmio-bus.3)
        if [[ ${LIXINOS_AUDIO:-coreaudio} != off ]]; then
            display+=(-audiodev "${LIXINOS_AUDIO:-coreaudio},id=playback"
                      -device virtio-sound-device,audiodev=playback,streams=1,bus=virtio-mmio-bus.8)
        fi
        if [[ $mode == --split ]]; then
            machine=virt,gic-version=3
        fi
        features=(--features "$profile")
        ;;
    --terminal)
        display=(-display none -chardev stdio,id=console,signal=off -serial chardev:console)
        features=(--features window-console)
        ;;
    --serial|--ssh)
        display=(-display none -chardev stdio,id=console,signal=off -serial chardev:console)
        features=()
        if [[ $mode == --ssh ]]; then features=(--features ssh-mux); fi
        ;;
esac

if [[ -z ${LIXINOS_KERNEL:-} ]] && ! "$use_release"; then
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
if ! "$disk_prepared"; then
    python3 ../scripts/disk_image.py "$disk"
fi
[[ -n $qemu ]] || { echo "找不到 qemu-system-aarch64。" >&2; exit 1; }
launcher=("$qemu")
if [[ $mode == --terminal ]]; then
    launcher=(python3 ../scripts/open-terminal.py "${launcher[@]}")
fi
need_clipboard=false
if [[ "$profile" == retina || "$profile" == split ]]; then
    need_clipboard=true
fi
outbound="user,id=outbound,net=10.0.2.0/24,host=10.0.2.2,guestfwd=tcp:10.0.2.100:22-cmd:../scripts/gpu20-tunnel.sh,guestfwd=tcp:10.0.2.101:8080-cmd:../scripts/map-tile.py"
if "$need_clipboard" && [[ ${LIXINOS_MAC_CLIPBOARD:-1} == 1 ]]; then
    outbound+=,guestfwd=tcp:10.0.2.104:8767-cmd:../scripts/mac-text-clipboard.py
fi
codex_bridge=${LIXINOS_CODEX_DEEPSEEK:-auto}
if [[ $codex_bridge == 1 || ( $codex_bridge == auto && $disk -ef "$work_disk" ) ]]; then
    outbound+=,guestfwd=tcp:10.0.2.102:8765-cmd:../scripts/codex-deepseek-bridge.py
fi
codex_shell=${LIXINOS_CODEX_SHELL:-auto}
if [[ $codex_shell == 1 || ( $codex_shell == auto && $disk -ef "$work_disk" ) ]]; then
    export LIXINOS_SHELL_RELEASE=${release:-latest}
    export LIXINOS_SHELL_FLAVOR=${choice:-rust}
    outbound+=,guestfwd=tcp:10.0.2.103:8766-cmd:../scripts/codex-shell-worker.py
fi
vcpus=${LIXINOS_CPUS:-2}
case $vcpus in 1|2) ;; *) echo 'LIXINOS_CPUS must be 1 or 2' >&2; exit 2 ;; esac
vm_args=(
    -machine "$machine" -accel hvf -cpu host -smp "$vcpus" -m "$ram" \
    "${display[@]}" -monitor none \
    -global virtio-mmio.force-legacy=false \
    -fw_cfg "name=opt/lixinos/clock-offset,string=$clock_offset" \
    -drive "if=none,id=fs,format=raw,cache=writethrough,file=${disk//,/,,}" \
    -device virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0 \
    -object rng-random,id=entropy,filename=/dev/urandom \
    -device virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5 \
    -netdev "$outbound" \
    -device virtio-net-device,netdev=outbound,bus=virtio-mmio-bus.4 \
    -kernel "$kernel_image"
)
if [[ ${LIXINOS_SWAP:-on} != off ]]; then
    paging_template=$(python3 "$root/scripts/swap_image.py")
    vm_args+=(-drive "if=none,id=paging,format=raw,snapshot=on,file=${paging_template//,/,,}"
             -device virtio-blk-device,drive=paging,bus=virtio-mmio-bus.9)
fi
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
    elif "$need_clipboard"; then
        exec python3 ../scripts/agent-bridge.py --clipboard "${launcher[@]}" "${vm_args[@]}"
    fi
    launcher=(python3 ../scripts/agent-bridge.py "${launcher[@]}")
elif "$need_clipboard"; then
    launcher=(python3 ../scripts/clipboard-bridge.py "${launcher[@]}")
fi
if [[ $mode == --ssh ]]; then
    exec python3 ../scripts/start-ssh.py "${launcher[@]}" "${vm_args[@]}"
fi
exec "${launcher[@]}" "${vm_args[@]}"
