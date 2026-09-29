# 当前状态

- 构建：仅 `build/debug/`；debugcon 0xE9 直接输出宿主终端。
- PMM：只使用 `MEM_CONVENTIONAL`。
- PMM bootstrap：一次扫描计算 PTE/PDE frame 上限和 metadata 大小，然后直接 boot_alloc；不再迭代收敛。
- global PTE/PDE：动态长度三层 64 叉；PTE capacity 额外留 511 slots 覆盖 metadata 切分造成的新 2M 边缘。
- per-CPU PTE：两层 64 叉，最多 4096 slots，当前只有 bootstrap CPU。
- frame：opaque 64-bit；无 allocated 元数据、无 frame→slot 反查。
- 下一步：建立页表并把 per-CPU PTE pool 接入 CPU-local。
