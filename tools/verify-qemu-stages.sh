#!/usr/bin/env bash
set -euo pipefail

verification_mode=strict
if [[ "${1:-}" == "--interactive" ]]; then
    verification_mode=interactive
    shift
fi
if (($# > 1)) || [[ "${1:-}" == --* ]]; then
    echo 'usage: tools/verify-qemu-stages.sh [--interactive] [qemu-serial.log]' >&2
    exit 2
fi
log_path="${1:-${STAGE_LOG:-${BUILD:-build}/qemu-serial.log}}"
contracts_path="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/roadmap-stage-contracts.tsv"

if [[ ! -f "$log_path" ]]; then
    echo "stage verification: log not found: $log_path" >&2
    exit 2
fi
if [[ ! -f "$contracts_path" ]]; then
    echo "stage verification: contract file not found: $contracts_path" >&2
    exit 2
fi

# 这些是稳定的启动/运行边界。此处刻意不要求细粒度 USER_RUNTIME 跟踪记录；这些记录用于定位用户映像内部故障，
# 而下列标记定义可启动调试映像的验收边界。
required_markers=(
    'OS_KERNEL_OK'
    'OS_CPU_INIT_OK'
    'OS_MM_OK'
    'OS_BUDDY_RANDOM_OK'
    'OS_CONTEXT_SWITCH_OK'
    'OS_DRIVER_OK'
    'OS_LITEFS_OK'
    'OS_NET_MANAGER_OK'
    'OS_DISPLAY_CORE_OK'
    'OS_USER_INIT_STARTED SERVICES=6'
    'OS_LIBC_TEST_OK'
    'OS_USER_RUNTIME_OK'
    'OS_USER_RUNTIME_SUBTESTS_OK'
    'OS_USERMODE_OK'
    'OS_WINDOW_SERVER_KERNEL_OK'
    'OS_WINDOW_WORKER_OK'
)

required_stages=(
    'OS_STAGE phase=BOOT step=5 value=1'
    'OS_STAGE phase=CPU step=5 value=1'
    'OS_STAGE phase=MEMORY step=5 value=1'
    'OS_STAGE phase=MEMORY step=14 value=2048'
    'OS_STAGE phase=SCHED step=5 value=1'
    'OS_STAGE phase=DRIVER step=5 value=1'
    'OS_STAGE phase=STORAGE step=5 value=1'
    'OS_STAGE phase=NETWORK step=5 value=1'
    'OS_STAGE phase=DISPLAY step=5 value=1'
    'OS_STAGE phase=USER_RUNTIME step=5 value=1'
    'OS_STAGE phase=DESKTOP step=4 value=1'

    # 规范阶段 0..18 均有运行时自测边界。阶段 19/20 在发布门禁实现前刻意标记为待处理；
    # 保留这些标记仍可明确指出停止位置。
    'OS_STAGE phase=SPEC_P0_REPOSITORY step=5 value=1'
    'OS_STAGE phase=SPEC_P1_CPU step=5 value=1'
    'OS_STAGE phase=SPEC_P2_MEMORY step=5 value=1'
    'OS_STAGE phase=SPEC_P3_SLAB step=5 value=1'
    'OS_STAGE phase=SPEC_P4_SMP step=5 value=1'
    'OS_STAGE phase=SPEC_P5_SCHEDULER step=5 value=1'
    'OS_STAGE phase=SPEC_P6_VM step=5 value=1'
    'OS_STAGE phase=SPEC_P7_SYSCALL step=5 value=1'
    'OS_STAGE phase=SPEC_P8_OBJECT step=5 value=1'
    'OS_STAGE phase=SPEC_P9_DEVICE step=5 value=1'
    'OS_STAGE phase=SPEC_P10_STORAGE step=5 value=1'
    'OS_STAGE phase=SPEC_P11_USB step=5 value=1'
    'OS_STAGE phase=SPEC_P12_NETWORK step=5 value=1'
    'OS_STAGE phase=SPEC_P13_GPU step=5 value=1'
    'OS_STAGE phase=SPEC_P14_WINDOW step=5 value=1'
    'OS_STAGE phase=SPEC_P15_AUDIO_BT step=5 value=1'
    'OS_STAGE phase=SPEC_P16_SERVICES step=5 value=1'
    'OS_STAGE phase=SPEC_P17_POWER step=5 value=1'
    'OS_STAGE phase=SPEC_P18_HARDENING step=5 value=1'
    'OS_STAGE phase=SPEC_P19_ABI step=15 value=0'
    'OS_STAGE phase=SPEC_P20_OS1 step=15 value=0'

    # 重构路线图阶段 0..9。阶段 0 在记录宿主基准和回归门禁前仍为待处理；
    # 内核中的度量边界仍能准确标识停止位置。
    'OS_STAGE phase=REFACTOR_P0_BASELINE step=15 value=0'
    'OS_STAGE phase=REFACTOR_P0_BASELINE step=16 value=1'
    'OS_STAGE phase=REFACTOR_P1_HEADERS step=5 value=1'
    'OS_STAGE phase=REFACTOR_P2_PRIMITIVES step=5 value=1'
    'OS_STAGE phase=REFACTOR_P2_PRIMITIVES step=16 value=7'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=15 value=0'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=1'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=2'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=3'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=4'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=5'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=6'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=7'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=8'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=9'
    'OS_STAGE phase=REFACTOR_P3_BOOT step=16 value=10'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=1 value=0'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=1'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=2'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=3'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=4'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=5'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=16 value=6'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=1 value=0'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=1'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=2'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=3'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=4'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=16 value=5'
    'OS_STAGE phase=REFACTOR_P4_SCHEDULER step=15 value=0'
    'OS_STAGE phase=REFACTOR_P5_PROCESS step=15 value=0'
    'OS_STAGE phase=REFACTOR_P6_MM step=1 value=0'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=1'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=2'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=3'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=4'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=5'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=6'
    'OS_STAGE phase=REFACTOR_P6_MM step=16 value=7'
    'OS_STAGE phase=REFACTOR_P6_MM step=15 value=0'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=1 value=0'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=1'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=2'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=3'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=4'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=5'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=6'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=7'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=8'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=9'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=10'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=11'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=12'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=13'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=14'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=15'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=16 value=16'
    'OS_STAGE phase=REFACTOR_P7A_GRAPHICS step=15 value=0'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=15 value=0'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=1'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=2'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=3'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=4'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=5'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=6'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=7'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=8'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=9'
    'OS_STAGE phase=REFACTOR_P7B_COMPOSITOR step=16 value=10'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=1'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=2'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=3'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=4'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=5'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=6'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=7'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=8'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=9'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=10'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=11'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=12'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=13'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=14'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=15'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=16'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=17'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=18'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=19'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=20'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=21'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=22'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=23'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=16 value=24'
    'OS_STAGE phase=REFACTOR_P8_DRIVERS step=15 value=0'
    'OS_STAGE phase=REFACTOR_P9_CLEANUP step=15 value=0'
)

if [[ "$verification_mode" == interactive ]]; then
    # This mode is opt-in: never infer success from a missing self-test.
    # The kernel must explicitly confirm both interactive-only skips and
    # the real desktop/display publication boundaries instead.
    interactive_markers=()
    for marker in "${required_markers[@]}"; do
        case "$marker" in
            OS_LIBC_TEST_OK|OS_USER_RUNTIME_OK|OS_USER_RUNTIME_SUBTESTS_OK|OS_USERMODE_OK) ;;
            *) interactive_markers+=("$marker") ;;
        esac
    done
    required_markers=("${interactive_markers[@]}"
        OS_USER_RUNTIME_SKIPPED_FOR_INTERACTIVE_DESKTOP
        OS_SMP_USER_DISPATCH_SKIPPED_FOR_INTERACTIVE_DESKTOP
        OS_DESKTOP_CACHE_OK OS_FRAMEBUFFER_UC_OK OS_DISPLAY_COMMIT_OK)
    interactive_stages=()
    for marker in "${required_stages[@]}"; do
        if [[ "$marker" != 'OS_STAGE phase=USER_RUNTIME step=5 value=1' ]]; then
            interactive_stages+=("$marker")
        fi
    done
    required_stages=("${interactive_stages[@]}")
