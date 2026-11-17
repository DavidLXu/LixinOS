#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--window | --split | --cocoa | --terminal | --serial]\n' "$0"
}

if (( $# > 1 )); then
    usage >&2
    exit 2
fi
machine=virt
case ${1:---window} in
    --window|--cocoa|--split)
        display=(-name LixinOS -display cocoa,zoom-to-fit=off -serial null
                 -device ramfb -device virtio-keyboard-device,bus=virtio-mmio-bus.1)
        profile=retina
        if [[ ${1:-} == --split ]]; then
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
    --serial)
        display=(-display none -serial stdio)
        profile=release
        features=()
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

cd "$(dirname "$0")/../Kernel"

if ! rustup_bin=$(command -v rustup); then
    rustup_bin="${CARGO_HOME:-$HOME/.cargo}/bin/rustup"
fi
if [[ ! -x "$rustup_bin" ]]; then
    printf '%s\n' 'Install rustup to build the LixinOS kernel. See README.md for setup.' >&2
    exit 1
fi

# Bash 3.2 with nounset requires this form for an empty array.
"$rustup_bin" run stable cargo build --locked --target-dir target --profile "$profile" ${features[@]+"${features[@]}"}
disk=${LIXINOS_DISK:-../data/lixinos.img}
if [[ ! -e "$disk" ]]; then
    mkdir -p "$(dirname "$disk")"
    temporary=$(mktemp "${disk}.XXXXXX")
    trap 'unlink "$temporary"' EXIT
    qemu-img create -q -f raw "$temporary" 8M
    ln "$temporary" "$disk" || [[ -f "$disk" ]]
    unlink "$temporary"
    trap - EXIT
fi
launcher=("$(command -v qemu-system-aarch64)")
if [[ ${1:---window} == --terminal ]]; then
    launcher=(python3 ../scripts/open-terminal.py "${launcher[@]}")
fi
vm_args=(
    -machine "$machine" -accel hvf -cpu host -smp 1 -m 128M \
    "${display[@]}" -monitor none \
    -global virtio-mmio.force-legacy=false \
    -drive "if=none,id=fs,format=raw,cache=writethrough,file=${disk//,/,,}" \
    -device virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0 \
    -kernel "target/aarch64-unknown-none-softfloat/$profile/lixin-kernel"
)
if [[ ${DEEPSEEK_API_KEY+x} != x ]]; then
    if DEEPSEEK_API_KEY=$(/usr/bin/security find-generic-password \
        -a "$(id -un)" -s LixinOS.DeepSeek -w 2>/dev/null); then
        export DEEPSEEK_API_KEY
    fi
fi
if [[ -n ${DEEPSEEK_API_KEY:-} ]]; then
    if [[ ${1:-} == --terminal ]]; then
        exec python3 ../scripts/agent-bridge.py --terminal "${launcher[@]}" "${vm_args[@]}"
    fi
    exec python3 ../scripts/agent-bridge.py "${launcher[@]}" "${vm_args[@]}"
fi
exec "${launcher[@]}" "${vm_args[@]}"
