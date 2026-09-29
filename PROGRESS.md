# 当前状态

- 目标：保持最小 x86_64 UEFI → kernel 调试链路。
- 已有：ELF loader、串口/printf、MEM_CONVENTIONAL 早期 PMM、12x24 A8 framebuffer 文本。
- 构建：仅保留 `build/debug/` 调试路径。
- 验证：`make test`，随后 `make run`；F5 使用 QEMU + GDB。
- 下一步：在现有最小基础上继续页表和内核栈。
