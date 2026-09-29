# 当前状态

- 构建：仅 `build/debug/`；debugcon 0xE9 直接输出宿主终端。
- PMM：只使用 `MEM_CONVENTIONAL`。
- PMM bootstrap：先模拟预留 metadata，按预留后的实际 RAM 动态计算 PTE/PDE pool 容量和三层位图大小，再从 UEFI memory map 真正切出 metadata。
- global PTE/PDE：三层 64 叉，但 middle/leaf/slot 数组按实际 frame 数动态分配，不再固定占 64³ slots。
- per-CPU PTE：两层 64 叉，容量为 min(PTE frames, 4096)，当前只有 bootstrap CPU。
- frame：opaque 64-bit；无 allocated 元数据、无 frame→slot 反查。
- 下一步：建立页表，并把 per-CPU PTE pool 接入 CPU-local。
