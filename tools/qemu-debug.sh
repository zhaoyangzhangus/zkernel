#!/usr/bin/env bash
#
# qemu-debug.sh —— 给 VS Code 的 F5 用：启动调试版 QEMU，并等 gdbstub 真正可连接。
#
# 为什么不能简单地在后台起 QEMU 就完事：
#   QEMU 用 -S 把 CPU 停在复位向量等 GDB（这样 GDB 才有机会在固件启动前下断点），
#   但 gdbstub 的监听端口是 QEMU 初始化过程中才建立的。VS Code 的 preLaunchTask
#   会在本脚本退出后立刻启动 GDB，太早连会 "Connection refused"。
#   所以这里轮询端口，确认能连上之后才打印就绪标记并退出（脚本随后仍守着 QEMU）。
#
# VS Code 侧的配合见 .vscode/tasks.json：
#   - problemMatcher.background.endsPattern 匹配下面的 "[qemu] gdbstub listening"
#   - pattern 匹配下面 diagnose() 输出的 "[qemu] <脚本路径>: error: <原因>"。
#     之所以要带脚本路径：VS Code 规定自定义 problemMatcher 的 pattern 必须同时有
#     file 和 message 两个捕获组（否则报“问题模式无效。必须至少包含一个文件和一条消息。”，
#     对应源码里的 ProblemPatternParser.problemPattern.missingProperty）。
set -euo pipefail

PORT="${GDB_PORT:-1234}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="$ROOT/tools/$(basename "${BASH_SOURCE[0]}")"
cd "$ROOT"

# 按 VS Code 能识别的格式报告问题：出错时“问题”面板里会出现一条指向本脚本的可点击条目。
# 用法：diagnose error "原因"
diagnose() {
    echo "[qemu] ${SELF}: $1: $2" >&2
}

# 上一轮调试残留的 QEMU 会占着端口，先清掉
# （模式里的 [g] 是防 pkill 匹配到本脚本自身）
pkill -f "[g]db tcp::${PORT}" 2>/dev/null || true

make -s DEBUG=1 all

echo "[qemu] starting (gdbstub port ${PORT}, cpu paused)"
make -s DEBUG=1 GDB_PORT="${PORT}" run-gdb &
qemu_pid=$!

# 等端口就绪。CPU 被 -S 停住，所以这里等多久都不会丢启动过程。
for _ in $(seq 1 150); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
        exec 3<&- 3>&-          # 探测用的连接立刻关掉，QEMU 会继续等真正的 GDB
        echo "[qemu] gdbstub listening on 127.0.0.1:${PORT}"
        wait "${qemu_pid}"
        exit 0
    fi
    sleep 0.1
done

diagnose error "gdbstub 未能在 127.0.0.1:${PORT} 上监听（QEMU 是否已经退出？端口是否被占用？）"
kill "${qemu_pid}" 2>/dev/null || true
exit 1