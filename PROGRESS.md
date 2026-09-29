# 当前状态

- 目标：最小 x86_64 UEFI → kernel 调试链路。
- 构建：仅 `build/debug/`。
- 输出：删除 16550/COM1 驱动，`printf` 直接写 QEMU debugcon 0xE9 到宿主终端。
- 内核：最终 UEFI memory map、仅 MEM_CONVENTIONAL 的 `pmm_alloc(size)`、12x24 A8 framebuffer 文本。
- 精简：删除 `tests/` 和 `drivers/`。
- 下一步：页表与内核栈。
