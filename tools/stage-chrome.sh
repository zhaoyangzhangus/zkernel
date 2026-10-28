#!/usr/bin/env bash
# 将 Debian 构建的 Chromium 目标暂存到 System ESP 映像。
#
# Chromium 目标链接到 System 动态加载器/libc，但数据文件仍相对于可执行文件目录解析。
# 浏览器程序、启动器和相对路径资源统一放入自己的应用目录。
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"

chromium_out="${CHROMIUM_OUT:-}"
build_dir="${BUILD:-$project_root/build}"
cjk_font="${OS_CJK_FONT:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}"
cjk_font_license="${OS_CJK_FONT_LICENSE:-/usr/share/doc/fonts-noto-cjk/copyright}"
dry_run=0
llvm_strip="${LLVM_STRIP:-}"

usage() {
    cat <<'EOF'
用法：./tools/stage-chrome.sh [选项]

选项：
  --chromium-out PATH  Debian Chromium 输出目录
  --build PATH         项目构建目录（默认 build）
  --dry-run            只检查输入和输出，不复制文件
  -h, --help           显示帮助
EOF
}

while (($# > 0)); do
    case "$1" in
        --chromium-out)
            (($# >= 2)) || { echo "--chromium-out 需要参数" >&2; exit 2; }
            chromium_out="$2"
            shift 2
            ;;
        --build)
            (($# >= 2)) || { echo "--build 需要参数" >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "未知参数：$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$build_dir" != /* ]]; then
    build_dir="$project_root/$build_dir"
fi
if [[ -z "$chromium_out" ]]; then
    echo "请设置 CHROMIUM_OUT 或使用 --chromium-out 指定 Chromium 输出目录" >&2
    exit 2
fi
if [[ "$chromium_out" != /* ]]; then
    chromium_out="$project_root/$chromium_out"
fi
[[ -d "$chromium_out" ]] || {
    echo "找不到 Chromium 输出目录：$chromium_out" >&2
    exit 1
}
chromium_out="$(CDPATH= cd -- "$chromium_out" && pwd)"
chromium_root="$(CDPATH= cd -- "$chromium_out/../.." && pwd)"
if [[ -z "$llvm_strip" ]]; then
    llvm_strip="$chromium_root/third_party/llvm-build/Release+Asserts/bin/llvm-strip"
fi

chrome_source="$chromium_out/chrome"
launcher_source="$build_dir/elf/chrome-start.elf"
esp_dir="$build_dir/esp"
stage_dir="$esp_dir/usr/apps/chrome"

[[ -f "$chrome_source" && -x "$chrome_source" ]] || {
    echo "找不到可执行的 Debian Chromium：$chrome_source" >&2
    echo "请先完成：ninja -C $chromium_out chrome" >&2
    exit 1
}
[[ -d "$esp_dir" ]] || {
    echo "找不到 ESP：$esp_dir，请先构建 esp" >&2
    exit 1
}
[[ -f "$esp_dir/lib/ld-system.so.1" ]] || {
    echo "缺少动态加载器：$esp_dir/lib/ld-system.so.1" >&2
    exit 1
}
[[ -f "$esp_dir/lib/ld-linux-system.so.1" ]] || {
    echo "缺少 Chromium Linux ABI 动态加载器：$esp_dir/lib/ld-linux-system.so.1" >&2
    echo "请先重新执行 make BUILD=... esp" >&2
    exit 1
}
[[ -f "$esp_dir/lib/libsystemc.so.1" ]] || {
    echo "缺少运行库：$esp_dir/lib/libsystemc.so.1" >&2
    exit 1
}
chrome_runtime_mode="$(python3 - "$chrome_source" "$esp_dir" "$chromium_out/args.gn" \
    "$build_dir/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu" <<'PY'
import re
import os
import subprocess
import sys
from pathlib import Path
chrome, esp = sys.argv[1], Path(sys.argv[2])
mode = 'system'
if len(sys.argv) > 3:
    settings = Path(sys.argv[3]).read_text()
    selected = re.findall(r'^\s*native_os_runtime\s*=\s*"([^"\n]*)"\s*$', settings, re.M)
    if len(selected) > 1 or (selected and selected[0] not in ('system', 'glibc')):
        raise SystemExit('Invalid or duplicate Chrome runtime selection')
    if selected:
        mode = selected[0]
inspection_env = dict(os.environ, LC_ALL='C')
try:
    headers = subprocess.check_output(['readelf', '-lW', chrome], text=True, env=inspection_env)
    dynamic = subprocess.check_output(['readelf', '-dW', chrome], text=True, env=inspection_env)
except (OSError, subprocess.CalledProcessError) as error:
    raise SystemExit('Cannot inspect Chrome runtime dependencies: ' + str(error))
interpreters = re.findall(r'Requesting program interpreter: ([^\]]+)\]', headers)
allowed = ('/lib/ld-linux-system.so.1',) if mode == 'system' else ('/usr/apps/chrome/lib/ld-linux-x86-64.so.2',)
if len(interpreters) != 1 or interpreters[0] not in allowed:
    raise SystemExit('Chrome must use the system Linux-personality interpreter')
needed = re.findall(r'\(NEEDED\).*Shared library: \[([^\]]+)\]', dynamic)
if 'libliteosc.so.1' in needed:
    raise SystemExit('Chrome uses a retired runtime dependency; rebuild with the current toolchain')
if mode == 'system':
    runtime = [name for name in needed if name == 'libsystemc.so.1']
    if not runtime:
        raise SystemExit('Chrome does not depend on the system C runtime')
    if 'libc.so.6' in needed:
        raise SystemExit('Chrome mixes system and glibc runtimes')
    paths = [interpreters[0].lstrip('/')] + ['lib/' + name for name in runtime]
else:
    if 'libc.so.6' not in needed or 'libsystemc.so.1' in needed:
        raise SystemExit('Chrome does not exclusively depend on the selected guest glibc runtime')
    paths = [interpreters[0].lstrip('/')]
    pending, inspected = list(needed), set()
    while pending:
        name = pending.pop()
        if name in inspected:
            continue
        if not name or '/' in name or name in ('.', '..'):
            raise SystemExit('Invalid Chrome runtime dependency: '+name)
        inspected.add(name)
        relative = 'usr/apps/chrome/lib/'+name
        dependency = esp/relative
        if not dependency.is_file():
            raise SystemExit('Missing actual Chrome runtime dependency: '+str(dependency))
        paths.append(relative)
        try:
            detail = subprocess.check_output(['readelf', '-dW', str(dependency)],
                                             text=True, env=inspection_env)
        except (OSError, subprocess.CalledProcessError) as error:
            raise SystemExit('Cannot inspect Chrome runtime dependencies: '+str(error))
        pending.extend(re.findall(r'\(NEEDED\).*Shared library: \[([^\]]+)\]', detail))
    if len(sys.argv) < 5:
        raise SystemExit('Missing selected guest glibc baseline')
    interpreter = esp/interpreters[0].lstrip('/')
    if not interpreter.is_file():
        raise SystemExit('Missing actual Chrome runtime dependency: '+str(interpreter))
    baseline = Path(sys.argv[4])
    for name in ('libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
                 'librt.so.1', 'libresolv.so.2', 'libutil.so.1', 'ld-linux-x86-64.so.2'):
        if name not in inspected and name != 'ld-linux-x86-64.so.2':
            continue
        expected = baseline/name
        actual = esp/'usr/apps/chrome/lib'/name
        if not expected.is_file() or not actual.is_file() or expected.read_bytes() != actual.read_bytes():
            raise SystemExit(('Chrome guest interpreter does not match its glibc baseline: ' if name == 'ld-linux-x86-64.so.2' else 'Chrome guest glibc baseline mismatch: ')+name)
    interpreter = esp/interpreters[0].lstrip('/')
    if not interpreter.is_file():
        raise SystemExit('Missing actual Chrome runtime dependency: '+str(interpreter))
    if interpreter.read_bytes() != (baseline/'ld-linux-x86-64.so.2').read_bytes():
        raise SystemExit('Chrome guest interpreter does not match its glibc baseline')
for relative in paths:
    if not (esp/relative).is_file():
        raise SystemExit('Missing actual Chrome runtime dependency: ' + str(esp/relative))
print(mode)
PY
)"
if [[ "$chrome_runtime_mode" == glibc ]]; then
    python3 "$project_root/tools/nvidia/vulkan_runtime_abi.py" \
        --runtime-dir "$esp_dir/usr/apps/chrome/lib" "$chrome_source"
fi
# 从生成此 ELF 的同一构建图中选择启动器配置。
chrome_gpu_profile="$(python3 - "$chromium_out/args.gn" "$chrome_runtime_mode" <<'PY'
import re
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
values = re.findall(r'^\s*enable_vulkan\s*=\s*([^\s#]+)\s*(?:#.*)?$', text, re.M)
if len(values) > 1 or (values and values[0] not in ('true', 'false')):
    raise SystemExit('Invalid or duplicate enable_vulkan selection')
enabled = values == ['true']
if enabled and sys.argv[2] != 'glibc':
    raise SystemExit('Chrome Vulkan requires the verified driver-compatible runtime')
print('vulkan' if enabled else 'software')
PY
)"
if [[ "$chrome_gpu_profile" == vulkan ]]; then
    # dlopen 根依赖不会出现在 Chrome 的 DT_NEEDED 中。发布启动参数前，检查 ESP 中实际部署的加载器、
    # NVIDIA ICD 和着色器编译器依赖闭包。
    python3 - "$project_root" "$esp_dir" <<'PY'
import importlib.util
import json
from pathlib import Path
import sys
root, esp = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location('chrome_vulkan_stage', root/'tools/nvidia/stage-vulkan.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
runtime = esp/'usr/apps/chrome/lib'
icd = json.loads((esp/'usr/apps/chrome/vulkan/nvidia_icd.json').read_text())
if icd.get('ICD', {}).get('library_path') != '/usr/apps/chrome/lib/libGLX_nvidia.so.0':
    raise SystemExit('Chrome Vulkan ICD does not select the deployed NVIDIA driver')
egl = json.loads((esp/'usr/apps/chrome/egl/10_nvidia.json').read_text())
if egl.get('ICD', {}).get('library_path') != '/usr/apps/chrome/lib/libEGL_nvidia.so.0':
    raise SystemExit('Chrome EGL vendor does not select the private NVIDIA driver')
catalog = {path.name: path for path in runtime.iterdir() if path.is_file()}
selected = stage.vulkan_closure(catalog, runtime)
stage.abi.check(runtime, selected.values())
print('CHROME_VULKAN_RUNTIME_PASS', len(selected))
PY
fi
[[ -f "$project_root/assets/chrome/gpu-$chrome_gpu_profile.conf" ]] || {
    echo "缺少 Chrome GPU 启动配置：$chrome_gpu_profile" >&2; exit 1
}
[[ -f "$cjk_font" && -f "$cjk_font_license" ]] || {
    echo "缺少 Noto Sans CJK 字体或许可证；请安装 fonts-noto-cjk，或设置 OS_CJK_FONT / OS_CJK_FONT_LICENSE" >&2
    exit 1
}
[[ -x "$llvm_strip" ]] || {
    echo "找不到 llvm-strip：$llvm_strip" >&2
    exit 1
}

if ((dry_run)); then
    echo "Chrome staging input: $chrome_source"
    echo "Chrome staging output: $stage_dir"
    echo "Chrome CJK font: $cjk_font"
    echo "Input check only; this does not establish that the Chromium build is complete."
    exit 0
fi

# 增量构建期间，旧的已链接二进制仍然存在。
# 不能把文件存在误认为构建完成，也不能与构建器的链接操作竞争。
command -v flock >/dev/null || { echo "缺少 flock；无法保护统一构建/打包入口" >&2; exit 1; }
command -v ninja >/dev/null || { echo "缺少 ninja；无法验证 Chromium 是否构建完成" >&2; exit 1; }
# systemd 构建任务可能在启动 shell 退出后继续运行，而 shell 可能释放 flock。
# 加载另一个 Ninja 构建图前，也要保护该活动任务。
if pgrep -u "$(id -u)" -x ninja >/dev/null; then
    echo "Ninja 构建仍在运行；拒绝打包旧 Chrome，未修改镜像" >&2
    exit 1
fi
for unit in system-chromium-build.service system-chromium-build.scope \
            liteos-chromium-build.service liteos-chromium-build.scope; do
    if systemctl --user is-active --quiet "$unit"; then
        echo "Chromium 构建单元仍在运行；未修改镜像" >&2
        exit 1
    fi
done
exec 9>"$build_dir/.chromium-build.lock"
flock -n 9 || {
    echo "Chromium 构建仍在运行；拒绝将旧可执行文件打包到镜像" >&2
    exit 1
}
build_check="$(ninja -n -C "$chromium_out" chrome)" || {
    echo "无法检查 Chromium 构建状态；未修改镜像" >&2
    exit 1
}
if [[ "${build_check##*$'\n'}" != "ninja: no work to do." ]]; then
    echo "Chromium 尚有待构建内容；请先完成统一构建，未修改镜像" >&2
    exit 1
fi
# 浏览器参数保存在原生启动器中，而不在 Chromium ELF 内。要求同一次部署中也包含当前启动器构建，
# 不要静默使用仍禁用 GPU 合成的旧启动器。
if ! make -q -C "$project_root" "BUILD=$build_dir" "$launcher_source"; then
    echo "Chrome 启动器尚未构建完成；请先构建 chrome-start.elf，未修改镜像" >&2
    exit 1
fi
[[ -f "$launcher_source" ]] || {
    echo "缺少 Chrome 启动器：$launcher_source，未修改镜像" >&2; exit 1
}

# 原生移植版的 H.264 仅支持硬件解码。修改 ESP 中任何浏览器/资源前，先构建并验证其独立 dlopen 依赖根。
chrome_hw_video=false
if python3 - "$chromium_out/args.gn" <<'PY'
from pathlib import Path
import re, sys
text = Path(sys.argv[1]).read_text()
sys.exit(0 if re.search(r'^\s*proprietary_codecs\s*=\s*true\s*$', text, re.M) else 1)
PY
then
    chrome_hw_video=true
    python3 "$project_root/tools/nvidia/build-video-decoder.py"
    python3 "$project_root/tools/nvidia/video-decoder-runtime-gate.py" \
        --runtime-dir "$esp_dir/usr/apps/chrome/lib" \
        "$build_dir/nvidia/vulkan/libos-vulkan-video.so.1"
fi

mkdir -p "$stage_dir"

# 每种目标文件系统/请求模式只探测一次。即使 chmod(0644)，FAT/NTFS 挂载掩码也可能显示为 0777；
# 不要仅因文件系统无法表示请求的 POSIX 权限，就重新复制相同的大文件。
declare -A stage_mode_cache=()
stage_file_mode=""
stage_effective_mode() {
    local directory="$1" requested="$2" device key probe observed
    device="$(stat -c '%d' -- "$directory")" || return 1
    key="$device:$requested"
    if [[ -n "${stage_mode_cache[$key]+present}" ]]; then
        stage_file_mode="${stage_mode_cache[$key]}"
        return 0
    fi
    probe="$(mktemp "$directory/.chrome-mode.XXXXXX")" || return 1
    if ! chmod "$requested" -- "$probe"; then
        rm -f -- "$probe"
        return 1
    fi
    if ! observed="$(stat -c '%a' -- "$probe")"; then
        rm -f -- "$probe"
        return 1
    fi
    rm -f -- "$probe" || return 1
    stage_mode_cache[$key]="$observed"
    stage_file_mode="$observed"
}

install_changed() {
    local source="$1"
    local destination="$2"
    local mode="$3"
    local temporary
    if [[ -f "$destination" ]] && cmp -s -- "$source" "$destination"; then
        stage_effective_mode "${destination%/*}" "$mode" || return 1
        if [[ "$(stat -c '%a' -- "$destination")" == "$stage_file_mode" ]]; then
            return 0
        fi
    fi
    temporary="$(mktemp "${destination}.stage.XXXXXX")" || return 1
    if ! install -m "$mode" -- "$source" "$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi
    if ! mv -f -- "$temporary" "$destination"; then
        rm -f -- "$temporary"
        return 1
    fi
}

install_executable() {
    install_changed "$1" "$2" 0755
}

install_data() {
    local source="$1"
    local destination="$2"
    install_changed "$source" "$destination" 0644
}

mkdir -p "$esp_dir/EFI/OS/fonts"
# Fail closed while replacing a payload, including a Vulkan-to-software
# 切换。暂存失败时不能让旧 GPU 配置仍附着在新替换的二进制上；最终配置会在下方提交。
install_data "$project_root/assets/chrome/gpu-staging.conf" "$stage_dir/gpu-mode"
install_executable "$launcher_source" "$stage_dir/chrome-start"
if $chrome_hw_video; then
    mkdir -p "$esp_dir/usr/apps/chrome/lib"
    install_data "$build_dir/nvidia/vulkan/libos-vulkan-video.so.1" \
                 "$esp_dir/usr/apps/chrome/lib/libos-vulkan-video.so.1"
fi
install_data "$cjk_font" "$esp_dir/EFI/OS/fonts/NotoSansCJK-Regular.ttc"
install_data "$cjk_font_license" "$esp_dir/EFI/OS/fonts/NotoSansCJK-LICENSE"

# 调试版 Chromium 二进制包含数 GB 的 DWARF 和静态符号数据。启动卷为 FAT32，实际单文件上限为 4 GiB，
# 且 QEMU/UEFI 路径刻意保持精简。完整调试二进制保留在 chromium_out 供 GDB 使用，ESP 映像中仅放运行时副本。
# --strip-unneeded 会保留运行时加载器所需动态符号，并移除仅供调试/静态链接的符号。
python3 "$script_dir/chromium/stage-binary.py" \
    --source "$chrome_source" --target "$stage_dir/chrome" --strip "$llvm_strip"

# 首次单进程启动时 chrome_sandbox 为可选项，但目标生成该文件后就应暂存，避免之后多进程启动还需手动修改映像。
# 来宾不使用 setuid 沙箱位；其策略由目标进程 ABI 执行。
if [[ -f "$chromium_out/chrome_sandbox" ]]; then
    install_executable "$chromium_out/chrome_sandbox" "$stage_dir/chrome_sandbox"
fi

# 常规 Linux 发行版会将 Chromium 浏览器数据文件放在可执行文件旁。使用 nullglob，使缺少可选文件类型时不报错。
shopt -s nullglob
for pattern in '*.pak' '*.dat' '*.bin'; do
    for source in "$chromium_out"/$pattern; do
        [[ -f "$source" ]] || continue
        install_data "$source" "$stage_dir/$(basename -- "$source")"
    done
done

# 合并资源目录时不重写未变负载，也不删除目标目录独有文件。仅在 cp 成功后发布每个变更项。
merge_resources() (
    local source="$1" destination="$2" entry target temporary
    shopt -s dotglob nullglob
    mkdir -p -- "$destination"
    for entry in "$source"/*; do
        target="$destination/${entry##*/}"
        if [[ -d "$entry" && ! -L "$entry" ]]; then
            merge_resources "$entry" "$target"
            continue
        fi
        if [[ -L "$entry" && -L "$target" ]] &&
           [[ "$(readlink -- "$entry")" == "$(readlink -- "$target")" ]]; then
            continue
        fi
        if [[ -f "$entry" && ! -L "$entry" && -f "$target" && ! -L "$target" ]] &&
           cmp -s -- "$entry" "$target"; then
            stage_effective_mode "$destination" "$(stat -c '%a' -- "$entry")"
            if [[ "$(stat -c '%a' -- "$target")" == "$stage_file_mode" ]]; then
                continue
            fi
        fi
        temporary="$(mktemp -d "$destination/.chrome-resource.XXXXXX")"
        trap 'rm -f -- "$temporary/payload"; rmdir -- "$temporary"' EXIT
        cp -a -- "$entry" "$temporary/payload"
        mv -fT -- "$temporary/payload" "$target"
        rmdir -- "$temporary"
        trap - EXIT
    done
)

for directory in locales resources MEIPreload; do
    source="$chromium_out/$directory"
    [[ -d "$source" ]] || continue
    merge_resources "$source" "$stage_dir/$directory"
done
shopt -u nullglob

# 最后发布：浏览器/资源暂存中断时，不能让新 GPU 后端配置配合旧可执行文件启用。
# install_data 会以原子方式替换文件。
install_data "$project_root/assets/chrome/gpu-$chrome_gpu_profile.conf" \
             "$stage_dir/gpu-mode"

manifest="$build_dir/chrome-staging.manifest"
temporary_manifest="$(mktemp "$build_dir/.chrome-manifest.XXXXXX")"
trap 'rm -f -- "$temporary_manifest"' EXIT
{
    echo "chromium_out=$chromium_out"
    echo "stage_dir=$stage_dir"
    echo "chrome_runtime_mode=$chrome_runtime_mode"
    echo "chrome_gpu_profile=$chrome_gpu_profile"
    echo "chrome_hw_video=$chrome_hw_video"
    if $chrome_hw_video; then
        sha256sum "$esp_dir/usr/apps/chrome/lib/libos-vulkan-video.so.1"
    fi
    sha256sum "$stage_dir/chrome-start"
    sha256sum "$stage_dir/gpu-mode"
    sha256sum "$esp_dir/EFI/OS/fonts/NotoSansCJK-Regular.ttc" \
              "$esp_dir/EFI/OS/fonts/NotoSansCJK-LICENSE"
    find "$stage_dir" -type f -printf '%P\n' | sort | while IFS= read -r relative; do
        sha256sum "$stage_dir/$relative"
    done
} > "$temporary_manifest"
if ! cmp -s -- "$temporary_manifest" "$manifest"; then
    mv -f -- "$temporary_manifest" "$manifest"
fi

echo "Chrome staged at $stage_dir"
echo "Manifest written to $manifest"
if [[ ! -f "$stage_dir/chrome_sandbox" ]]; then
    echo "注意：未发现 chrome_sandbox，当前只能按单进程/无 setuid 沙箱路径验证" >&2
fi
