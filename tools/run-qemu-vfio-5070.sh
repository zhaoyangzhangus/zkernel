#!/usr/bin/env bash
# 将隔离的 RTX 5070 IOMMU 组绑定到 vfio-pci，然后运行 run-qemu.sh。
# 默认路径会在退出时恢复宿主驱动。
set -u -o pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
build_dir="${BUILD:-build}"
gpu_bdf="${OS_GPU_PCI:-}"
audio_bdf="${OS_GPU_AUDIO_PCI:-}"
seconds=10
gdb_port=1234
debug=0
no_build=0
headless=1
keep_open=0
fullscreen=0
check_only=0
no_rebind=0
allow_boot_vga=0
with_stdvga=0
with_ramfb=0
with_1080p=0
root_run=0
modprobe_cmd="${MODPROBE:-}"

usage() {
    cat <<'EOF'
用法：sudo tools/run-qemu-vfio-5070.sh [选项]

环境变量：BUILD、OS_GPU_PCI、OS_GPU_AUDIO_PCI；命令行选项优先。

  --check                 只做预检，不加载模块、不解绑设备
  --gpu-pci BDF           GPU BDF（默认自动寻找 10de:2f04）
  --gpu-audio-pci BDF     音频 BDF（默认按 GPU 推导）
  --build DIR             构建目录（默认 build）
  --no-build              使用已有 build/esp
  --headless              无图形输出（默认）
  --window                保留 QEMU 图形窗口
  --with-stdvga            保留 QEMU Standard VGA，仅用于 guest GOP 诊断
  --with-1080p             使用 1920x1080 QEMU Standard VGA framebuffer
  --with-ramfb             使用可见的 EFI/QEMU RAM framebuffer，不占 PCI 资源
  --seconds N             普通运行模式持续秒数（默认 10）
  --keep-open             持续运行直到 Ctrl-C
  --fullscreen            QEMU GTK 全屏显示并按窗口缩放
  --debug                 开启 GDB stub
  --gdb-port N            GDB 端口（默认 1234）
  --no-rebind             退出后不恢复原驱动（诊断用）
  --allow-boot-vga        允许摘除 boot_vga 主卡（可能使主机黑屏）
  -h, --help              显示帮助
EOF
}

while (($# > 0)); do
    case "$1" in
        --check) check_only=1; shift ;;
        --gpu-pci)
            (($# >= 2)) || { echo "--gpu-pci 需要 BDF" >&2; exit 2; }
            gpu_bdf="$2"; shift 2 ;;
        --gpu-audio-pci)
            (($# >= 2)) || { echo "--gpu-audio-pci 需要 BDF" >&2; exit 2; }
            audio_bdf="$2"; shift 2 ;;
        --build)
            (($# >= 2)) || { echo "--build 需要目录" >&2; exit 2; }
            build_dir="$2"; shift 2 ;;
        --no-build) no_build=1; shift ;;
        --headless) headless=1; shift ;;
        --window) headless=0; shift ;;
        --with-stdvga) with_stdvga=1; shift ;;
        --with-1080p) with_1080p=1; with_stdvga=1; with_ramfb=0; shift ;;
        --with-ramfb) with_ramfb=1; shift ;;
        --seconds)
            (($# >= 2)) || { echo "--seconds 需要参数" >&2; exit 2; }
            seconds="$2"; shift 2 ;;
        --keep-open) keep_open=1; shift ;;
        --fullscreen) fullscreen=1; shift ;;
        --debug) debug=1; shift ;;
        --gdb-port)
            (($# >= 2)) || { echo "--gdb-port 需要参数" >&2; exit 2; }
            gdb_port="$2"; shift 2 ;;
        --no-rebind) no_rebind=1; shift ;;
        --allow-boot-vga) allow_boot_vga=1; shift ;;
        --root-run) root_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$build_dir" in
    ''|/*|.|..|../*|*/../*|*/..) 
        echo "--build 必须是项目内相对目录：$build_dir" >&2
        exit 2
        ;;
