#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ( $# -eq 2 && $2 != --configure-only ) ]]; then
    echo "Usage: $0 CHROMIUM_150.0.7871.100_SRC [--configure-only]" >&2
    exit 2
fi
profile=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$1" && pwd)
args=$(cat "$profile/args.gn")
if [[ -n ${CHROMIUM_TOOLCHAIN_ARGS:-} ]]; then
    args+=$'\n'"$(cat "$CHROMIUM_TOOLCHAIN_ARGS")"
fi
cd "$src"
version=$(awk -F= '/^(MAJOR|MINOR|BUILD|PATCH)=/ {v=v sep $2; sep="."} END {print v}' chrome/VERSION)
if [[ $version != 150.0.7871.100 ]]; then
    echo "Expected Chromium 150.0.7871.100, found $version" >&2
    exit 1
fi
changes=("$profile"/{content-shell,software-window,sysroot-packages,crash-handler,field-trials,views-delegate,test-services,local-devtools,optional-services,initial-connection}.patch)
# Later patches can touch the same lines as earlier ones. Reconstruct the
# series on a private copy before updating any files in the source tree.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
awk '/^\+\+\+ b\// {print substr($2, 3)}' "${changes[@]}" | sort -u > "$scratch/files"
while IFS= read -r path; do
    mkdir -p "$scratch/source/$(dirname "$path")"
    cp -p "$path" "$scratch/source/$path"
done < "$scratch/files"
for ((i=${#changes[@]}-1; i>=0; i--)); do
    if patch -d "$scratch/source" --dry-run -R -f -p1 < "${changes[i]}" >/dev/null 2>&1; then
        patch -d "$scratch/source" -R -f -p1 < "${changes[i]}" >/dev/null
    fi
done
for change in "${changes[@]}"; do
    patch -d "$scratch/source" -f -p1 < "$change" >/dev/null
done
while IFS= read -r path; do
    if ! cmp -s "$scratch/source/$path" "$path"; then
        cp "$scratch/source/$path" "$path"
    fi
done < "$scratch/files"
target=//content/shell:content_shell
out=out/LixinOS
scope=(--root-target="$target" --root-pattern="$target")
"${GN:-gn}" gen "$out" --args="$args" "${scope[@]}" --fail-on-unused-args
deps="$out/content_shell.deps.txt"
"${GN:-gn}" desc "$out" "$target" deps --all "${scope[@]}" > "$deps"
for excluded in //chrome:chrome //pdf:pdf //printing:printing \
    //components/crash/core/app:chrome_crashpad_handler //extensions/browser:browser //content/test:test_support //content/test:setup_field_trials \
    //services/network:test_support //net:test_support //third_party/webrtc/modules/portal:portal \
    //content/shell:content_browsertests_mojom //content/shell:rust_test_service_ffi \
    //content/shell:shell_controller_mojom //content/test:content_test_mojo_bindings //services/test/echo:lib \
    //ui/views:test_support //ui/wm:test_support //ui/aura:test_support //cc:test_support \
    //components/permissions:permissions //services/on_device_model:on_device_model_service \
    //third_party/dawn/src/dawn/native:native //media/gpu/vaapi:vaapi //ui/ozone/platform/x11:x11 //content/web_test:web_test_browser \
    //content/web_test:web_test_renderer //base/test:test_trace_processor \
    //tools/v8_context_snapshot:generate_v8_context_snapshot \
    //third_party/angle:libEGL //third_party/angle:libGLESv2 //third_party/vulkan-loader/src:libvulkan \
    //content/browser/devtools:devtools_resources //third_party/test_fonts:test_fonts \
    //third_party/swiftshader/src/Vulkan:swiftshader_libvulkan; do
    if grep -Fxq "$excluded" "$deps"; then
        echo "Unexpected dependency of $target: $excluded" >&2
        exit 1
    fi
done
if [[ $# -eq 2 ]]; then
    exit 0
fi
"${AUTONINJA:-autoninja}" -C "$out" content_shell
wc -c "$out/content_shell" "$out/content_shell.pak"
