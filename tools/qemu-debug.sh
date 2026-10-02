#!/usr/bin/env bash
set -euo pipefail

PORT="${GDB_PORT:-1234}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="$ROOT/tools/qemu-debug.sh"
cd "$ROOT"

pkill -f "[g]db tcp::${PORT}" 2>/dev/null || true
make -s all
if [[ ! -f "$ROOT/build/debug/kernel.elf" ]]; then
    echo "[qemu] $SELF: error: build/debug/kernel.elf is missing" >&2
    exit 1
fi

echo "[qemu] starting on :${PORT}"
make -s GDB_PORT="${PORT}" debug &
pid=$!

for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
        exec 3<&- 3>&-
        echo "[qemu] ready on :${PORT}"
        wait "$pid"
        exit $?
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done

echo "[qemu] ${SELF}: error: gdbstub did not start" >&2
kill "$pid" 2>/dev/null || true
exit 1
