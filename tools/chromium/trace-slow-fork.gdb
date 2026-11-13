set pagination off
target remote /tmp/system-keys.sock
# 当前内核反汇编：在采集完所有阶段时间戳后调用。
# 重新构建内核后必须重新验证此地址。不设置入口或阶段断点：
# 调试器暂停不能污染测得的 fork 时间区间。
hbreak *0xffffffff8001a030
commands
silent
printf "SLOW_FORK root=%#lx areas=%lu ptes=%lu lock_ns=%lu objects_ns=%lu protect_ns=%lu finish_ns=%lu status=%ld\n", $rsi, $rdx, $rcx, $r8, $r9, *(unsigned long*)$rsp, *(unsigned long*)($rsp+8), *(long*)($rsp+16)
delete breakpoints
detach
quit
end
continue
