# 当前状态

- 构建：仅 `build/debug/`；F5 单后台 task，GDB 连接后自动 continue。
- PMM：容量统计含 Loader/BootServices/Conventional；当前 seed 仅 Conventional；4K:2M = 1:7。
- Paging：新增自有 4-level page tables；`paging_takeover()` 建表后执行 `mov cr3`，不再继续使用 UEFI CR3。
- Mapping policy：RAM range 优先 1G page，其次 2M，最后 4K；1G page 通过 CPUID 检测。
- Bootstrap identity：暂时保留 Loader/BootServices/Conventional + PMM bootstrap metadata 的低地址映射，保证当前 kernel/UEFI stack/PMM pointers 在 CR3 切换后继续有效。
- Kernel direct map：`VA = VM_DIRECT_MAP_BASE + PA`；当前只映射普通 RAM，不映射 MMIO/framebuffer。
- VM：direct-map window 作为 `VM_REGION_DIRECT_MAP` 统一登记；物理 hole 保持页表 unmapped，但 VA window 不再分给其它用途。
- VM free-space：Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- VM used-space：独立 RB-tree，保存 allocated region 的 start/size/type/attrs。
- kernel VA：完整高 canonical half 128 TiB，由 VM 统一管理。
- 下一步：迁移 kernel stack/PMM pointers 到 high-half direct map，之后删除 bootstrap identity map；再实现通用 map/unmap/page-fault 层。
