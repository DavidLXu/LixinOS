#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [--window | --split | --cocoa | --terminal | --serial | --ssh]\n' "$0"
}

if (( $# > 1 )); then
    usage >&2
    exit 2
fi
machine=virt
case ${1:---window} in
    --window|--cocoa|--split)
        display=(-name LixinOS -display cocoa,zoom-to-fit=off -serial null
                 -device ramfb -device virtio-keyboard-device,bus=virtio-mmio-bus.1
                 -device virtio-tablet-device,bus=virtio-mmio-bus.3)
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
    --serial|--ssh)
        display=(-display none -serial stdio)
        profile=release
        features=()
        if [[ ${1:-} == --ssh ]]; then features=(--features ssh-mux); fi
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
python3 ../scripts/disk_image.py "$disk"
launcher=("$(command -v qemu-system-aarch64)")
if [[ ${1:---window} == --terminal ]]; then
    launcher=(python3 ../scripts/open-terminal.py "${launcher[@]}")
fi
vm_args=(
    -machine "$machine" -accel hvf -cpu host -smp 1 -m 512M \
    "${display[@]}" -monitor none \
    -global virtio-mmio.force-legacy=false \
    -drive "if=none,id=fs,format=raw,cache=writethrough,file=${disk//,/,,}" \
    -device virtio-blk-device,drive=fs,bus=virtio-mmio-bus.0 \
    -object rng-random,id=entropy,filename=/dev/urandom \
    -device virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.5 \
    -netdev user,id=outbound,net=10.0.2.0/24,host=10.0.2.2 \
    -device virtio-net-device,netdev=outbound,bus=virtio-mmio-bus.4 \
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
    launcher=(python3 ../scripts/agent-bridge.py "${launcher[@]}")
fi
if [[ ${1:-} == --ssh ]]; then
    exec python3 ../scripts/start-ssh.py "${launcher[@]}" "${vm_args[@]}"
fi
exec "${launcher[@]}" "${vm_args[@]}"
