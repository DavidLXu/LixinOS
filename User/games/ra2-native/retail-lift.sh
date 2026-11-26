#!/usr/bin/env bash
# Generate the selected retail game's translated C using the pinned upstream tools.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
recomp="$port/../../../Kernel/target/user-build/ra2-recomp"
dir=${1:?Pass the directory holding the original Steam or EA game}
if [[ ! -f "$recomp/upstream/run_lift.py" || ! -f "$recomp/pcrecomp/tools/lift/lift32.py" ]]; then
    RA2_DIR="$dir" exec "$port/build.sh" ra2-recomp
fi
recomp=$(cd "$recomp" && pwd)
target=${RA2_TARGET:-game}
case $target in
game) gen=gen_game; work="$recomp/upstream/work/game";;
gamemd) gen=gen; work="$recomp/upstream/work";;
*) echo "Use RA2_TARGET=game or gamemd" >&2; exit 2;;
esac
exe=$(python3 - "$dir" "$target" <<'PY'
from pathlib import Path
import sys
name=sys.argv[2]+'.exe'
for p in Path(sys.argv[1]).iterdir():
    if p.is_file() and p.name.lower()==name:
        print(p.resolve())
        break
else:
    sys.exit('Missing '+name)
PY
)
export PCRECOMP="$recomp/pcrecomp"
source "$port/recomp/python-env.sh"
cd "$recomp/upstream"
# The game's patches rely on positions in the exact executable supported upstream.
"$lift_python" - "$exe" "$target" <<'PY'
import sys, run_lift
if run_lift.exe_stamp(sys.argv[1]) != run_lift.targets()[sys.argv[2]]['stamp']:
    sys.exit('Unsupported retail executable. Use the Steam/EA release specified upstream.')
PY
mkdir -p "$work" src/recomp
T="$PCRECOMP/tools"
"$lift_python" "$T/pe/pe_analyze.py" "$exe" --json "$work/pe_analysis.json" >/dev/null
"$lift_python" "$T/cpp/rtti.py" "$exe" -o "$work/rtti.json" --seeds "$work/rtti_seeds.json"
"$lift_python" "$T/disasm/disasm32.py" "$exe" -o "$work/functions.json" --seed-functions "$work/rtti_seeds.json"
out=$(mktemp -d "$recomp/lift-$target.XXXXXXXX")
"$lift_python" run_lift.py --all --target "$target" --exe "$exe" --catalog "$work/functions.json" --out "$out"
test -s "$out/recomp_dispatch.c"
RA2_LIFT_DIR="$out" RA2_TARGET="$target" bash "$port/build.sh" ra2-recomp
receipt=$(mktemp "$recomp/current-$target.XXXXXXXX")
printf '%s\n' "$out" > "$receipt"
mv -f "$receipt" "$recomp/current-$target"