fi

missing=()
has_record() {
    # Required records contain literal space-separated fields. Never accept a
    # success token embedded in a legacy name or a longer field value.
    grep -Eq -- "(^|[[:space:]])${1}([[:space:]]|$)" "$log_path"
}
for marker in "${required_markers[@]}"; do
    if ! has_record "$marker"; then
        missing+=("$marker")
    fi
done
for marker in "${required_stages[@]}"; do
    if ! has_record "$marker"; then
        missing+=("$marker")
    fi
done

# Keep the detailed operational checks above, but make the phase list itself
# come from the roadmap manifest.  This prevents a newly added phase from
# being accepted by the static checker while disappearing from stage-sanity.
while IFS='|' read -r family phase serial_name owner_source; do
    [[ -n "$family" && "${family:0:1}" != "#" ]] || continue
    if ! has_record "OS_STAGE phase=$serial_name"; then
        missing+=("OS_STAGE phase=$serial_name")
    fi
done < "$contracts_path"

# Match diagnostic tokens, not prefixes or text embedded in a different name.
# An optional whitespace-separated host timestamp does not change the record.
loader_failure_pattern='(^|[[:space:]])OS_DIAG_RT_FAIL([[:space:]]|$)'
libc_failure_pattern='(^|[[:space:]])OS_LIBC_TEST_(DETAIL_)?FAIL([[:space:]]|$)'
stage_failure_pattern='(^|[[:space:]])OS_STAGE_FAIL([[:space:]]|$)'