esac
if [[ ! "$seconds" =~ ^[1-9][0-9]*$ || ! "$gdb_port" =~ ^[1-9][0-9]*$ ||
      "$gdb_port" -gt 65535 ]]; then
    echo "seconds/gdb-port 参数无效" >&2
    exit 2
fi

canonical_bdf() {
    local value="$1"
    value="${value#0000:}"
    [[ "$value" =~ ^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]] || return 1
    printf '0000:%s\n' "$value"
}

command -v lspci >/dev/null 2>&1 || {
    echo "缺少 lspci，请安装 pciutils" >&2
    exit 1
}
if [[ -z "$gpu_bdf" ]]; then
    gpu_bdf="$(lspci -Dnn 2>/dev/null | awk '/VGA compatible controller.*\[10de:2f04\]/ {print $1; exit}')"
fi
if [[ -z "$audio_bdf" && -n "$gpu_bdf" ]]; then
    gpu_domain="${gpu_bdf%%:*}"
    gpu_bus_slot="${gpu_bdf#*:}"
    audio_bdf="${gpu_domain}:${gpu_bus_slot%.*}.1"
fi
gpu_bdf="$(canonical_bdf "$gpu_bdf" 2>/dev/null || true)"
audio_bdf="$(canonical_bdf "$audio_bdf" 2>/dev/null || true)"
if [[ -z "$gpu_bdf" || -z "$audio_bdf" ]]; then
    echo "找不到 RTX 5070 GPU/HDA BDF；请设置 OS_GPU_PCI/OS_GPU_AUDIO_PCI" >&2
    exit 1
fi

gpu_sysfs="/sys/bus/pci/devices/$gpu_bdf"
audio_sysfs="/sys/bus/pci/devices/$audio_bdf"
if [[ ! -d "$gpu_sysfs" || ! -d "$audio_sysfs" ]]; then
    echo "GPU sysfs 设备不存在：$gpu_bdf / $audio_bdf" >&2
    exit 1
fi
gpu_id="$(cat "$gpu_sysfs/vendor" 2>/dev/null || true):$(cat "$gpu_sysfs/device" 2>/dev/null || true)"
audio_id="$(cat "$audio_sysfs/vendor" 2>/dev/null || true):$(cat "$audio_sysfs/device" 2>/dev/null || true)"
[[ "$gpu_id" == '0x10de:0x2f04' ]] || { echo "$gpu_bdf 不是 RTX 5070 ($gpu_id)" >&2; exit 1; }
[[ "$audio_id" == '0x10de:0x2f80' ]] || { echo "$audio_bdf 不是 RTX 5070 HDA ($audio_id)" >&2; exit 1; }

group_link="$(readlink -f "$gpu_sysfs/iommu_group" 2>/dev/null || true)"
if [[ -z "$group_link" || ! -d "$group_link/devices" ]]; then
    echo "GPU 没有 IOMMU group；请在固件/内核启用 AMD IOMMU" >&2
    exit 1
