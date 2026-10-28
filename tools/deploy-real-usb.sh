#!/usr/bin/env bash
# Deliberately restricted to the user's confirmed Lenovo K121, not a generic disk writer.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
check_only=0
case "${1:-}" in
    --check) check_only=1; shift ;;
    '') ;;
    *) echo '用法：sudo bash tools/deploy-real-usb.sh（清空指定 U 盘）；--check 仅检查' >&2; exit 2 ;;
esac
[[ $# == 0 ]] || exit 2
[[ $check_only == 1 || $EUID == 0 ]] || { echo '请用 sudo bash 运行此脚本。' >&2; exit 1; }
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$project_dir/build/esp"
if [[ $check_only == 0 ]]; then
    python3 "$project_dir/tools/migrate-system-layout.py" "$source_dir"
fi
python3 "$project_dir/tools/finalize-app-layout.py" "$source_dir" --strict
device_link=/dev/disk/by-id/usb-Lenovo_K121_AA000000000000000375-0:0
device="$(readlink -e -- "$device_link")"
[[ -b "$device" && "$(lsblk -dnro TRAN "$device")" == usb ]] || exit 1
device_bytes="$(lsblk -bdnro SIZE "$device")"
[[ $device_bytes -gt 120000000000 && $device_bytes -lt 130000000000 ]] || exit 1
[[ "$(lsblk -dnro PTTYPE "$device")" == gpt ]] || exit 1
mapfile -t partitions < <(lsblk -nrpo NAME,TYPE "$device" | awk '$2=="part" {print $1}')
[[ ${#partitions[@]} == 1 && "${partitions[0]}" == "${device}1" ]] || {
    echo '分区布局与已核对的 U 盘不符，拒绝格式化。' >&2; exit 1;
}
partition="${partitions[0]}"
for utility in parted mkfs.fat rsync mount umount sha256sum flock fuser; do
    command -v "$utility" >/dev/null || { echo "缺少工具：$utility" >&2; exit 1; }
done
for required in EFI/BOOT/BOOTX64.EFI EFI/OS/kernel.elf EFI/OS/loader.conf \
                EFI/OS/init EFI/OS/init-runtime usr/apps/fileman/fileman usr/apps/chrome/chrome usr/apps/cube/cube lib/libgraphics.so.1 \
                usr/apps/chrome/lib/libGLX_nvidia.so.0 \
                usr/apps/chrome/vulkan/nvidia_icd.json EFI/OS/firmware/rtl8126/rtl8126a-2.fw EFI/OS/firmware/rtl8126/rtl8126a-3.fw; do
    [[ -s "$source_dir/$required" ]] || { echo "缺少部署文件：$required" >&2; exit 1; }
done
[[ -z "$(find "$source_dir" -type f -size +4294967295c -print -quit)" ]] || {
    echo '存在超过 FAT32 限制的文件，拒绝部署。' >&2; exit 1;
}
# Hold the launcher's existing lock for the entire deployment, including
# manifest creation. Opening read-only does not truncate or create anything.
exec {deploy_lock_fd}<"$project_dir/build/qemu-run.lock"
flock -n "$deploy_lock_fd" || {
    echo 'QEMU 启动器仍占用统一构建目录，拒绝部署。' >&2; exit 1;
}
if fuser "$project_dir/build/qemu-fat.img" >/dev/null 2>&1; then
    echo '请先关闭当前 QEMU，再运行本脚本。' >&2; exit 1;
fi
if [[ $check_only == 1 ]]; then
    echo "只读预检通过：Lenovo K121 $device，UEFI、内核、Chrome、cube 和 NVIDIA ICD 文件齐全；尚未格式化。"
    exit 0
fi
temporary="$(mktemp -d /tmp/system-usb-deploy.XXXXXX)"
mounted=0
cleanup() {
    if [[ $mounted == 1 ]]; then sync; umount "$temporary" || return; fi
    rmdir -- "$temporary" 2>/dev/null || true
}
trap cleanup EXIT
manifest="$(mktemp /tmp/system-usb-manifest.XXXXXX)"
trap 'cleanup; rm -f -- "$manifest"' EXIT
(cd "$source_dir" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$manifest"
systemctl stop system-usb-journal.service
if findmnt -rn -S "$partition" >/dev/null; then umount "$partition"; fi
# Formatting is authorized; the exact stable USB identity was validated above.
echo "清除 Lenovo K121：$device（不备份）"
parted --script "$device" set 1 esp on
mkfs.fat -F 32 -n SYSTEM "$partition"
mount "$partition" "$temporary"
mounted=1
rsync -r --info=progress2 "$source_dir/" "$temporary/"
sync
(cd "$temporary" && sha256sum --quiet -c "$manifest")
sync
umount "$temporary"
mounted=0
echo '完成：UEFI 启动文件、内核、Chrome、cube 已复制并逐文件校验；U 盘已卸载，可拔出。'
