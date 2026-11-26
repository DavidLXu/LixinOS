# Fetch a pinned source archive. A failed transfer leaves the existing cache intact.
fetch_archive() (
    set -eu
    local path=$1 digest=$2 url=$3 temp
    verify() { printf '%s  %s\n' "$digest" "$1" | shasum -a 256 -c - >/dev/null; }
    if [[ -f "$path" ]] && verify "$path"; then return; fi
    temp=$(mktemp "$path.tmp.XXXXXX")
    trap 'rm -f "$temp"' EXIT
    curl -fLsS --retry 3 "$url" -o "$temp"
    verify "$temp"
    mv "$temp" "$path"
)
