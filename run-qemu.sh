#!/usr/bin/env bash
# 使用 Linux QEMU/OVMF 组件运行 System。
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

make_cmd="${MAKE:-make}"
qemu_path="${QEMU_PATH:-$(command -v qemu-system-x86_64 || true)}"
qemu_img_path="${QEMU_IMG:-$(command -v qemu-img || true)}"
firmware="${OVMF_CODE:-}"
build_dir="${BUILD:-build}"
debug=0
start_paused=0
no_build=0
reuse_image=0
prepare_only=0
headless=0
usb_nested=0
all_disks=0
stdvga=1
ramfb=0
vga_width=0
vga_height=0
vga_memory_mb=64
vfio_devices=()
usb_audio_devices=()
[[ -z "${QEMU_USB_AUDIO:-}" ]] || usb_audio_devices+=("$QEMU_USB_AUDIO")
vfio_primary=""
# 默认从可移动 FAT 卷启动。USB 根目录内核路径会先验证 /EFI/OS/init 和
# /EFI/OS/init-runtime，再将其选作根目录 /。保留显式 NVMe 开关，用于诊断和
# 对比旧路径。
usb_storage=1
chrome_fast=0
monitor_socket=""
qmp_socket=""
keep_open=0
fullscreen=0
no_kvm=0
net_dump=""
emulated_net=0
physical_nic="${QEMU_PHYSICAL_NIC:-0000:07:00.0}"
seconds=10
cpu_count=4
qemu_memory="${QEMU_MEMORY:-4096M}"
gdb_port=1234
debug_level=2
serial_trace=0
qemu_trace="${QEMU_TRACE:-}"
stream_log_dir="${QEMU_STREAM_LOG_DIR:-}"
# 即使来宾扫描输出（以及 HMP screendump）正常，QEMU 的 GTK Wayland 客户端也
# 可能仍显示过期的黑色画面。若有 X 显示器，优先使用 XWayland；需要其他路径的
# 主机可通过 QEMU_GDK_BACKEND 显式选择 wayland、x11 或其他 GTK 后端。
qemu_gdk_backend="${QEMU_GDK_BACKEND:-${GDK_BACKEND:-}}"

usage() {
    cat <<'EOF'
用法：./run-qemu.sh [选项]

选项：
  --debug             启动 GDB stub，监听 127.0.0.1:1234
  --start-paused      在首条指令前暂停，通过已有 QMP 继续，不开放 GDB 端口
  --gdb-port N        修改 GDB 端口
  --headless          不显示图形窗口
  --usb-nested        测试 xHCI -> Hub -> Hub -> Mouse
  --usb-storage       使用 xHCI USB Mass Storage U盘启动（默认）
  --chrome-fast       使用 NVMe 根盘启动 Chrome，避免 USB 随机 I/O 瓶颈
  --all-disks         在 USB 根盘之外附加一个 NVMe 数据盘
  --nvme-root         使用原来的 NVMe 根盘启动（诊断/回退）
  --no-stdvga         不添加 QEMU Standard VGA（给 VFIO GPU 直通使用）
  --vga-1080p         使用 1920x1080 QEMU Standard VGA framebuffer
  --ramfb             使用不占 PCI 资源的 EFI/QEMU RAM framebuffer
  --usb-audio VID:PID  直通指定 USB 声卡（如 0db0:cc78）
  --vfio-pci BDF      将主机 PCI 设备 BDF 直通到来宾（可重复）
  --vfio-primary BDF  将一个 VFIO GPU 作为来宾 primary VGA（启用 x-vga）
  --monitor-socket P  在 Unix socket P 开启 HMP monitor
  --qmp-socket P     在 Unix socket P 开启 QMP（虚拟设备输入/诊断）
  --keep-open         普通运行模式下持续运行，Ctrl-C 退出
  --fullscreen        将 QEMU GTK 输出切换为全屏并按窗口缩放
  --seconds N         普通运行模式最多运行 N 秒（默认 10）
  --no-build          使用现有 build/esp，不重新编译
  --reuse-image       复用现有磁盘，保留来宾数据（必须与 --no-build 一起使用）
  --prepare-only      只准备统一磁盘镜像，不启动 QEMU（可配合 --no-build）
  --debug-level N     编译 DEBUG 等级（默认 2）
  --serial-trace      保留 COM1 串口诊断输出（会明显降低启动速度）
  --cpu N             vCPU 数量（默认 4）
  --no-kvm            强制使用软件模拟
  --net-dump PATH     将 QEMU 虚拟网卡报文保存到 PATH（诊断用）
  --no-emulated-net   使用物理网卡 VFIO（默认，QEMU_PHYSICAL_NIC 指定 BDF）
  --emulated-net      显式使用 e1000e/SLIRP（仅对照诊断）
  --qemu PATH         指定 Linux QEMU 路径
  --firmware PATH     指定 OVMF_CODE 固件路径
  -h, --help          显示帮助
EOF
    echo '环境变量 QEMU_STREAM_LOG_DIR：将完整 stdout/stderr 额外写入已有目录内的独立运行目录（图形模式 stdout 包含串口；headless 串口仍为 build/qemu-serial.log）'
}

