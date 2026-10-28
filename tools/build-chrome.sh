#!/usr/bin/env bash
# One Chromium output directory and one bounded builder; never stage a live VM.
set -euo pipefail

project_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
chromium_src="${CHROMIUM_SRC:-}"
[[ -n "$chromium_src" ]] || {
    echo "请设置 CHROMIUM_SRC 指向实际 Chromium 源码目录；不会猜测旧路径" >&2
    exit 2
}
chromium_src="$(CDPATH= cd -- "$chromium_src" && pwd)"
chromium_out="$chromium_src/out/liteos-debian"
build_log="${OS_CHROME_BUILD_LOG:-$project_root/build/chrome-unified-build.log}"
check_only=false
plan_only=false
case "${1:-}" in
    '') ;;
    --check) check_only=true ;;
    --plan) plan_only=true ;;
    *) echo "用法：$0 [--check|--plan]；CHROMIUM_SRC 可指定 Chromium 源码目录" >&2; exit 2 ;;
esac
(( $# <= 1 )) || { echo "参数过多" >&2; exit 2; }

for tool in python3 ninja sccache systemd-run systemctl flock grep; do
    command -v "$tool" >/dev/null || { echo "缺少工具：$tool" >&2; exit 1; }
done
[[ -x "$chromium_src/buildtools/linux64/gn" ]] || {
    echo "缺少 Chromium GN：$chromium_src/buildtools/linux64/gn" >&2; exit 1;
}
python3 - "$chromium_out/args.gn" "$project_root" <<'PY'
import re
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
expected = {'native_os_root': sys.argv[2], 'native_os_build_dir': sys.argv[2] + '/build'}
for key, value in expected.items():
    match = re.search(r'^\s*' + key + r'\s*=\s*"([^"\n]*)"\s*$', text, re.M)
    if match is None or match.group(1) != value:
        raise SystemExit('args.gn 的 ' + key + ' 不指向当前项目；拒绝构建其他副本')
if not re.search(r'^\s*use_sysroot\s*=\s*true\s*$', text, re.M):
    raise SystemExit('必须启用固定 sysroot；拒绝使用本机开发头文件')
if not re.search(r'^\s*sysroot\s*=\s*"//build/linux/debian_bullseye_amd64-sysroot"\s*$', text, re.M):
    raise SystemExit('sysroot 必须指向统一的固定 Debian SDK')
for option in ('use_qt5', 'use_qt6', 'rtc_use_pipewire'):
    if not re.search(r'^\s*' + option + r'\s*=\s*false\s*$', text, re.M):
        raise SystemExit('nativeguest 必须显式关闭宿主桌面依赖：' + option)
sdk = Path(sys.argv[1]).parents[2] / 'build/linux/debian_bullseye_amd64-sysroot'
if not (sdk / 'usr/include/linux/limits.h').is_file():
    raise SystemExit('固定 SDK 不完整；不会退回本机头文件')
PY
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH
printf 'Chromium source: %s\nChromium output: %s\nSystem build: %s/build\n' \
    "$chromium_src" "$chromium_out" "$project_root"
echo 'GN threads=1; Ninja jobs=12; CPUQuota=1200%; MemoryHigh=22G; MemoryMax=24G; swap=24G; cache=30G'
$check_only && exit 0
max_build_steps="${OS_CHROME_MAX_BUILD_STEPS:-500}"
if $plan_only; then
    exec python3 "$project_root/tools/chrome-build-plan.py" \
        --out "$chromium_out" --max-steps "$max_build_steps"
fi

mkdir -p "$project_root/build"
[[ -d "$(dirname -- "$build_log")" ]] || {
    echo "构建日志目录不存在：$build_log" >&2; exit 1
}
touch -- "$build_log"
exec 9>"$project_root/build/.chromium-build.lock"
flock -n 9 || { echo "已有统一构建入口运行；不会启动第二个构建" >&2; exit 1; }
# Also protect a legacy/manual builder which does not hold our lock.
if pgrep -u "$(id -u)" -x ninja >/dev/null; then
    echo "已有 Ninja 构建运行；保留它，不启动第二个构建" >&2; exit 1
fi
for unit in system-chromium-build.scope system-chromium-build.service \
            liteos-chromium-build.scope liteos-chromium-build.service; do
    if systemctl --user is-active --quiet "$unit"; then
        echo "资源单元仍运行：$unit；拒绝重叠构建或绕过资源上限" >&2; exit 1
    fi
done
# A cache daemon started elsewhere would run its compiler children outside
# this service's budget. Do not stop another job's daemon automatically.
if pgrep -u "$(id -u)" -x sccache >/dev/null; then
    echo "已有 sccache 进程；待其所属任务结束后再运行此入口" >&2; exit 1
fi

cd "$chromium_src"
# Unlike a transient scope, this service removes its idle cache daemon after
# the main builder exits, so a later invocation can reuse the same unit name.
# Compilation and generated files remain incremental in out/liteos-debian.
# Ninja tracks GN inputs itself. Bootstrap missing/unsafe graphs only; running
# GN unconditionally defeats its up-to-date check and wastes startup memory.
systemd-run --user --wait --pipe --collect --unit=system-chromium-build \
    --working-directory="$chromium_src" \
    -p Nice=10 -p MemoryHigh=22G -p MemoryMax=24G -p MemorySwapMax=24G -p CPUQuota=1200% \
    -p KillMode=control-group \
    env SCCACHE_CACHE_SIZE=30G OS_ROOT="$project_root" \
    OS_CHROME_MAX_BUILD_STEPS="$max_build_steps" /bin/bash -c \
    'set -euo pipefail
graph=out/liteos-debian/build.ninja
if [[ ! -f "$graph" ]] || ! { grep -m1 -A3 "^rule gn$" "$graph" | grep -qE "^  command = .*gn .*--threads=1([[:space:]]|$).*--regeneration gen \\.$"; }; then
    ./buildtools/linux64/gn gen out/liteos-debian --threads=1
fi
python3 "$OS_ROOT/tools/chrome-build-plan.py" --out out/liteos-debian \
    --max-steps "$OS_CHROME_MAX_BUILD_STEPS"
exec ninja -C out/liteos-debian -j12 chrome' \
    >>"$build_log" 2>&1
