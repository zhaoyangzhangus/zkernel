#!/usr/bin/env bash
set -euo pipefail

PORT="${GDB_PORT:-1234}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="$ROOT/tools/qemu-debug.sh"
cd "$ROOT"

pid=""

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# 清理上一次异常退出残留的 QEMU。
pkill -f "[g]db tcp::${PORT}" 2>/dev/null || true

make -s all
if [[ ! -f "$ROOT/build/debug/kernel.elf" ]]; then
    echo "[qemu] $SELF: error: build/debug/kernel.elf is missing" >&2
    exit 1
fi

echo "[qemu] starting on :${PORT}"
make -s GDB_PORT="${PORT}" debug &
pid=$!

# 不主动连接 gdbstub，只观察 LISTEN，避免 readiness probe 本身占用 GDB 连接。
for _ in $(seq 1 100); do
    if ss -ltnH "sport = :${PORT}" 2>/dev/null | grep -q .; then
        echo "[qemu] ready on :${PORT}"
        break
    fi
    if ! kill -0 "${pid}" 2>/dev/null; then
        wait "${pid}"
        exit $?
    fi
    sleep 0.1
done

if ! ss -ltnH "sport = :${PORT}" 2>/dev/null | grep -q .; then
    echo "[qemu] $SELF: error: gdbstub did not start" >&2
    exit 1
fi

# preLaunchTask 到这里已被 VSCode 视为 ready。
# 等待 cppdbg 真正连入；调试结束后连接消失，此任务负责自动关闭 QEMU。
connected=0
while kill -0 "${pid}" 2>/dev/null; do
    if ss -tnH state established "sport = :${PORT}" 2>/dev/null | grep -q .; then
        connected=1
    elif [[ "${connected}" -eq 1 ]]; then
        # GDB 曾经连接过，现在已经断开：调试会话结束。
        exit 0
    fi
    sleep 0.1
done

wait "${pid}"
