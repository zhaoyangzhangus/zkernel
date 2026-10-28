#!/usr/bin/env bash
set -euo pipefail

qemu_launcher="${1:-}"
if [[ -z "$qemu_launcher" || ! -f "$qemu_launcher" ]]; then
    echo "usage: $0 /path/to/run-qemu.sh" >&2
    exit 2
fi

# 这是对启动器策略的静态检查，不代表来宾网络已连通。
# 保留下面的诊断 NAT 检查，但绝不允许默认使用 NAT。
if ! grep -qxF 'emulated_net=0' "$qemu_launcher" ||
   ! grep -qF 'physical_nic="${QEMU_PHYSICAL_NIC:-0000:07:00.0}"' "$qemu_launcher" ||
   ! grep -qF 'vfio_devices+=("$physical_nic")' "$qemu_launcher" ||
   ! grep -qE 'qemu_args\+=\([[:space:]]*-nic[[:space:]]+none[[:space:]]*\)' "$qemu_launcher"; then
    echo "QEMU must default to physical NIC VFIO and suppress the implicit emulated NIC" >&2
    exit 1
fi

net_guard_line="$(grep -n -m1 'if \[\[ -n "\$net_dump" \]\]' "$qemu_launcher" | cut -d: -f1 || true)"
netdev_line="$(grep -n -F -m1 ' -netdev "user,id=vm-net,host=10.0.2.2"' "$qemu_launcher" | cut -d: -f1 || true)"
nic_line="$(grep -n -m1 ' -device "e1000e,netdev=vm-net' "$qemu_launcher" | cut -d: -f1 || true)"

if [[ -z "$net_guard_line" || -z "$netdev_line" || -z "$nic_line" ||
      "$netdev_line" -ge "$net_guard_line" || "$nic_line" -ge "$net_guard_line" ]]; then
    echo "Explicit emulated diagnostics must retain e1000e and the host-loopback alias; --net-dump may only add a filter" >&2
    exit 1
fi
if grep -q 'guestfwd=.*cmd:socat' "$qemu_launcher"; then
    echo "Emulated diagnostics must use the host alias, not the subprocess relay" >&2
    exit 1
fi

if grep -q ' -net none' "$qemu_launcher"; then
    echo "QEMU must select physical or emulated networking explicitly" >&2
    exit 1
fi

echo "QEMU launcher policy: physical NIC VFIO by default; e1000e/SLIRP only for explicit diagnostics (static check)"
