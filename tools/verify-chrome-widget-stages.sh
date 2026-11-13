#!/usr/bin/env bash
set -euo pipefail

log_path="${1:-}"
if [[ -z "$log_path" || ! -r "$log_path" ]]; then
    echo "usage: $0 SERIAL_LOG" >&2
    exit 2
fi

required_markers=(
    WIDGET_INIT_PLATFORM_BEGIN
    WIDGET_INIT_PLATFORM_DONE
    WIDGET_INIT_COMPOSITOR_BEGIN
    WIDGET_INIT_COMPOSITOR_DONE
    WIDGET_INIT_ACCEL_BEGIN
    WIDGET_INIT_ACCEL_DONE
    WIDGET_INIT_HOST_BEGIN
    WIDGET_INIT_HOST_DONE
    WIDGET_INIT_DONE
    SURFACE_PRESENT
)

# 对当前运行时中完整的空白分隔记录只扫描一次。
awk -v stages="${required_markers[*]}" '
function ordered(s, previous, line) {
    previous = 0
    for (s = 1; s <= count; ++s) {
        line = first_line[s]
        if (line <= previous) return 0
        previous = line
    }
    return 1
}
BEGIN {
    count = split(stages, suffix, " ")
    for (s = 1; s <= count; ++s) index_of["OS_" suffix[s]] = s
}
{
    for (field = 1; field <= NF; ++field) {
        token = $field
        sub(/\r$/, "", token)
        if (token in index_of) {
            key = index_of[token]
            if (!(key in first_line)) {
                first_line[key] = NR
                ++seen_count
                # 后续日志不能改变首次出现的记录。
                if (seen_count == count && ordered()) exit 0
            }
        }
    }
}
END {
    if (ordered()) { print "Chrome widget stages: OK"; exit 0 }
    print "missing or out-of-order Chrome widget stages (one complete namespace required)" > "/dev/stderr"
    exit 1
}' "$log_path"
