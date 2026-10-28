#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD:-build}"
elf_path="${1:-$build_dir/esp/EFI/OS/kernel.elf}"
log_path="${2:-$build_dir/qemu-serial.log}"
objdump_cmd="${OBJDUMP:-x86_64-w64-mingw32-objdump}"

if [[ ! -f "$elf_path" ]]; then
    printf 'debug location verification: ELF not found: %s\n' "$elf_path" >&2
    exit 2
fi
if [[ ! -f "$log_path" ]]; then
    printf 'debug location verification: stage log not found: %s\n' "$log_path" >&2
    exit 2
fi
if ! command -v "$objdump_cmd" >/dev/null 2>&1; then
    printf 'debug location verification: objdump not found: %s\n' "$objdump_cmd" >&2
    exit 2
fi

if ! "$objdump_cmd" -t "$elf_path" |
    grep -F 'kernel_debug_stage_at' >/dev/null; then
    printf 'debug location verification: stage symbol missing from %s\n' \
        "$elf_path" >&2
    exit 1
fi

# 尽管中间链接对象为 PE/COFF，内核最终仍链接为 ELF 调试映像。检查两个有代表性的源文件，
# 避免被剥离或仅含符号的映像意外通过 F5 约定检查。
for source in kernel/debug_stage.c kernel/init/early.c; do
    if ! "$objdump_cmd" --dwarf=info "$elf_path" |
        grep -F "$source" >/dev/null; then
        printf 'debug location verification: DWARF source missing: %s\n' \
            "$source" >&2
        exit 1
    fi
done

"$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/verify-qemu-stages.sh" \
    "$log_path"

# 基准记录也是面向调试器的约定。只有计时值而没有调用位置，虽可用于绘图，却无法解释重构回归。
# 此检查与阶段验证分开，因为刻意在早期启动失败的日志中可能尚无基准记录。
benchmark_records=$(grep -E '^OS_BENCH(_VALUE)? ' "$log_path" || true)
if [[ -n "$benchmark_records" ]]; then
    unlocated_benchmarks=$(printf '%s\n' "$benchmark_records" |
        grep -Ev '^OS_BENCH(_VALUE)? name=[^[:space:]]+ (cycles|value)=[0-9]+ loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
    if [[ -n "$unlocated_benchmarks" ]]; then
        printf 'debug location verification: unlocated benchmark record in %s\n' \
            "$log_path" >&2
        printf '%s\n' "$unlocated_benchmarks" >&2
        exit 1
    fi

    benchmark_locations=$(printf '%s\n' "$benchmark_records" |
        sed -n 's/.* loc=\(kernel\/[^[:space:]]*:[0-9][0-9]*\).*/\1/p' |
        sort -u)
    benchmark_location_errors=0
    while IFS= read -r location; do
        [[ -n "$location" ]] || continue
        source_path="${location%:*}"
        source_line="${location##*:}"
        if [[ ! -f "$source_path" ]]; then
            printf 'debug location verification: benchmark source not found: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
            continue
        fi
        source_line_count=$(wc -l < "$source_path")
        if ((source_line > source_line_count)); then
            printf 'debug location verification: benchmark line out of range: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
            continue
        fi
        source_text=$(sed -n "${source_line}p" "$source_path" || true)
        if [[ "$source_text" != *kernel_perf_emit_scope* &&
              "$source_text" != *kernel_perf_emit_value* ]]; then
            printf 'debug location verification: benchmark line is not a perf call: %s\n' \
                "$location" >&2
            benchmark_location_errors=$((benchmark_location_errors + 1))
        fi
    done <<< "$benchmark_locations"
    if ((benchmark_location_errors != 0)); then
        exit 1
    fi
fi

# The kmalloc summary predates the generic OS_BENCH format, but it is
# still a benchmark record.  Require the same source location so a memory
# allocator regression can be opened at the report call site.
kmalloc_records=$(grep -E '^OS_PERF_KMALLOC_OK ' "$log_path" || true)
if [[ -n "$kmalloc_records" ]]; then
    unlocated_kmalloc=$(printf '%s\n' "$kmalloc_records" |
        grep -Ev '^OS_PERF_KMALLOC_OK .* loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
    if [[ -n "$unlocated_kmalloc" ]]; then
        printf 'debug location verification: unlocated kmalloc report in %s\n' \
            "$log_path" >&2
        printf '%s\n' "$unlocated_kmalloc" >&2
        exit 1
    fi

    kmalloc_locations=$(printf '%s\n' "$kmalloc_records" |
        sed -n 's/.* loc=\(kernel\/[^[:space:]]*:[0-9][0-9]*\).*/\1/p' |
        sort -u)
    kmalloc_location_errors=0
    while IFS= read -r location; do
        [[ -n "$location" ]] || continue
        source_path="${location%:*}"
        source_line="${location##*:}"
        if [[ ! -f "$source_path" ]]; then
            printf 'debug location verification: kmalloc source not found: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
            continue
        fi
        source_line_count=$(wc -l < "$source_path")
        if ((source_line > source_line_count)); then
            printf 'debug location verification: kmalloc line out of range: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
            continue
        fi
        source_text=$(sed -n "${source_line}p" "$source_path" || true)
        if [[ "$source_text" != *kernel_perf_emit_report* ]]; then
            printf 'debug location verification: kmalloc line is not a perf report call: %s\n' \
                "$location" >&2
            kmalloc_location_errors=$((kmalloc_location_errors + 1))
        fi
    done <<< "$kmalloc_locations"
    if ((kmalloc_location_errors != 0)); then
        exit 1
    fi
fi

printf 'debug location verification: OK (%s, %s)\n' "$elf_path" "$log_path"
