set pagination off
target remote /tmp/system-keys.sock
# 当前内核：位于 x86_tsc_to_ns(finished - started) 之后。
# 所有时间戳都在这唯一的断点触发前保存。
hbreak *0xffffffff80019db3
commands
silent
printf "FORK_PHASES total_ns=%lu lock_cycles=%lu objects_cycles=%lu protect_cycles=%lu finish_cycles=%lu areas=%lu ptes=%lu\n", $rax, *(unsigned long*)($rsp+0x38)-*(unsigned long*)($rsp+0x18), *(unsigned long*)($rsp+0x48)-*(unsigned long*)($rsp+0x38), $r15-*(unsigned long*)($rsp+0x48), $rbp-$r15, $r12, $r14
delete breakpoints
detach
quit
end
continue
