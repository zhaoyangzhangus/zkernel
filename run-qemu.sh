#!/usr/bin/env bash
# Run System with the Linux QEMU/OVMF stack.
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
# Boot from the removable FAT volume by default.  The USB-root kernel path
# already validates /EFI/OS/init and /EFI/OS/init-runtime before selecting it as /.  Keep an
# explicit NVMe switch for diagnostics and for comparing the old path.
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
# QEMU's GTK Wayland client can keep a stale black surface even when the
# guest scanout (and HMP screendump) is correct.  Prefer the XWayland path
# when an X display is available; QEMU_GDK_BACKEND can explicitly select
# wayland, x11, or another GTK backend for hosts that need a different path.
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
        # The nested-device test needs the xHCI topology without a storage
        # device occupying the direct root port, so retain its NVMe-root test
        # semantics even though normal boots default to USB.
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

# Canonical physical network selection. Never detach a host driver here:
# the existing VFIO preflight must fail closed if the NIC is not ready.
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

# Chrome maps large read-only assets and performs many small profile I/O
# operations during startup. Keep USB as the explicit default for storage
# validation, but let the runtime-oriented launch select the fast NVMe path.
if ((chrome_fast)); then
    usb_storage=0
fi
if ((emulated_net == 0)) && [[ -n "$net_dump" ]]; then
    echo "--net-dump 仅支持模拟网卡，不能与 --no-emulated-net 同用" >&2
    exit 2
fi

# B10B-3 HMP HOTPLUG MONITOR
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

# Keep one QEMU launcher per BUILD alive at a time.  The lock is held by this
# shell for the full QEMU lifetime, including timeout/debug wrappers, so a
# second invocation cannot rebuild the shared disk or silently start a second
# VM against a different inode.
command -v flock >/dev/null 2>&1 || {
    echo "缺少 flock（util-linux），无法保证同一 BUILD 只运行一台 QEMU" >&2
    exit 1
}
exec {qemu_lock_fd}>"$build_dir/qemu-run.lock"
if ! flock -n "$qemu_lock_fd"; then
    echo "该 BUILD 已有 QEMU 启动器运行，拒绝并行构建/启动：$build_dir" >&2
    exit 1
fi

# Validate an explicitly requested evidence destination before compiling or
# replacing the disk. Never silently create a missing removable mount path.
stream_log_run=""
if [[ -n "$stream_log_dir" ]]; then
    [[ -d "$stream_log_dir" ]] || {
        echo "完整日志目录不存在：$stream_log_dir" >&2; exit 1;
    }
    stream_log_run="$(mktemp -d "$stream_log_dir/qemu-stream.XXXXXX")" || exit 1
    echo "QEMU 完整输出镜像：$stream_log_run"
fi

# Fail before builds or disk replacement when this process cannot open/map
# the requested devices. Image-only preparation needs neither VFIO nor mlock.
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
    # COM1 is byte-serial at 115200 baud and every diagnostic character stalls
    # the guest while it waits for the UART.  Keep normal runtime boots fast;
    # opt into the diagnostic path only when --serial-trace is requested.
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

# The kernel VFS self-tests create temporary entries at the ESP root.  QEMU
# 10's vvfat backend can leave those entries visible on the host after the
# guest removes them; on the next boot they make the mkdir/create checks fail
# (or trigger a vvfat assertion).  Quarantine every known self-test artifact
# before constructing the synthetic FAT image.  Moving keeps the evidence
# recoverable while making the boot directory deterministic.
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

# Do not boot directly from QEMU's `fat:rw:<directory>` backend.  Its vvfat
# implementation can abort the whole emulator when a guest truncates or
# fsyncs a file (for example Ctrl+S in notepad).  Materialize a normal FAT
# disk image from a read-only directory view, then let the guest write that
# raw image.  The generated image is disposable and never mutates build/esp.
qemu_secondary_image="$build_dir/qemu-nvme-data.img"

# qemu-img's legacy `fat:ro:` source defaults to FAT16 and refuses a Chrome
# payload larger than roughly 512 MiB.  Keep the small-image path fast, but
# materialize large images as FAT32 with a little writable headroom for
# /tmp/chrome-data and the guest's runtime files.
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

# Split OVMF images need their matching writable variable flash. Keep it
# alongside the canonical disk, and never replace firmware settings on restart.
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

# A non-interactive headless launch must not make QEMU's serial backend depend
# on the stdout process-substitution pipe.  That pipe changes TCG timing under
# Popen/VS Code task runners and can hide a guest wakeup race.  Keep the live
# terminal path unchanged, while giving headless automation a direct serial
# file that is still mirrored to qemu-serial.log below.
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
    # Keep guest RAM below the platform's IVMD reserved ranges around 2 GiB.
    # The rest of RAM is placed above 4 GiB; DMA isolation remains enabled.
    qemu_machine+=",max-ram-below-4g=${QEMU_MAX_RAM_BELOW_4G:-1G}"
