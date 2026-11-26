# Sourced by the build and retail lift. Install only into the local build cache.
lift_python="$recomp/lift-env/bin/python"
if [[ ! -x $lift_python ]]; then python3 -m venv "$recomp/lift-env"; fi
if ! "$lift_python" -c 'import capstone, pefile' >/dev/null 2>&1; then
    "$lift_python" -m pip install pefile==2024.8.26 capstone==5.0.6
fi
