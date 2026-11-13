set pagination off
target remote /tmp/system-keys.sock
# 已验证的暂存/未剥离 build ID 为 bb5196af759dd50f，PIE 基址为 0x40000000。
# 使用不同 Chrome 镜像前，先确认这些符号。
set $accelerator_count = 0
hbreak *0x4e95cb00
commands
silent
printf "FOCUS_ACCEL manager=%p accelerator=%p\n", $rdi, $rsi
x/4wx $rsi
set $accelerator_count = $accelerator_count + 1
if $accelerator_count >= 8
delete breakpoints
detach
quit
end
continue
end
hbreak *0x51ee7420
commands
silent
printf "BROWSER_ACCEL browser=%p accelerator=%p\n", $rdi, $rsi
x/4wx $rsi
continue
end
continue
