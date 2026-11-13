set pagination off
target remote /tmp/system-keys.sock
# 只恢复有界的诊断额度。不更改应用寄存器、内存、代码或调度状态。
set *(unsigned int*)&g_linux_chrome_trace_bytes = 0
detach
quit