fi
group_id="$(basename "$group_link")"
group_devices=()
for device_path in "$group_link"/devices/*; do
    [[ -e "$device_path" ]] || continue
    group_devices+=("$(basename "$device_path")")
done
if ((${#group_devices[@]} != 2)) ||
   ! printf '%s\n' "${group_devices[@]}" | grep -Fxq "$gpu_bdf" ||
   ! printf '%s\n' "${group_devices[@]}" | grep -Fxq "$audio_bdf"; then
    echo "IOMMU group $group_id 不是只包含 GPU/HDA：${group_devices[*]}" >&2
    exit 1
fi
reserved_regions_file="$group_link/reserved_regions"
if [[ -r "$reserved_regions_file" ]] && grep -qE '[[:space:]]direct$' "$reserved_regions_file"; then
    echo "警告：IOMMU group $group_id 含固件声明的 direct/1:1 保留区，VFIO container 可能因 EINVAL 被内核拒绝" >&2
    echo "请在失败后执行：sudo dmesg -T | grep -Ei 'vfio|iommu|amd.?vi|ivmd|unity|1:1|reserved|01:00'" >&2
fi
if ((check_only)); then
    BUILD="$build_dir" "$root_dir/tools/audit-environment.sh" \
        --build "$build_dir" --gpu-pci "$gpu_bdf" --gpu-audio-pci "$audio_bdf"
    exit $?
fi

if [[ "$(cat "$gpu_sysfs/boot_vga" 2>/dev/null || echo 0)" == 1 &&
      "$allow_boot_vga" == 0 ]]; then
    echo "拒绝摘除 boot_vga=1 主卡；请先让 AMD iGPU 成为主显示，或加 --allow-boot-vga" >&2
    exit 1
fi

if ((EUID != 0)); then
    if ((no_build == 0)); then
        echo "先以当前用户构建系统镜像，再请求 root 负责 VFIO/QEMU" >&2
        make -C "$root_dir" -f GNUmakefile esp BUILD="$build_dir" DEBUG=2 \
            OS_NVIDIA=1 OS_INTERACTIVE_DESKTOP=1 OS_DEBUG_SERIAL=0 -j4
        build_status=$?
        ((build_status == 0)) || exit "$build_status"
    fi
    command -v sudo >/dev/null 2>&1 || {
        echo "缺少 sudo；请以 root 重新运行，或安装 sudo" >&2
        exit 1
    }
    root_arguments=(--root-run --no-build --gpu-pci "$gpu_bdf"
                    --gpu-audio-pci "$audio_bdf" --build "$build_dir"
                    --seconds "$seconds" --gdb-port "$gdb_port")
    ((debug)) && root_arguments+=(--debug)
    ((headless)) && root_arguments+=(--headless) || root_arguments+=(--window)
    ((with_stdvga)) && root_arguments+=(--with-stdvga)
    ((with_1080p)) && root_arguments+=(--with-1080p)
    ((with_ramfb)) && root_arguments+=(--with-ramfb)
    ((keep_open)) && root_arguments+=(--keep-open)
    ((fullscreen)) && root_arguments+=(--fullscreen)
    ((no_rebind)) && root_arguments+=(--no-rebind)
    ((allow_boot_vga)) && root_arguments+=(--allow-boot-vga)
    exec sudo -- "$0" "${root_arguments[@]}"
fi

if [[ -z "$modprobe_cmd" ]]; then
    for candidate in /usr/sbin/modprobe /sbin/modprobe \
        /usr/bin/modprobe /bin/modprobe; do
        if [[ -x "$candidate" ]]; then
            modprobe_cmd="$candidate"
            break
        fi
    done
fi
[[ -n "$modprobe_cmd" && -x "$modprobe_cmd" ]] || {
    echo "缺少 modprobe，请安装 kmod，或设置 MODPROBE=/path/to/modprobe" >&2
    exit 1
}
"$modprobe_cmd" vfio >/dev/null 2>&1 || true
"$modprobe_cmd" vfio_iommu_type1 >/dev/null 2>&1 || {
    echo "无法加载 vfio_iommu_type1" >&2
    exit 1
}
"$modprobe_cmd" vfio-pci >/dev/null 2>&1 || {
    echo "无法加载 vfio-pci" >&2
    exit 1
}
vfio_driver="/sys/bus/pci/drivers/vfio-pci"
[[ -d "$vfio_driver" ]] || { echo "vfio-pci 未出现在 sysfs" >&2; exit 1; }

declare -A original_drivers=()
for device_bdf in "$gpu_bdf" "$audio_bdf"; do
    device_sysfs="/sys/bus/pci/devices/$device_bdf"
    driver_link=""
    if [[ -L "$device_sysfs/driver" ]]; then
        driver_link="$(readlink -f "$device_sysfs/driver" 2>/dev/null || true)"
    fi
    original_drivers["$device_bdf"]=""
    [[ -z "$driver_link" ]] || original_drivers["$device_bdf"]="$(basename "$driver_link")"
done

cleanup_done=0
restore_devices() {
    ((cleanup_done)) && return 0
    cleanup_done=1
    if ((no_rebind)); then
        echo "保留 VFIO 绑定（--no-rebind）" >&2
        return 0
    fi
    for device_bdf in "$gpu_bdf" "$audio_bdf"; do
        device_sysfs="/sys/bus/pci/devices/$device_bdf"
        current_link=""
        if [[ -L "$device_sysfs/driver" ]]; then
            current_link="$(readlink -f "$device_sysfs/driver" 2>/dev/null || true)"
        fi
        [[ "$current_link" != "$vfio_driver" ]] || printf '%s' "$device_bdf" > "$vfio_driver/unbind" 2>/dev/null || true
        original_driver="${original_drivers[$device_bdf]}"
        if [[ -n "$original_driver" && -d "/sys/bus/pci/drivers/$original_driver" ]]; then
            "$modprobe_cmd" "$original_driver" >/dev/null 2>&1 || true
            printf '%s' "$original_driver" > "$device_sysfs/driver_override" 2>/dev/null || true
            printf '%s' "$device_bdf" > "/sys/bus/pci/drivers/$original_driver/bind" 2>/dev/null || true
        fi
        printf '\n' > "$device_sysfs/driver_override" 2>/dev/null || true
    done
}
trap restore_devices EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for device_bdf in "$gpu_bdf" "$audio_bdf"; do
    device_sysfs="/sys/bus/pci/devices/$device_bdf"
    printf 'vfio-pci' > "$device_sysfs/driver_override" || exit 1
done
for device_bdf in "$gpu_bdf" "$audio_bdf"; do
    device_sysfs="/sys/bus/pci/devices/$device_bdf"
    driver_link=""
    if [[ -L "$device_sysfs/driver" ]]; then
        driver_link="$(readlink -f "$device_sysfs/driver" 2>/dev/null || true)"
    fi
    [[ -z "$driver_link" ]] || printf '%s' "$device_bdf" > "$driver_link/unbind" || exit 1
done
for device_bdf in "$gpu_bdf" "$audio_bdf"; do
    printf '%s' "$device_bdf" > "$vfio_driver/bind" || exit 1
done
[[ -e "/dev/vfio/$group_id" ]] || { echo "/dev/vfio/$group_id 未出现" >&2; exit 1; }

qemu_arguments=(--no-build)
if ((with_ramfb)); then
    qemu_arguments+=(--no-stdvga --ramfb)
elif ((with_1080p)); then
    qemu_arguments+=(--vga-1080p)
elif ((with_stdvga == 0)); then
    qemu_arguments+=(--no-stdvga --vfio-primary "$gpu_bdf")
fi
qemu_arguments+=(--vfio-pci "$gpu_bdf" --vfio-pci "$audio_bdf")
((headless)) && qemu_arguments+=(--headless)
((debug)) && qemu_arguments+=(--debug --gdb-port "$gdb_port")
((keep_open)) && qemu_arguments+=(--keep-open) || qemu_arguments+=(--seconds "$seconds")
((fullscreen)) && qemu_arguments+=(--fullscreen)

echo "QEMU VFIO passthrough: GPU=$gpu_bdf AUDIO=$audio_bdf GROUP=$group_id" >&2
ulimit -l unlimited || { echo "无法提高 VFIO 内存锁定额度" >&2; exit 1; }
BUILD="$build_dir" "$root_dir/run-qemu.sh" "${qemu_arguments[@]}"
qemu_status=$?
guest_failure=""
if [[ -r "$build_dir/qemu-serial.log" ]]; then
    guest_failure="$(grep -E '^OS_(STAGE_FAIL|FRAMEBUFFER_MAP_FAIL)([[:space:]]|$)' \
        "$build_dir/qemu-serial.log" | tail -1 || true)"
fi
if [[ -n "$guest_failure" ]]; then
    echo "VFIO 设备已直通，但 guest 报告失败：$guest_failure" >&2
    ((qemu_status == 0)) && qemu_status=1
elif ((qemu_status != 0)); then
    echo "VFIO 启动失败；若错误是 Failed to set group container: Invalid argument，优先检查上面的 direct/1:1 保留区和内核日志。" >&2
fi
exit "$qemu_status"
