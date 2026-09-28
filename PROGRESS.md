# 当前状态

- 当前目标：建立最小可运行的 x86_64 内核基础设施。
- 当前任务：第一版物理内存分配 + 早期 framebuffer 文字输出。
- 当前问题：修复 PMM 将物理地址 0 与“分配失败”混用的问题。
- 测试状态：PMM 仍然只使用 MEM_CONVENTIONAL；失败值改为 UINT64_MAX，host 测试显式覆盖从物理地址 0 开始的 ConventionalMemory。12x24 A8 字体 host 测试已加入。需要本地执行 `make test` 和 `make run`。
- 下一步：确认 QEMU 中两次 PMM 分配成功，再继续页表/内核栈。
