#!/bin/sh
# QEMU starts this byte pipe only when the guest connects to its GPU20 endpoint.
set -eu
config=$(/usr/bin/ssh -G gpu20 2>/dev/null)
field() { printf '%s\n' "$config" | awk -v name="$1" '$1 == name {print $2; exit}'; }
jump=$(field proxyjump)
host=$(field hostname)
port=$(field port)
case "$jump" in ''|-*|none|*,*) echo 'gpu20 requires one configured ProxyJump' >&2; exit 1;; esac
exec /usr/bin/ssh -T -o BatchMode=yes -o StrictHostKeyChecking=yes \
    -o ForwardAgent=no -o ControlMaster=no -o ControlPath=none \
    -o ConnectTimeout=10 -W "$host:$port" -- "$jump"