# A successful boot marker must not mask a failed runtime subtest.
if grep -Eq -- "$loader_failure_pattern" "$log_path"; then
    echo "stage verification: dynamic loader failed in $log_path" >&2
    grep -E -- 'OS_DIAG_RT_(FAIL|PROTECT_)' "$log_path" >&2 || true
    exit 1
fi

if grep -Eq -- "$libc_failure_pattern" "$log_path"; then
    echo "stage verification: native libc test failed in $log_path" >&2
    grep -E -- "$libc_failure_pattern" "$log_path" >&2 || true
    exit 1
fi

if grep -Eq -- "$stage_failure_pattern" "$log_path"; then
    echo "stage verification: explicit stage failure found in $log_path" >&2
    grep -E -- "$stage_failure_pattern" "$log_path" >&2 || true
    exit 1
fi

# A stage without a source location is not actionable in a debugger.  Keep
# the complete log on disk, but report only the offending records here.
unlocated_stages=$(grep -E '^OS_STAGE(_FAIL)? phase=' "$log_path" |
    grep -Ev ' loc=kernel/[^[:space:]]+:[0-9]+([[:space:]]|$)' || true)
if [[ -n "$unlocated_stages" ]]; then
    echo "stage verification: unlocated stage record found in $log_path" >&2
    printf '%s\n' "$unlocated_stages" >&2
    exit 1
fi

# A syntactically valid location is not enough: a stale path or an out-of-date
# line number makes a debug build impossible to navigate.  Resolve every
# location against the current source tree and require the recorded source
# line to contain the stage publication call (or its small helper wrapper).
# Failure helpers carry the call-site through a local *_fail macro, so accept
# that wrapper spelling as well.
stage_locations=$(sed -nE '/^OS_STAGE(_FAIL)? phase=/ s/.* loc=([^[:space:]]*).*/\1/p' \
    "$log_path" | sort -u)
location_errors=0
while IFS= read -r location; do
    [[ -n "$location" ]] || continue
    case "$location" in
        kernel/*:[1-9][0-9]*) ;;
        *)
            printf 'stage verification: invalid source location: %s\n' \
                "$location" >&2
            location_errors=$((location_errors + 1))
            continue
            ;;
    esac

    source_path="${location%:*}"
    source_line="${location##*:}"
    if [[ ! -f "$source_path" ]]; then
        printf 'stage verification: source file not found for %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
        continue
    fi

    source_line_count=$(wc -l < "$source_path")
    if ((source_line > source_line_count)); then
        printf 'stage verification: source line out of range: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
        continue
    fi

    source_text=$(sed -n "${source_line}p" "$source_path" || true)
    if [[ "$source_text" != *kernel_debug_stage* &&
          "$source_text" != *debug_trace_stage* &&
          ! "$source_text" =~ [[:alnum:]_]+_fail[[:space:]]*\( ]]; then
        printf 'stage verification: source line is not a stage call: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
    elif [[ "$source_text" != *OS_DEBUG_PHASE_* &&
            ! "$source_text" =~ [[:alnum:]_]+_fail[[:space:]]*\( ]]; then
        printf 'stage verification: source line is a shared stage helper, not a call site: %s\n' \
            "$location" >&2
        location_errors=$((location_errors + 1))
    fi
done <<< "$stage_locations"

if ((location_errors != 0)); then
    exit 1
fi

if ((${#missing[@]} != 0)); then
    echo "stage verification: missing markers in $log_path" >&2
    printf '  %s\n' "${missing[@]}" >&2
    diagnostics=$(grep -Ein \
        'fail|error|fatal|panic|abort|invalid|cannot|undefined|assert|timeout|fault|halt' \
        "$log_path" | tail -n 24 || true)
    if [[ -n "$diagnostics" ]]; then
        printf '%s\n' "$diagnostics" >&2
    else
        echo "no explicit failure line; full log: $log_path" >&2
    fi
    exit 1
fi

sequence_error=''
if ! sequence_error=$(awk '
/OS_STAGE phase=/ {
    for (i = 1; i <= NF; ++i) {
        current = $i
        sub(/^seq=/, "", current)
        sub(/\r$/, "", current)
        if (current !~ /^[0-9]+$/) continue
        if (count != 0 && (current + 0) <= (previous + 0)) {
            printf "sequence is not strictly increasing: %s after %s\n", current, previous
            bad = 1
        }
        previous = current
        ++count
    }
}
END {
    if (count == 0) {
        print "no sequenced OS_STAGE records found"
        bad = 1
    }
    exit bad ? 1 : 0
}
' "$log_path"); then
    echo "stage verification: invalid stage sequence in $log_path" >&2
    printf '%s\n' "$sequence_error" >&2
    exit 1
fi

stage_count=$(grep -Fc -- 'OS_STAGE phase=' "$log_path" || true)
if [[ "${STAGE_VERIFY_VERBOSE:-0}" == "1" ]]; then
    echo "QEMU stage verification: OK ($verification_mode, $log_path, $stage_count stage records)"
fi
