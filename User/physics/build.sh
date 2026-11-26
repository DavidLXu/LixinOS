#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cache="$root/Kernel/target/user-build"
musl="$cache/cpython-port"
bash "$root/User/mujoco/build.sh"
mj="$cache/mujoco-port"
gnome="$cache/gnome-port"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
work="$cache/physics-build"
out="$cache/physics-package/bin"
mkdir -p "$work" "$out" "$cache/physics-package/lib/physics"
(cd "$root/User/mujoco" && xxd -i triple.mjb) | sed 's/^unsigned/static const unsigned/' > "$work/triple-model.h"
mesa="$gnome/sysroot"
[[ -f "$mesa/lib/libEGL.a" ]] || bash "$root/User/gnome/build-mesa.sh"
export PKG_CONFIG_PATH=""
export PKG_CONFIG_LIBDIR="$mesa/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig"
read -r -a flags <<< "$(pkg-config --static --cflags --libs glesv2)"
"$gnome/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -I"$root/User/tcc" -I"$mj/mujoco-4.0.1/include" -I"$work" \
    "$root/User/physics/app.c" "$root/User/gui/han-font.c" -Wl,--start-group "$mj/libmujoco-engine.a" "${flags[@]}" -lm -Wl,--end-group -o "$work/physics"
python3 - "$root" "$work" <<'PY_PACK'
from pathlib import Path
import runpy, sys, tempfile, shutil
root, work = map(Path, sys.argv[1:])
pkg = work.parent / "physics-package"
with tempfile.TemporaryDirectory(dir=work) as tmp:
    stage = Path(tmp)
    runpy.run_path(str(root / "User/gcc/package.py"))["tool"](
        stage, "/bin/physics.win3", work / "physics", library="/lib/physics",
        launcher_flag="-DLX_DBUS", build_dir=work, deflate=True)
    images = pkg / "lib/physics/images"
    shutil.copytree(stage / "lib/physics/images", images, dirs_exist_ok=True)
    shutil.copy2(stage / "bin/physics.win3", pkg / "bin/physics.win3")
    for path in images.iterdir():
        if not (stage / "lib/physics/images" / path.name).exists():
            path.unlink()
PY_PACK
install -m 644 "$mj/mujoco-4.0.1/LICENSE" "$cache/physics-package/lib/physics/LICENSE-MuJoCo"
install -m 644 "$cache/mujoco-package/lib/mujoco/LICENSE-libccd" "$cache/physics-package/lib/physics/"
install -m 644 "$gnome/mesa-24.3.4/licenses/MIT" "$cache/physics-package/lib/physics/LICENSE-Mesa-MIT"
