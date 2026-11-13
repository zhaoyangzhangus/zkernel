#!/usr/bin/env bash
# 仅允许使用 FXSAVE64 之外不引入额外扩展寄存器状态的特性。
# KVM 暴露所有宿主 CPU 的特性交集；TCG 保持基线配置。
qemu_host_has_cpu_flags() {
    local cpu_info="$1"
    shift
    [[ -r "$cpu_info" ]] || return 1
    awk -v wanted="$*" '
        BEGIN { count=split(wanted, flags, " ") }
        /^flags[[:space:]]*:/ {
            cpus++
            for (j=1; j<=count; j++) {
                found=0
                for (i=3; i<=NF; i++) if ($i==flags[j]) found=1
                if (!found) missing=1
            }
        }
        END { exit !(cpus>0 && !missing) }
    ' "$cpu_info"
}

qemu_select_cpu_model() {
    local cpu_info="$1" accelerated="$2" chrome="$3"
    local model='qemu64,rdrand=on,xsave=off,avx=off,avx2=off'
    if [[ "$accelerated" == 1 && "$chrome" == 1 && -r "$cpu_info" ]]; then
        local host_flag qemu_flag
        for host_flag in ssse3 sse4_1 sse4_2 popcnt; do
            if qemu_host_has_cpu_flags "$cpu_info" "$host_flag"; then
                qemu_flag="${host_flag//_/.}"
                model+=",${qemu_flag}=on"
            fi
        done
    fi
    # KVM 使用稳定 TSC 时才能向来宾通报精确频率；不修改 TCG 和不满足条件的宿主。
    if [[ "$accelerated" == 1 ]] &&
        qemu_host_has_cpu_flags "$cpu_info" constant_tsc nonstop_tsc; then
        model+=",invtsc=on"
    fi
    printf '%s\n' "$model"
}
