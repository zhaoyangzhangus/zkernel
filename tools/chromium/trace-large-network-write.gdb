set pagination off
target remote /tmp/system-bing-send.sock
# 入口帧偏移来自 arch_trap_frame_t，而非 Chromium 符号。
# 仅当此 ELF 与已部署的内核匹配时使用。硬件断点可避免修改正在运行的内核指令。
hbreak linux_syscall_dispatch if (*(unsigned long long*)($rdi+112) == 44 || *(unsigned long long*)($rdi+112) == 1) && *(unsigned long long*)($rdi+88) > 4096
commands
silent
printf "LARGE_NETWORK_WRITE syscall=%llu fd=%llu length=%llu\n", *(unsigned long long*)($rdi+112), *(unsigned long long*)($rdi+72), *(unsigned long long*)($rdi+88)
delete breakpoints
detach
quit
end
continue
