set pagination off
target remote /tmp/system-keys.sock
# 已根据 19c3db402a4771a9fa2e85fd18932777d9465c5da62588a92bba265decbdd518 验证。
# 所有时间戳均在暂停前采集，因此不包含调试器暂停时间。
break *0xffffffff80019df3
commands
silent
if $rax >= 50000000
printf "FORK_PHASES total_ns=%lu lock_cycles=%lu objects_cycles=%lu protect_cycles=%lu finish_cycles=%lu areas=%lu ptes=%lu\n", $rax, *(unsigned long*)($rsp+0x38)-*(unsigned long*)($rsp+0x18), *(unsigned long*)($rsp+0x48)-*(unsigned long*)($rsp+0x38), $r15-*(unsigned long*)($rsp+0x48), $rbp-$r15, $r12, $r14
delete breakpoints
detach
quit
end
continue
end
continue
