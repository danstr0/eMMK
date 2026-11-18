#!/usr/bin/env bash
#
# tap_setup.sh — host side of the tap test rig (multiqueue / RSS tests).
#
#   tap_setup.sh [up] [owner]   create the tap (idempotent); needs root
#   tap_setup.sh down           remove it
#   tap_setup.sh status         show it (no root needed)
#
# From PowerShell, without a password prompt:
#   wsl -u root --cd <repo> -- bash tools/tap_setup.sh
#
# Creates a multi-queue tap device owned by <owner> (default: the user who
# ran sudo, else the WSL default user, uid 1000), so QEMU can open it with
# queues=N without root. The host gets 192.168.250.1/24 on it; the kernel
# uses 192.168.250.2 (make NET=tap). The device lasts until WSL restarts
# (wsl --shutdown); run this again afterwards.

set -euo pipefail

TAP=${EMMK_TAP:-emmk0}
HOST_ADDR=192.168.250.1/24

cmd=${1:-up}

if [ "$cmd" = status ]; then
    if ip link show "$TAP" >/dev/null 2>&1; then
        ip -d link show "$TAP" | head -3
        ip -br addr show "$TAP"
    else
        echo "$TAP: not present (run: wsl -u root -- bash tools/tap_setup.sh)"
        exit 1
    fi
    exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "tap_setup.sh: needs root. From PowerShell:" >&2
    echo "  wsl -u root --cd <repo> -- bash tools/tap_setup.sh" >&2
    exit 1
fi

if [ "$cmd" = down ]; then
    ip link del "$TAP" 2>/dev/null && echo "$TAP removed" || echo "$TAP: not present"
    exit 0
fi

if [ "$cmd" != up ]; then
    echo "usage: tap_setup.sh [up [owner]|down|status]" >&2
    exit 2
fi

owner=${2:-${SUDO_USER:-$(getent passwd 1000 | cut -d: -f1)}}
if [ -z "$owner" ] || ! id "$owner" >/dev/null 2>&1; then
    echo "tap_setup.sh: cannot determine the owning user; pass it: tap_setup.sh up <user>" >&2
    exit 1
fi

modprobe tun 2>/dev/null || true

if ip link show "$TAP" >/dev/null 2>&1; then
    # Recreate if it is not ours or not multi-queue
    if ! ip -d link show "$TAP" | grep -q "multi_queue" \
       || [ "$(ip -d link show "$TAP" | sed -n 's/.*user \([0-9]*\).*/\1/p')" != "$(id -u "$owner")" ]; then
        ip link del "$TAP"
    fi
fi
if ! ip link show "$TAP" >/dev/null 2>&1; then
    ip tuntap add dev "$TAP" mode tap multi_queue user "$owner"
fi

ip addr replace "$HOST_ADDR" dev "$TAP"
ip link set "$TAP" up

echo "$TAP ready: multi-queue tap owned by $owner, host address $HOST_ADDR"
echo "Kernel side (make NET=tap): 192.168.250.2. Lasts until wsl --shutdown."

