#!/usr/bin/env bash
set -euo pipefail

# Ninja 可能会把被中断或因 OOM 终止的操作留下的零字节归档误认为已完成目标，因为其时间戳
# 仍然是最新的。
# 只操作调用方明确选择的生成输出目录。
out_root="${CHROMIUM_OUT:-}"
dry_run=0

while (($# > 0)); do
    case "$1" in
        --chromium-out)
            (($# >= 2)) || { echo "--chromium-out 需要参数" >&2; exit 2; }
            out_root="$2"
            shift 2
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        -h|--help)
            echo "用法：CHROMIUM_OUT=PATH $0 [--chromium-out PATH] [--dry-run]"
            exit 0
            ;;
        *)
            echo "未知参数：$1" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$out_root" ]]; then
    echo "请设置 CHROMIUM_OUT 或使用 --chromium-out 指定 Chromium 输出目录" >&2
    exit 2
fi
if [[ ! -d "$out_root/obj" ]]; then
    echo "Chromium prebuild: 输出目录或 obj 子目录不存在：$out_root" >&2
    exit 1
fi
out_root="$(CDPATH= cd -- "$out_root" && pwd)"
obj_root="$out_root/obj"

if [[ ! -d "$out_root" || ! -d "$obj_root" ]]; then
    echo "Chromium prebuild: output tree is absent; nothing to repair"
    exit 0
fi

mapfile -d '' zero_archives < <(
    find "$obj_root" -xdev -type f -name '*.a' -size 0c -print0
)

if ((${#zero_archives[@]} == 0)); then
    echo "Chromium prebuild: no zero-byte archives"
    exit 0
fi

for archive in "${zero_archives[@]}"; do
    if ((dry_run)); then
        echo "Chromium prebuild: would remove incomplete archive $archive"
    else
        echo "Chromium prebuild: removing incomplete archive $archive"
        rm -f -- "$archive"
    fi
done

if ((dry_run)); then
    echo "Chromium prebuild: found ${#zero_archives[@]} incomplete archive(s)"
else
    echo "Chromium prebuild: repaired ${#zero_archives[@]} incomplete archive(s)"
fi
