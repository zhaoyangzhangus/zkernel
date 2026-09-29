# 当前状态

- 目标：最小 x86_64 UEFI → kernel 调试链路。
- 构建：仅 `build/debug/`；生成物不进 Git。
- 内核：串口/printf、最终 UEFI memory map、仅 MEM_CONVENTIONAL 的 `pmm_alloc(size)`、12x24 A8 framebuffer 文本。
- 精简：删除 ESP/release 路径、提交的 build/dist、重复设计文档、loader 重复 memory-map 统计和内核启动自检。
- 验证：`make test && make run`，F5 使用 QEMU + GDB。
- 下一步：页表与内核栈。
