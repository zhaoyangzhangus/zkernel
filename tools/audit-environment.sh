#!/usr/bin/env bash
# 对 Linux/QEMU 测试宿主进行只读审计。
set -u -o pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$root_dir"

build_dir="${BUILD:-build}"
gpu_bdf="${OS_GPU_PCI:-}"
audio_bdf="${OS_GPU_AUDIO_PCI:-}"
modprobe_cmd="${MODPROBE:-}"
required_failures=0
passthrough_failures=0

usage() {
    cat <<'EOF'
usage: tools/audit-environment.sh [--build DIR] [--gpu-pci BDF]
                                [--gpu-audio-pci BDF]

The audit is read-only. It checks the Linux build/QEMU tools, stale Windows
build state, and the IOMMU/VFIO preconditions for an RTX 5070 passthrough.
Environment: BUILD, OS_GPU_PCI, OS_GPU_AUDIO_PCI. Command-line options override them.
EOF
}

while (($# > 0)); do
    case "$1" in
        --build)
            (($# >= 2)) || { echo "--build needs a directory" >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --gpu-pci)
            (($# >= 2)) || { echo "--gpu-pci needs a BDF" >&2; exit 2; }
            gpu_bdf="$2"
            shift 2
            ;;
        --gpu-audio-pci)
            (($# >= 2)) || { echo "--gpu-audio-pci needs a BDF" >&2; exit 2; }
            audio_bdf="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

pass() { printf 'PASS  %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; }
fail_required() {
    printf 'MISS  %s\n' "$*"
    required_failures=$((required_failures + 1))
}
fail_passthrough() {
    printf 'BLOCK %s\n' "$*"
    passthrough_failures=$((passthrough_failures + 1))
}

check_command() {
    local command_name="$1"
    local description="${2:-$1}"
    if command -v "$command_name" >/dev/null 2>&1; then
        pass "$description: $(command -v "$command_name")"
    else
        fail_required "$description: command '$command_name' is missing"
    fi
}

if [[ -z "$modprobe_cmd" ]]; then
    for candidate in /usr/sbin/modprobe /sbin/modprobe \
        /usr/bin/modprobe /bin/modprobe; do
        if [[ -x "$candidate" ]]; then
            modprobe_cmd="$candidate"
            break
        fi
    done
fi

printf 'System Linux/QEMU environment audit\n'
printf 'root=%s\n' "$root_dir"
printf 'build=%s\n' "$build_dir"
printf 'host=%s\n' "$(uname -srmo 2>/dev/null || echo unknown)"
printf '\n[build and test tools]\n'

for command_name in \
    bash make gcc g++ ar objcopy objdump ld \
    qemu-system-x86_64 qemu-img gdb python3 \
    awk sed grep find cmp mkdir cp truncate timeout tee perl; do
    check_command "$command_name"
done

if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    pass "/dev/kvm is readable and writable"
else
    warn "/dev/kvm is unavailable; QEMU can fall back to TCG, but KVM tests are not ready"
fi

firmware="${OVMF_CODE:-}"
if [[ -z "$firmware" ]]; then
    for candidate in \
        /usr/share/OVMF/OVMF_CODE_4M.fd \
        /usr/share/OVMF/OVMF_CODE.fd \
        /usr/share/edk2/ovmf/OVMF_CODE.fd \
        /usr/share/qemu/edk2-x86_64-code.fd; do
        if [[ -f "$candidate" ]]; then
            firmware="$candidate"
            break
        fi
    done
fi
if [[ -f "$firmware" ]]; then
    pass "OVMF_CODE: $firmware"
else
    fail_required "OVMF_CODE firmware is missing (install ovmf or set OVMF_CODE)"
fi

printf '\n[System entrypoints]\n'
for entrypoint in run-qemu.sh \
    tools/run-qemu-matrix.sh tools/verify-qemu-stages.sh \
    tools/verify-roadmap-stages.sh tools/verify-debug-locations.sh; do
    if [[ ! -x "$entrypoint" ]]; then
        fail_required "$entrypoint is not executable"
    elif bash -n "$entrypoint" >/dev/null 2>&1; then
        pass "$entrypoint: executable and parses as Bash"
    else
        fail_required "$entrypoint does not parse as Bash"
    fi
done

printf '\n[target toolchains]\n'
for tool_name in gcc ar objcopy objdump; do
    tool="x86_64-w64-mingw32-$tool_name"
    if command -v "$tool" >/dev/null 2>&1; then
        pass "PE/COFF target $tool"
    else
        fail_required "PE/COFF target $tool is missing (install MinGW-w64)"
    fi
done

elf_ready=1
for tool_name in gcc g++ ld objcopy objdump; do
    dedicated="x86_64-elf-$tool_name"
    if ! command -v "$dedicated" >/dev/null 2>&1; then
        elf_ready=0
    fi
done
if ((elf_ready)); then
    pass "ELF runtime target: x86_64-elf-*"
elif command -v gcc >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1 && \
     command -v ld >/dev/null 2>&1 && command -v objcopy >/dev/null 2>&1 && \
     command -v objdump >/dev/null 2>&1; then
    warn "x86_64-elf-* is missing; the Linux Makefile fallback will use native ELF tools"
else
    fail_required "neither x86_64-elf-* nor a complete native ELF toolchain is available"
fi

if [[ -d "$build_dir" ]]; then
    stale_dep_count="$(grep -RIlE '(^|[[:space:]])[A-Za-z]:/' \
        --include='*.d' "$build_dir" 2>/dev/null | wc -l)"
    if ((stale_dep_count == 0)); then
        pass "no Windows drive-letter paths found in $build_dir/*.d"
    else
        warn "$stale_dep_count dependency files in $build_dir contain Windows paths; rebuild affected targets with GNUmakefile"
    fi
else
    warn "build directory does not exist yet: $build_dir"
fi

if [[ "$(stat -f -c %T . 2>/dev/null || true)" == fuse* ]]; then
    warn "project is on a FUSE/Windows filesystem; a native Linux filesystem is recommended for build speed and file locking"
fi

printf '\n[RTX 5070 VFIO preflight]\n'
if ! command -v lspci >/dev/null 2>&1; then
    fail_passthrough "pciutils/lspci is missing"
else
    if [[ -z "$gpu_bdf" ]]; then
        gpu_bdf="$(lspci -Dnn 2>/dev/null | awk '/VGA compatible controller.*\[10de:2f04\]/ {print $1; exit}')"
    fi
    if [[ -z "$audio_bdf" && -n "$gpu_bdf" ]]; then
        gpu_domain="${gpu_bdf%%:*}"
        gpu_bus_slot="${gpu_bdf#*:}"
        gpu_bus_slot="${gpu_bus_slot%.*}"
        audio_bdf="${gpu_domain}:${gpu_bus_slot}.1"
    fi
fi

canonical_bdf() {
    local value="$1"
    value="${value#0000:}"
    if [[ "$value" =~ ^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
        printf '0000:%s\n' "$value"
        return 0
    fi
    return 1
}

if ! gpu_bdf="$(canonical_bdf "$gpu_bdf" 2>/dev/null)"; then
    fail_passthrough "RTX 5070 BDF was not found; set OS_GPU_PCI=0000:bb:ss.f"
fi
if ! audio_bdf="$(canonical_bdf "$audio_bdf" 2>/dev/null)"; then
    fail_passthrough "RTX 5070 audio BDF was not found; set OS_GPU_AUDIO_PCI=0000:bb:ss.f"
fi

group_link=""
if [[ -n "$gpu_bdf" && -e "/sys/bus/pci/devices/$gpu_bdf" ]]; then
    vendor="$(cat "/sys/bus/pci/devices/$gpu_bdf/vendor" 2>/dev/null || true)"
    device_id="$(cat "/sys/bus/pci/devices/$gpu_bdf/device" 2>/dev/null || true)"
    if [[ "$vendor:$device_id" == '0x10de:0x2f04' ]]; then
        pass "GPU $gpu_bdf is NVIDIA GB205 / RTX 5070"
    else
        fail_passthrough "GPU $gpu_bdf is $vendor:$device_id, expected 0x10de:0x2f04"
    fi
    group_link="$(readlink -f "/sys/bus/pci/devices/$gpu_bdf/iommu_group" 2>/dev/null || true)"
    if [[ -n "$group_link" && -d "$group_link/devices" ]]; then
        group_id="$(basename "$group_link")"
        group_devices=()
        for device_path in "$group_link"/devices/*; do
            [[ -e "$device_path" ]] || continue
            group_devices+=("$(basename "$device_path")")
        done
        pass "GPU is in IOMMU group $group_id: ${group_devices[*]}"
        if ((${#group_devices[@]} == 2)) &&
           printf '%s\n' "${group_devices[@]}" | grep -Fxq "$gpu_bdf" &&
           printf '%s\n' "${group_devices[@]}" | grep -Fxq "$audio_bdf"; then
            pass "IOMMU group is isolated to GPU and its audio function"
        else
            fail_passthrough "IOMMU group $group_id contains devices other than $gpu_bdf and $audio_bdf"
        fi
        reserved_regions_file="$group_link/reserved_regions"
        if [[ -r "$reserved_regions_file" ]]; then
            direct_regions="$(awk '$3 == "direct" { print }' "$reserved_regions_file")"
            if [[ -n "$direct_regions" ]]; then
                fail_passthrough "IOMMU group $group_id advertises firmware-mandated direct/1:1 regions; VFIO Type1 container setup may be rejected"
                while IFS= read -r region; do
                    [[ -n "$region" ]] || continue
                    printf 'INFO  group %s direct region: %s\n' "$group_id" "$region"
                done <<< "$direct_regions"
                warn "check sudo dmesg for 'Firmware has requested this device have a 1:1 IOMMU mapping'; review BIOS DMA Protection or a kernel/firmware fix"
            else
                pass "IOMMU group $group_id has no firmware-mandated direct regions"
            fi
        else
            warn "IOMMU group $group_id does not expose reserved_regions"
        fi
    else
        fail_passthrough "GPU has no usable IOMMU group; enable AMD IOMMU in firmware/kernel"
    fi
else
    fail_passthrough "GPU sysfs device is missing: $gpu_bdf"
fi

if [[ -n "$audio_bdf" && -e "/sys/bus/pci/devices/$audio_bdf" ]]; then
    audio_id="$(cat "/sys/bus/pci/devices/$audio_bdf/vendor" 2>/dev/null || true):$(cat "/sys/bus/pci/devices/$audio_bdf/device" 2>/dev/null || true)"
    if [[ "$audio_id" == '0x10de:0x2f80' ]]; then
        pass "audio function $audio_bdf is NVIDIA HDA"
    else
        fail_passthrough "audio function $audio_bdf is $audio_id, expected 0x10de:0x2f80"
    fi
else
    fail_passthrough "GPU audio function sysfs device is missing: $audio_bdf"
fi

if [[ -n "$gpu_bdf" && -e "/sys/bus/pci/devices/$gpu_bdf/driver" ]]; then
    gpu_driver="$(basename "$(readlink -f "/sys/bus/pci/devices/$gpu_bdf/driver")")"
    printf 'INFO  GPU driver: %s\n' "$gpu_driver"
    if [[ "$gpu_driver" == nouveau ]]; then
        warn "GPU is owned by nouveau; it must be detached before QEMU starts"
    elif [[ "$gpu_driver" == vfio-pci ]]; then
        pass "GPU is already bound to vfio-pci"
    else
        warn "GPU is owned by $gpu_driver; the passthrough launcher will not detach it silently"
    fi
fi

if [[ -e "/sys/bus/pci/devices/$gpu_bdf/boot_vga" ]] &&
   [[ "$(cat "/sys/bus/pci/devices/$gpu_bdf/boot_vga" 2>/dev/null || echo 0)" == 1 ]]; then
    fail_passthrough "GPU is boot_vga=1 and currently owns the host console; switch host display to another GPU first"
fi

if [[ -d /sys/bus/pci/drivers/vfio-pci ]]; then
    pass "vfio-pci driver is present in sysfs"
else
    fail_passthrough "vfio-pci is not loaded (install kmod, then load vfio-pci/vfio_iommu_type1)"
fi
if [[ -n "$modprobe_cmd" && -x "$modprobe_cmd" ]]; then
    pass "modprobe is available: $modprobe_cmd"
else
    fail_passthrough "modprobe is missing (install kmod)"
fi
if [[ -e /dev/vfio/vfio ]]; then
    pass "/dev/vfio/vfio exists"
else
    fail_passthrough "/dev/vfio/vfio is missing; VFIO modules are not active"
fi
if [[ -n "$group_link" ]]; then
    group_id="$(basename "$group_link")"
    if [[ -e "/dev/vfio/$group_id" ]]; then
        pass "/dev/vfio/$group_id exists"
    else
        warn "/dev/vfio/$group_id is not present yet; it appears after binding the group to vfio-pci"
    fi
fi

printf '\n[summary]\n'
if ((required_failures == 0)); then
    pass 'Linux/QEMU test prerequisites are present'
else
    printf 'MISS  %d Linux/QEMU prerequisite(s) are missing\n' "$required_failures"
fi
if ((passthrough_failures == 0)); then
    pass 'RTX 5070 VFIO preflight is ready'
else
    printf 'BLOCK %d RTX 5070 VFIO precondition(s) are not ready\n' "$passthrough_failures"
fi

if ((required_failures != 0 || passthrough_failures != 0)); then
    exit 1
fi
