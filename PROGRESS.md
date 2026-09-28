# 当前状态

- 当前目标：建立最小可运行的 x86_64 内核基础设施。
- 当前任务：第一版物理内存分配 + 早期 framebuffer 文字输出。
- 当前问题：已修复 PMM 只接受 ConventionalMemory 导致 QEMU 启动时分配失败的问题。
- 测试状态：PMM 现在使用 ExitBootServices 后可回收的 ConventionalMemory、BootServicesCode、BootServicesData，同时继续保留 LoaderCode/Data 和 RuntimeServices；host 测试覆盖这些类型。12x24 A8 字体 host 测试已加入。需要本地执行 `make test` 和 `make run`。
- 下一步：确认 QEMU 中 PMM 分配成功，再用 PMM 建立内核自己的页表/栈。