fi

qemu_args=(
    -machine "$qemu_machine"
    -vga none
    -m "$qemu_memory"
    # The freestanding crypto tests require a strong seed.  QEMU's qemu64
    # model hides RDRAND by default, so expose the virtual instruction without
    # enabling AVX/XSAVE, which the kernel does not yet preserve. Chrome/KVM
    # may use host-supported SSSE3/SSE4/POPCNT with the existing FXSAVE64 state.
    -cpu "$qemu_cpu_model"
    -smp "$cpu_count"
    -drive "if=pflash,format=raw,readonly=on,file=$firmware"
    "${firmware_vars_args[@]}"
    -boot order=c
    # Interactive launches use QEMU stdout as the live COM1 stream; headless
    # non-PTY launches use the direct serial file selected above so polling
    # tools do not depend on a process-substitution pipe.
    -serial "$serial_device"
    -d guest_errors
    -D "$build_dir/qemu.log"
)

if ((!keep_open)); then
    # 有时限的诊断保留停止现场；常驻桌面遵循来宾的关机和重启请求。
    qemu_args+=( -no-reboot -no-shutdown )
fi

if ((stdvga)); then
    # P14: deterministic QEMU Standard VGA.  A configured mode lets OVMF
    # expose a real 1920x1080 GOP instead of merely scaling an 800x600 RAMFB.
    vga_spec="VGA,vgamem_mb=${vga_memory_mb}"
    if ((vga_width > 0 && vga_height > 0)); then
        vga_spec+=",xres=${vga_width},yres=${vga_height}"
    fi
    qemu_args+=( -device "$vga_spec" )
elif ((ramfb)); then
    # RAMFB is a system device rather than a PCI VGA device.  It gives the
    # GTK backend a visible EFI framebuffer without consuming MMIO space or
    # changing the BAR placement of a VFIO GPU.
    qemu_args+=( -device ramfb )
fi

if ((usb_storage)); then
    # The generated image is deliberately the only boot disk in USB mode, so
    # both OVMF and System exercise the same U盘 filesystem and write path.
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
    # Capture keyboard input while the pointer is over the QEMU window.  GTK
    # does not provide the macOS-only `full-grab` option; `grab-on-hover` is
    # the supported Linux mechanism and avoids sending guest keys to the
    # launcher terminal when the VM window is active.
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
    # B10B-2 NESTED USB HUB TOPOLOGY
    #
    # Physical topology:
    #
    #   xHCI root port 1
    #        |
    #        +-- Hub #1
    #              |
    #              +-- port 1: Hub #2
    #                          |
    #                          +-- port 1: Mouse
    #
    # xHCI route strings expected by System:
    #
    #   Hub #1 : 0x00000
    #   Hub #2 : 0x00001
    #   Mouse  : 0x00011
    #
    # Keep the topology independent of the display backend so automated HMP
    # tests can inject input in headless QEMU as well as in the GTK window.
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
    # USB boot already created the xHCI controller for the storage device;
    # add the input devices to that same controller in every display mode.
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
    # Network boot is not used: leave the physical NIC to the guest driver,
    # without exposing an optional vendor PXE/UEFI expansion ROM.
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
# The physical NIC is canonical; emulation is an explicit diagnostic mode.
# `-net dump` was removed from newer QEMU builds, so the optional diagnostic
# path adds a filter to this explicitly named user-mode NIC.
if ((emulated_net)); then
qemu_args+=(
    # Chrome reaches 127.0.0.1:7890 through SLIRP's host alias. Normal NAT
    # sockets are independent per connection, without a guestfwd subprocess
    # or a persistent TCP chardev that could mix concurrent TLS streams.
    -netdev "user,id=vm-net,host=10.0.2.2"
    -device "e1000e,netdev=vm-net,mac=52:54:00:12:34:56"
)
else
    # Suppress QEMU's implicit default NIC too. VFIO NICs remain unaffected.
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

# Keep the complete stream in the two log files, but keep the interactive
# terminal quiet unless a line describes a failure or an abnormal stop.
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
    # QEMU normally reports the deliberate timeout termination on stderr.  It
    # remains in qemu-stderr.log, but only diagnostic-looking lines reach the
    # interactive terminal.
    tee "${stderr_targets[@]}" |
        awk '!/terminating on signal 15 from pid/ &&
             tolower($0) ~ /error|fail|fatal|panic|abort|invalid|cannot|could not|unsupported|assert|segmentation|fault/ {
            print
            fflush()
        }' >&2
}

# Ctrl+S is VSTOP (XOFF) in a host terminal.  If the QEMU GTK window has not
# taken focus yet, the terminal consumes that byte instead of the emulated
# USB keyboard.  Disable software flow control for this launcher and restore
# the exact terminal mode on every exit path.  This only applies to an
# interactive graphical launch; headless/debug invocations have no terminal
# keyboard to capture.
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
