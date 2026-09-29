# 当前状态

- 构建：仅 `build/debug/`；debugcon 0xE9 输出宿主终端。
- 初始可用内存统计：LoaderCode/Data + BootServicesCode/Data + Conventional。
- PMM 配额：按物理容量 PTE 4K : PDE 2M = 1 : 7；天然 4K 边缘优先计入 PTE，不足部分整块拆 2M。
- metadata：按一次统计结果动态计算，并用 bootstrap allocator 从 MEM_CONVENTIONAL 切出。
- 当前 seed：只放 MEM_CONVENTIONAL；Loader/BootServices 等建立自有 stack/page tables 后再回收。
- global PTE/PDE：动态三层 64 叉；per-CPU PTE：动态两层 64 叉。
- 下一步：建立页表和内核栈，然后回收 Loader/BootServices。