while (($# > 0)); do
    case "$1" in
        --debug) debug=1; shift ;;
        --start-paused) start_paused=1; shift ;;
        --gdb-port)
            (($# >= 2)) || { echo "--gdb-port 需要参数" >&2; exit 2; }
            gdb_port="$2"; shift 2 ;;
        --headless) headless=1; shift ;;
        # 嵌套设备测试需要 xHCI 拓扑，且直连根端口不能被存储设备占用；因此即使
        # 普通启动默认使用 USB，此测试仍沿用以 NVMe 为根设备的测试语义。
        --usb-nested) usb_nested=1; usb_storage=0; shift ;;
        --usb-storage) usb_storage=1; shift ;;
        --chrome-fast) chrome_fast=1; shift ;;
        --all-disks) all_disks=1; shift ;;
        --nvme-root) usb_storage=0; shift ;;
        --no-stdvga) stdvga=0; shift ;;
        --vga-1080p)
            stdvga=1
            ramfb=0
            vga_width=1920
            vga_height=1080
            vga_memory_mb=32
            shift
            ;;
        --ramfb) ramfb=1; stdvga=0; shift ;;
        --usb-audio)
            (($# >= 2)) || { echo "--usb-audio 需要 VID:PID" >&2; exit 2; }
            usb_audio="$2"
            usb_audio_devices+=("$usb_audio")
            [[ "$usb_audio" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{4}$ ]] || {
                echo "USB 声卡标识必须为四位十六进制 VID:PID" >&2; exit 2;
            }
            shift 2
            ;;
        --vfio-pci)
            (($# >= 2)) || { echo "--vfio-pci 需要 BDF 参数" >&2; exit 2; }
            if [[ ! "$2" =~ ^([0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
                echo "无效的 PCI BDF：$2" >&2
                exit 2
            fi
            vfio_devices+=("$2")
            shift 2
            ;;
        --vfio-primary)
            (($# >= 2)) || { echo "--vfio-primary 需要 BDF 参数" >&2; exit 2; }
            if [[ ! "$2" =~ ^([0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
                echo "无效的 primary VGA BDF：$2" >&2
                exit 2
            fi
            [[ -z "$vfio_primary" ]] || {
                echo "--vfio-primary 只能指定一个 BDF" >&2
                exit 2
            }
            vfio_primary="$2"
            shift 2
            ;;
        --monitor-socket)
            (($# >= 2)) || {
                echo "--monitor-socket 需要参数" >&2
                exit 2
            }
            monitor_socket="$2"
            shift 2
            ;;
        --qmp-socket)
            (($# >= 2)) || {
                echo "--qmp-socket 需要参数" >&2
                exit 2
            }
            qmp_socket="$2"
            shift 2
            ;;

        --keep-open) keep_open=1; shift ;;
        --fullscreen) fullscreen=1; shift ;;
        --seconds)
            (($# >= 2)) || { echo "--seconds 需要参数" >&2; exit 2; }
            seconds="$2"; shift 2 ;;
        --no-build) no_build=1; shift ;;
        --reuse-image) reuse_image=1; shift ;;
        --prepare-only) prepare_only=1; shift ;;
        --debug-level)
            (($# >= 2)) || { echo "--debug-level 需要参数" >&2; exit 2; }
            debug_level="$2"; shift 2 ;;
        --serial-trace) serial_trace=1; shift ;;
        --cpu)
            (($# >= 2)) || { echo "--cpu 需要参数" >&2; exit 2; }
            cpu_count="$2"; shift 2 ;;
        --no-kvm) no_kvm=1; shift ;;
        --no-emulated-net) emulated_net=0; shift ;;
        --emulated-net) emulated_net=1; shift ;;
        --net-dump)
            (($# >= 2)) || { echo "--net-dump 需要参数" >&2; exit 2; }
            net_dump="$2"; shift 2 ;;
        --qemu)
            (($# >= 2)) || { echo "--qemu 需要参数" >&2; exit 2; }
            qemu_path="$2"; shift 2 ;;
        --firmware)
            (($# >= 2)) || { echo "--firmware 需要参数" >&2; exit 2; }
            firmware="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数：$1" >&2; usage >&2; exit 2 ;;
    esac
done

# USB 设备直通前检查唯一性和访问权限，避免启动后才静默丢失声卡。
for usb_audio in "${usb_audio_devices[@]}"; do
    [[ "$usb_audio" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{4}$ ]] || {
        echo "USB 声卡标识必须为 VID:PID" >&2; exit 2;
    }
    usb_audio="${usb_audio,,}"
    if ((prepare_only == 0)); then
        audio_matches=()
        for audio_device in /sys/bus/usb/devices/*; do
            [[ -f "$audio_device/idVendor" && -f "$audio_device/idProduct" ]] || continue
            [[ "$(<"$audio_device/idVendor"):$(<"$audio_device/idProduct")" == "$usb_audio" ]] || continue
            audio_matches+=("$audio_device")
        done
        ((${#audio_matches[@]} == 1)) || {
            echo "USB 声卡 $usb_audio 匹配到 ${#audio_matches[@]} 个设备，需要唯一的已连接设备" >&2; exit 1;
        }
        audio_device="${audio_matches[0]}"
        printf -v audio_node '/dev/bus/usb/%03d/%03d' "$(<"$audio_device/busnum")" "$(<"$audio_device/devnum")"
        [[ -r "$audio_node" && -w "$audio_node" ]] || {
            echo "无法访问 USB 声卡 $audio_node，请配置该设备的 USB 访问权限" >&2; exit 1;
        }
    fi
done

if ((reuse_image && !no_build)); then
    echo "--reuse-image 必须与 --no-build 一起使用，避免构建后误用旧镜像" >&2
    exit 2
fi

# 选择规范的物理网络设备。此处绝不解绑宿主驱动：如果 NIC 尚未就绪，现有的
# VFIO 启动前检查必须直接失败。
if ((emulated_net == 0)); then
    if [[ ! "$physical_nic" =~ ^([0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
        echo "无效的 QEMU_PHYSICAL_NIC：$physical_nic" >&2
        exit 2
    fi
    physical_nic="${physical_nic,,}"
    [[ "$physical_nic" == ????:??:??.? ]] || physical_nic="0000:$physical_nic"
    physical_nic_present=0
    for candidate in "${vfio_devices[@]}"; do
        candidate="${candidate,,}"
        [[ "$candidate" == ????:??:??.? ]] || candidate="0000:$candidate"
        if [[ "$candidate" == "$physical_nic" ]]; then physical_nic_present=1; fi
    done
    if ((physical_nic_present == 0)); then vfio_devices+=("$physical_nic"); fi
fi

# Chrome 启动时会映射大型只读资源，并对配置文件执行大量小块 I/O。存储验证仍
# 显式默认使用 USB，但面向运行时的启动方式可选择速度更快的 NVMe 路径。
if ((chrome_fast)); then
    usb_storage=0
fi
if ((emulated_net == 0)) && [[ -n "$net_dump" ]]; then
    echo "--net-dump 仅支持模拟网卡，不能与 --no-emulated-net 同用" >&2
    exit 2
fi

# B10B-3 HMP 热插拔监视器
if [[ -n "$monitor_socket" &&
      "$monitor_socket" != /* ]]; then
    monitor_socket="$script_dir/$monitor_socket"
fi
if [[ -n "$qmp_socket" && "$qmp_socket" != /* ]]; then
    qmp_socket="$script_dir/$qmp_socket"
fi

if ((usb_nested && usb_storage)); then
    echo "--usb-nested 与 --usb-storage 请分开测试" >&2
    exit 2
fi
if ((all_disks && usb_storage == 0)); then
    echo "--all-disks 当前要求 USB 作为 UEFI 根盘" >&2
    exit 2
fi

[[ -n "$qemu_path" && -x "$qemu_path" ]] || {
    echo "找不到 Linux QEMU，请安装 qemu-system-x86 或使用 --qemu PATH" >&2
    exit 1
}
[[ -n "$qemu_img_path" && -x "$qemu_img_path" ]] || {
    echo "找不到 qemu-img，请安装 qemu-utils 或设置 QEMU_IMG" >&2
    exit 1
}

if [[ -z "$firmware" ]]; then
    for candidate in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/edk2-x86_64-code.fd; do
        if [[ -f "$candidate" ]]; then firmware="$candidate"; break; fi
    done
fi
[[ -f "$firmware" ]] || {
    echo "找不到 OVMF_CODE 固件，请安装 ovmf 或使用 --firmware PATH" >&2
    exit 1
}

mkdir -p "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd -P)"
qemu_fat_image="$build_dir/qemu-fat.img"

# 每个 BUILD 同一时间只允许一个 QEMU 启动器运行。此 shell 会在 QEMU 整个运行
# 期间持有锁，包括超时和调试包装脚本的运行时间，避免第二次调用重新构建共享
# 磁盘，或意外使用不同 inode 启动第二台虚拟机。
command -v flock >/dev/null 2>&1 || {
    echo "缺少 flock（util-linux），无法保证同一 BUILD 只运行一台 QEMU" >&2
    exit 1
}
exec {qemu_lock_fd}>"$build_dir/qemu-run.lock"
if ! flock -n "$qemu_lock_fd"; then
    echo "该 BUILD 已有 QEMU 启动器运行，拒绝并行构建/启动：$build_dir" >&2
    exit 1
fi

# 在编译或替换磁盘前，验证显式指定的证据保存位置。若可移动挂载路径不存在，
# 绝不静默创建该路径。
stream_log_run=""
if [[ -n "$stream_log_dir" ]]; then
    [[ -d "$stream_log_dir" ]] || {
        echo "完整日志目录不存在：$stream_log_dir" >&2; exit 1;
    }
    stream_log_run="$(mktemp -d "$stream_log_dir/qemu-stream.XXXXXX")" || exit 1
    echo "QEMU 完整输出镜像：$stream_log_run"
fi

# 如果当前进程无法打开或映射所请求的设备，应在构建或替换磁盘前失败。仅准备
# 镜像时既不需要 VFIO，也不需要 mlock。
if ((prepare_only == 0 && ${#vfio_devices[@]} > 0)); then
    python3 "$script_dir/tools/qemu-vfio-preflight.py" \
        --memory "$qemu_memory" "${vfio_devices[@]}"
fi

mirror_build_stdout() {
    tee "$build_dir/qemu-build.log" |
    awk 'tolower($0) ~ /(^|[^a-z])(fail|failed|failure|error|fatal|panic|abort|invalid|cannot|could not|undefined reference|make(\[[0-9]+\])?: \*\*\*)([^a-z]|$)/ {
            print
            fflush()
        }'
}

mirror_build_stderr() {
    tee "$build_dir/qemu-build-stderr.log" |
        awk 'tolower($0) ~ /(^|[^a-z])(fail|failed|failure|error|fatal|panic|abort|invalid|cannot|could not|undefined reference|make(\[[0-9]+\])?: \*\*\*)([^a-z]|$)/ {
            print
            fflush()
        }' >&2
}

if ((no_build == 0)); then
    # COM1 以 115200 波特率逐字节串行传输；每个诊断字符都会让来宾等待 UART，
    # 从而暂停运行。保持普通运行时启动速度；只有指定 --serial-trace 时才启用
    # 诊断路径。
    build_status=0
    nvidia_make_args=()
    if ((${#vfio_devices[@]} > 0)); then
        nvidia_make_args+=(OS_NVIDIA=1)
    fi
    "$make_cmd" -f GNUmakefile esp DEBUG="$debug_level" \
        OS_DEBUG_SERIAL="$serial_trace" \
        OS_AUTOSTART_CHROME="${OS_AUTOSTART_CHROME:-0}" \
        OS_INTERACTIVE_DESKTOP="${OS_INTERACTIVE_DESKTOP:-1}" \
        "${nvidia_make_args[@]}" \
        > >(mirror_build_stdout) 2> >(mirror_build_stderr) || build_status=$?
    if ((build_status != 0)); then
        echo "System 构建失败（退出码：$build_status），请查看 $build_dir/qemu-build.log" >&2
        exit "$build_status"
    fi
fi
if ((reuse_image == 0)); then
python3 "$script_dir/tools/migrate-system-layout.py" "$build_dir/esp"
python3 "$script_dir/tools/finalize-app-layout.py" "$build_dir/esp" --strict
[[ -f "$build_dir/esp/EFI/OS/kernel.elf" ]] || {
    echo "找不到 $build_dir/esp/EFI/OS/kernel.elf，请先构建镜像" >&2
    exit 1
}
python3 "$script_dir/tools/clean-runtime-aliases.py" "$build_dir/esp"

# 内核 VFS 自检会在 ESP 根目录创建临时条目。来宾删除这些条目后，QEMU 10 的
# vvfat 后端仍可能让它们留在宿主机上；下次启动时，这些条目会导致 mkdir/create
# 检查失败（或触发 vvfat 断言）。构建合成 FAT 镜像前，先隔离所有已知的自检
# 产物。移动文件既能保留证据以便恢复，也能确保启动目录内容稳定。
stale_dir="$build_dir/qemu-stale"
mkdir -p "$stale_dir"
for stale_entry in \
    "$build_dir/esp/vfs-api" \
    "$build_dir/esp/vfs-filemap-test"* \
    "$build_dir/esp/etc/vfs-filemap-test"* \
    "$build_dir/esp/EFI/OS/vfs-filemap-test"*; do
    [[ -e "$stale_entry" ]] || continue
    stale_name="$(basename -- "$stale_entry")"
    stale_target="$stale_dir/"$stale_name".$$"
    mv -- "$stale_entry" "$stale_target"
done
fi

# 不要直接从 QEMU 的 `fat:rw:<directory>` 后端启动。来宾截断文件或执行 fsync
# （例如在记事本中按 Ctrl+S）时，其 vvfat 实现可能导致整个模拟器中止。先从
# 只读目录视图生成普通 FAT 磁盘镜像，再让来宾写入该原始镜像。生成的镜像可
# 随时丢弃，且不会修改 build/esp。
qemu_secondary_image="$build_dir/qemu-nvme-data.img"

# qemu-img 的旧版 `fat:ro:` 源默认使用 FAT16，超过约 512 MiB 的 Chrome 负载会
# 被拒绝。保持小镜像路径的处理速度；大型镜像则生成 FAT32，并为 /tmp/chrome-data
# 和来宾运行时文件预留少量可写空间。
fat_mkfs=""
for candidate in "${MKFS_FAT:-}" /sbin/mkfs.fat /usr/sbin/mkfs.fat; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        fat_mkfs="$candidate"
        break
    fi
done
mformat_path=""
for candidate in "${MFORMAT:-}" /usr/bin/mformat /bin/mformat; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        mformat_path="$candidate"
        break
    fi
done
mcopy_path=""
for candidate in "${MCOPY:-}" /usr/bin/mcopy /bin/mcopy; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
        mcopy_path="$candidate"
        break
    fi
done

create_large_fat_image() {
    local source_dir="$1"
    local output_image="$2"
    local source_bytes
    local image_bytes
    local alignment=$((256 * 1024 * 1024))
    local headroom=$((512 * 1024 * 1024))

    [[ ( -n "$fat_mkfs" || -n "$mformat_path" ) && -n "$mcopy_path" ]] || {
        echo "大于 FAT16 容量的镜像需要 mkfs.fat（或 mformat）和 mcopy" >&2
        return 1
    }
    source_bytes="$(du -sb "$source_dir" | awk 'NR == 1 { print $1 }')" || return 1
    image_bytes=$((source_bytes + headroom))
    image_bytes=$(((image_bytes + alignment - 1) / alignment * alignment))
    truncate -s "$image_bytes" "$output_image" || return 1
    if [[ -n "$fat_mkfs" ]]; then
        "$fat_mkfs" -F 32 -n SYSTEM "$output_image" >/dev/null || return 1
    else
        "$mformat_path" -F -i "$output_image" -v SYSTEM :: >/dev/null || return 1
    fi
    (
        shopt -s dotglob nullglob
        local entries=("$source_dir"/*)
        ((${#entries[@]} > 0)) || exit 0
        "$mcopy_path" -s -i "$output_image" "${entries[@]}" ::
    ) || return 1
}

materialize_fat_image() {
    local output_image="$1"
    local source_bytes
    local temp_dir
    local temp_image
    source_bytes="$(du -sb "$build_dir/esp" | awk 'NR == 1 { print $1 }')"
    temp_dir="$(mktemp -d "$build_dir/.qemu-image.tmp.XXXXXX")"
    temp_image="$temp_dir/$(basename -- "$output_image")"
    if ((source_bytes > 480 * 1024 * 1024)); then
        if ! create_large_fat_image "$build_dir/esp" "$temp_image"; then
            rm -f -- "$temp_image"
            rmdir -- "$temp_dir" 2>/dev/null || true
            return 1
        fi
    elif ! "$qemu_img_path" convert -O raw "fat:ro:$build_dir/esp" \
            "$temp_image" >/dev/null 2>"$build_dir/qemu-img.stderr"; then
        rm -f -- "$temp_image"
        rmdir -- "$temp_dir" 2>/dev/null || true
        echo "无法生成 QEMU FAT 磁盘镜像，请查看 $build_dir/qemu-img.stderr" >&2
        return 1
    fi

    if ! mv -f -- "$temp_image" "$output_image"; then
        rm -f -- "$temp_image"
        rmdir -- "$temp_dir" 2>/dev/null || true
        echo "无法替换 QEMU FAT 磁盘镜像：$output_image" >&2
        return 1
    fi
    rmdir -- "$temp_dir" 2>/dev/null || true
}

if [[ -e "$qemu_fat_image" ]]; then
    command -v fuser >/dev/null 2>&1 || {
        echo "缺少 fuser（psmisc），无法确认磁盘未被运行中的 QEMU 使用" >&2
        exit 1
    }
    active_image_users="$(fuser "$qemu_fat_image" 2>/dev/null || true)"
    if [[ -n "$active_image_users" ]]; then
        echo "拒绝重建正在使用的 QEMU 磁盘：$qemu_fat_image" >&2
        fuser -v "$qemu_fat_image" 2>&1 >&2 || true
        exit 1
    fi
fi

if ((reuse_image)); then
    [[ -s "$qemu_fat_image" && -r "$qemu_fat_image" && -w "$qemu_fat_image" ]] || {
        echo "无法复用现有磁盘：$qemu_fat_image" >&2; exit 1;
    }
    if ((all_disks)); then
        [[ -s "$qemu_secondary_image" && -r "$qemu_secondary_image" && -w "$qemu_secondary_image" ]] || {
            echo "无法复用现有磁盘：$qemu_secondary_image" >&2; exit 1;
        }
        if fuser "$qemu_secondary_image" >/dev/null 2>&1; then
            echo "拒绝复用正在使用的 QEMU 磁盘：$qemu_secondary_image" >&2; exit 1;
        fi
    fi
else
    python3 "$script_dir/tools/preserve-qemu-app-data.py" "$qemu_fat_image" "$build_dir/esp"
    materialize_fat_image "$qemu_fat_image"
    if ((all_disks)); then
        materialize_fat_image "$qemu_secondary_image"
    fi
fi

if ((prepare_only)); then
    echo "QEMU 磁盘镜像已准备：$qemu_fat_image（未启动虚拟机）"
    exit 0
fi

# 拆分的 OVMF 镜像需要与之匹配的可写变量闪存。将其与规范磁盘放在一起，并且
# 重启时绝不替换固件设置。
firmware_vars_template="${OVMF_VARS_TEMPLATE:-}"
if [[ -z "$firmware_vars_template" ]]; then
    case "${firmware##*/}" in
        OVMF_CODE_4M*.fd) firmware_vars_template="${firmware%/*}/OVMF_VARS_4M.fd" ;;
        OVMF_CODE.fd) firmware_vars_template="${firmware%/*}/OVMF_VARS.fd" ;;
    esac
fi
firmware_vars_args=()
if [[ -n "$firmware_vars_template" ]]; then
    [[ -r "$firmware_vars_template" ]] || {
        echo "找不到配套 OVMF 变量模板：$firmware_vars_template" >&2; exit 1;
    }
    firmware_vars="$build_dir/OVMF_VARS.fd"
    if [[ ! -e "$firmware_vars" ]]; then
        cp -- "$firmware_vars_template" "$firmware_vars"
    fi
    [[ -w "$firmware_vars" && -s "$firmware_vars" &&
       "$(stat -c %s -- "$firmware_vars")" == "$(stat -c %s -- "$firmware_vars_template")" ]] || {
        echo "OVMF 变量存储不可写或尺寸不匹配：$firmware_vars" >&2; exit 1;
    }
    firmware_vars_args=( -drive "if=pflash,format=raw,file=$firmware_vars" )
fi

# 非交互式无头启动不能让 QEMU 串口后端依赖 stdout 的进程替换管道。该管道会在
# Popen/VS Code 任务运行器下改变 TCG 时序，并可能掩盖来宾唤醒竞争。保持实时
# 终端路径不变，同时为无头自动化提供直接写入的串口文件；下方还会将其同步到
# qemu-serial.log。
serial_device="file:/dev/stdout"
serial_to_file=0
if ((headless)) && [[ ! -t 1 ]]; then
    serial_device="file:$build_dir/qemu-serial.log"
    serial_to_file=1
fi

qemu_use_kvm=0
if ((no_kvm == 0)) && [[ -r /dev/kvm && -w /dev/kvm ]]; then
    qemu_use_kvm=1
fi
source "$script_dir/tools/qemu-cpu-model.sh"
qemu_cpu_model="$(qemu_select_cpu_model /proc/cpuinfo "$qemu_use_kvm" "$chrome_fast")"

qemu_machine=q35
if ((${#vfio_devices[@]} > 0)); then
    # 将来宾 RAM 保持在平台约 2 GiB 处的 IVMD 保留范围以下。其余 RAM 放在
    # 4 GiB 以上；DMA 隔离仍保持启用。
    qemu_machine+=",max-ram-below-4g=${QEMU_MAX_RAM_BELOW_4G:-1G}"
fi

qemu_args=(
    -machine "$qemu_machine"
    -vga none
    -m "$qemu_memory"
    # 独立运行的加密测试需要足够强的随机种子。QEMU 的 qemu64 模型默认隐藏
    # RDRAND，因此需要公开虚拟指令，但不能启用内核尚未保存状态的 AVX/XSAVE。
    # Chrome/KVM 可以使用主机支持的 SSSE3/SSE4/POPCNT，以及现有的 FXSAVE64 状态。
    -cpu "$qemu_cpu_model"
    -smp "$cpu_count"
    -drive "if=pflash,format=raw,readonly=on,file=$firmware"
    "${firmware_vars_args[@]}"
    -boot order=c
    # 交互式启动将 QEMU stdout 用作实时 COM1 数据流；无头且非 PTY 的启动使用
    # 上方选定的直接串口文件，避免轮询工具依赖进程替换管道。
    -serial "$serial_device"
    -d guest_errors
    -D "$build_dir/qemu.log"
)

if ((!keep_open)); then
    # 有时限的诊断保留停止现场；常驻桌面遵循来宾的关机和重启请求。
    qemu_args+=( -no-reboot -no-shutdown )
fi

if ((stdvga)); then
    # P14：使用固定配置的 QEMU Standard VGA。设置显示模式后，OVMF 可公开真正的
    # 1920x1080 GOP，而不只是缩放 800x600 RAMFB。
    vga_spec="VGA,vgamem_mb=${vga_memory_mb}"
    if ((vga_width > 0 && vga_height > 0)); then
        vga_spec+=",xres=${vga_width},yres=${vga_height}"
    fi
    qemu_args+=( -device "$vga_spec" )
elif ((ramfb)); then
    # RAMFB 是系统设备，不是 PCI VGA 设备。它可让 GTK 后端显示 EFI 帧缓冲区，
    # 无需占用 MMIO 空间，也不会改变 VFIO GPU 的 BAR 布局。
    qemu_args+=( -device ramfb )
fi

if ((usb_storage)); then
    # USB 模式下，特意只使用生成的镜像作为启动磁盘，让 OVMF 和 System 都通过
    # 同一个 U 盘文件系统和写入路径运行。
    qemu_args+=(
        -drive "if=none,id=vm-usb-stick,format=raw,file=$qemu_fat_image"
        -device "qemu-xhci,id=vm-xhci,msix=on,p2=8,p3=8"
    )
    usb_storage_device="usb-storage,id=vm-usb-storage,drive=vm-usb-stick,bus=vm-xhci.0,port=1"
    if ((all_disks)); then usb_storage_device+=",bootindex=1"; fi
    qemu_args+=( -device "$usb_storage_device" )
    if ((all_disks)); then
        qemu_args+=(
            -drive "if=none,id=vm-nvme-data,format=raw,file=$qemu_secondary_image"
            -device "nvme,drive=vm-nvme-data,serial=vm-data,bootindex=2"
        )
    fi
else
    qemu_args+=(
        -drive "if=none,id=vm-nvme0,format=raw,file=$qemu_fat_image"
        -device "nvme,drive=vm-nvme0,serial=vm-system,bootindex=1"
    )
fi

if [[ -n "$monitor_socket" ]]; then
    mkdir -p "$(dirname -- "$monitor_socket")"
    rm -f "$monitor_socket"

    qemu_args+=(
        -monitor
        "unix:$monitor_socket,server=on,wait=off"
    )
else
    qemu_args+=(
        -monitor
        none
    )
fi

if [[ -n "$qmp_socket" ]]; then
    mkdir -p "$(dirname -- "$qmp_socket")"
    rm -f "$qmp_socket"
    qemu_args+=( -qmp "unix:$qmp_socket,server=on,wait=off" )
fi


if ((qemu_use_kvm)); then
    qemu_args+=( -accel kvm )
else
    qemu_args+=( -accel tcg,thread=multi )
fi

if ((headless)); then
    qemu_args+=( -display none )
else
    # 指针位于 QEMU 窗口上方时捕获键盘输入。GTK 不提供仅 macOS 支持的
    # `full-grab` 选项；Linux 支持的 `grab-on-hover` 机制可避免虚拟机窗口处于
    # 活动状态时，将来宾按键发送到启动器终端。
    if ((fullscreen)); then
        qemu_args+=( -display gtk,zoom-to-fit=on,full-screen=on,grab-on-hover=on,show-cursor=off )
    else
        qemu_args+=( -display gtk,zoom-to-fit=off,grab-on-hover=on,show-cursor=off )
    fi
fi

if [[ -z "$qemu_gdk_backend" && -n "${DISPLAY:-}" && "$headless" -eq 0 ]]; then
    qemu_gdk_backend=x11
fi
qemu_launcher=()
if [[ -n "$qemu_gdk_backend" ]]; then
    qemu_launcher=(env "GDK_BACKEND=$qemu_gdk_backend")
fi

if ((usb_nested)); then
    #
    # B10B-2 嵌套 USB 集线器拓扑
    #
    # 实际连接拓扑：
    #
    #   xHCI 根端口 1
    #        |
    #        +-- 集线器 #1
    #              |
    #              +-- 端口 1：集线器 #2
    #                          |
    #                          +-- 端口 1：鼠标
    #
    # System 预期的 xHCI 路由字符串：
    #
    #   集线器 #1：0x00000
    #   集线器 #2：0x00001
    #   鼠标     ：0x00011
    #
    # 让拓扑不依赖显示后端，使自动化 HMP 测试既能在无头 QEMU 中注入输入，也
    # 能在 GTK 窗口中进行。
    qemu_args+=(
        -device "qemu-xhci,id=vm-xhci,msix=on,p2=8,p3=8"
        -device "usb-hub,id=vm-hub1,bus=vm-xhci.0,port=1"
        -device "usb-hub,id=vm-hub2,bus=vm-xhci.0,port=1.1"
        -device "usb-mouse,id=vm-nested-mouse,bus=vm-xhci.0,port=1.1.1"
    )
elif ((usb_storage == 0)); then
    qemu_args+=(
        -device "qemu-xhci,id=vm-xhci,msix=on,p2=8,p3=8"
        -device "usb-kbd,bus=vm-xhci.0"
        -device "usb-mouse,bus=vm-xhci.0"
    )
else
    # USB 启动已为存储设备创建 xHCI 控制器；所有显示模式下都将输入设备添加到
    # 同一个控制器。
    qemu_args+=(
        -device "usb-kbd,bus=vm-xhci.0"
        -device "usb-mouse,bus=vm-xhci.0"
    )
fi

if ((debug)); then qemu_args+=( -gdb "tcp::${gdb_port}" -S ); fi
if ((start_paused && !debug)); then qemu_args+=( -S ); fi

for vfio_device in "${vfio_devices[@]}"; do
    vfio_device_short="${vfio_device#0000:}"
    vfio_primary_short="${vfio_primary#0000:}"
    vfio_device_options="vfio-pci,host=$vfio_device"
    # 不使用网络启动：将物理 NIC 留给来宾驱动，不公开可选的厂商 PXE/UEFI 扩展 ROM。
    vfio_nic_short="${physical_nic#0000:}"
    if [[ "${vfio_device_short,,}" == "${vfio_nic_short,,}" ]]; then
        vfio_device_options+=",rombar=0"
    fi
    if [[ -n "$vfio_primary" && "$vfio_device_short" == "$vfio_primary_short" ]]; then
        vfio_device_options+=",x-vga=on"
    fi
    qemu_args+=( -device "$vfio_device_options" )
done

# 仅交接音频设备，保留宿主 USB 控制器上的其他外设。
((${#usb_audio_devices[@]} <= 2)) || { echo "当前最多直通两块 USB 声卡" >&2; exit 2; }
audio_port=4
for usb_audio in "${usb_audio_devices[@]}"; do
    qemu_args+=( -device "usb-host,id=host-audio-$audio_port,bus=vm-xhci.0,port=$audio_port,vendorid=0x${usb_audio%:*},productid=0x${usb_audio#*:}" )
    ((audio_port+=1))
done

if [[ -n "$qemu_trace" ]]; then
    IFS=',' read -r -a trace_events <<< "$qemu_trace"
    for trace_event in "${trace_events[@]}"; do
        [[ -n "$trace_event" ]] || continue
        qemu_args+=( -trace "enable=$trace_event" )
    done
    qemu_args+=( -trace "file=$build_dir/qemu-trace.log" )
fi
# 物理 NIC 是规范设备；模拟网卡仅用于显式开启的诊断模式。新版 QEMU 移除了
# `-net dump`，因此可选诊断路径会为这个显式命名的用户模式 NIC 添加过滤器。
if ((emulated_net)); then
qemu_args+=(
    # Chrome 通过 SLIRP 的主机别名访问 127.0.0.1:7890。普通 NAT 套接字彼此独立，
    # 每个连接各自使用一个套接字，不需要 guestfwd 子进程或可能混合并发 TLS 数据流
    # 的持久 TCP chardev。
    -netdev "user,id=vm-net,host=10.0.2.2"
    -device "e1000e,netdev=vm-net,mac=52:54:00:12:34:56"
)
else
    # 同时禁用 QEMU 隐式创建的默认 NIC。VFIO NIC 不受影响。
    qemu_args+=( -nic none )
fi
if [[ -n "$net_dump" ]]; then
    qemu_args+=(
        -object "filter-dump,id=vm-net-dump,netdev=vm-net,file=$net_dump"
    )
fi

stdout_log="$build_dir/qemu-stdout.log"
stderr_log="$build_dir/qemu-stderr.log"
serial_log="$build_dir/qemu-serial.log"
stdout_targets=("$stdout_log")
stderr_targets=("$stderr_log")
if [[ -n "$stream_log_run" ]]; then
    stdout_targets+=("$stream_log_run/stdout.log")
    stderr_targets+=("$stream_log_run/stderr.log")
fi

# 完整数据流仍写入两个日志文件；只有出现故障或异常停止信息时，才在交互式
# 终端中显示。
mirror_qemu_stdout() {
    if ((serial_to_file)); then
        tee "${stdout_targets[@]}"
    else
        tee "${stdout_targets[@]}" "$serial_log"
    fi |
    awk '/(^|[^A-Z])(FAIL|ERROR|FATAL|PANIC|FAULT|TIMEOUT|ASSERT)([^A-Z]|$)/ {
            sub(/\r$/, "", $0)
            print
            fflush()
        }'
}

mirror_qemu_stderr() {
    # QEMU 通常会在 stderr 中报告有意触发的超时终止。该信息仍保留在
    # qemu-stderr.log 中，但只有看起来像诊断信息的行会显示在交互式终端上。
    tee "${stderr_targets[@]}" |
        awk '!/terminating on signal 15 from pid/ &&
             tolower($0) ~ /error|fail|fatal|panic|abort|invalid|cannot|could not|unsupported|assert|segmentation|fault/ {
            print
            fflush()
        }' >&2
}

# 在宿主终端中，Ctrl+S 对应 VSTOP（XOFF）。如果 QEMU GTK 窗口尚未获得焦点，
# 该字符会被终端接收，而不会发给模拟的 USB 键盘。对此启动器禁用软件流控，并在
# 所有退出路径上恢复原有终端模式。此设置仅适用于交互式图形启动；无头或调试
# 调用没有需要捕获的终端键盘输入。
tty_state=""
restore_tty() {
    if [[ -n "$tty_state" ]]; then
        stty "$tty_state" 2>/dev/null || true
        tty_state=""
    fi
}
if ((headless == 0)) && [[ -t 0 ]]; then
    tty_state="$(stty -g 2>/dev/null || true)"
    if [[ -n "$tty_state" ]]; then
        stty -ixon -ixoff 2>/dev/null || true
        trap restore_tty EXIT INT TERM
    fi
fi

if ((debug)); then
    emit_debug_marker() {
        [[ "${OS_DEBUG_PROTOCOL:-0}" == "1" ]] || return 0
        printf '%s\n' "$1"
    }

    pid_file="$build_dir/qemu-debug.pid"
    if [[ -f "$pid_file" ]]; then
        old_pid="$(cat "$pid_file" 2>/dev/null || true)"
        if [[ "$old_pid" =~ ^[0-9]+$ ]] && kill -0 "$old_pid" 2>/dev/null; then
            old_command="$(ps -p "$old_pid" -o comm= 2>/dev/null | tr -d '[:space:]')"
            case "$old_command" in
                qemu-system-*)
                    kill "$old_pid" 2>/dev/null || true
                    for _ in $(seq 1 20); do
                        kill -0 "$old_pid" 2>/dev/null || break
                        sleep 0.1
                    done
                    kill -9 "$old_pid" 2>/dev/null || true
                    ;;
                *)
                    echo "build/qemu-debug.pid 指向非 QEMU 进程 $old_pid ($old_command)，请手动处理" >&2
                    exit 1
                    ;;
            esac
        fi
        rm -f "$pid_file"
    fi
    emit_debug_marker 'QEMU_DEBUG_START'
    "${qemu_launcher[@]}" "$qemu_path" "${qemu_args[@]}" \
        > >(mirror_qemu_stdout) \
        2> >(mirror_qemu_stderr) &
    qemu_pid=$!
    echo "$qemu_pid" >"$pid_file"
    cleanup_debug() {
        if kill -0 "$qemu_pid" 2>/dev/null; then kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true; fi
        rm -f "$pid_file"
        restore_tty
    }
    trap cleanup_debug EXIT INT TERM
    ready=0
    for _ in $(seq 1 150); do
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            echo "QEMU 在 GDB 端口就绪前退出，请查看 $stderr_log" >&2
            exit 1
        fi
        if (exec 3<>"/dev/tcp/127.0.0.1/${gdb_port}") 2>/dev/null; then
            exec 3>&-
            ready=1
            break
        fi
        sleep 0.1
    done
    ((ready)) || { echo "QEMU GDB 端口 ${gdb_port} 未就绪" >&2; exit 1; }
    emit_debug_marker 'QEMU_DEBUG_READY'
    wait "$qemu_pid"
else
    run_status=0
    if ((keep_open)); then
        "${qemu_launcher[@]}" "$qemu_path" "${qemu_args[@]}" \
            > >(mirror_qemu_stdout) \
            2> >(mirror_qemu_stderr) || run_status=$?
    else
        timeout --foreground --signal=TERM "${seconds}s" \
            "${qemu_launcher[@]}" "$qemu_path" "${qemu_args[@]}" \
            > >(mirror_qemu_stdout) \
            2> >(mirror_qemu_stderr) || run_status=$?
        if ((run_status == 124)); then run_status=0; fi
    fi
    if ((run_status != 0)); then
        echo "QEMU 退出码：$run_status，请查看 $stderr_log" >&2
        exit "$run_status"
    fi
fi
