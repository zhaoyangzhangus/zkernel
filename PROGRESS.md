# 当前状态

- 构建：仅 `build/debug/`；F5 单后台 task，GDB 连接后自动 continue。
- Early paging：`kernel_main()` 进入后第一阶段即 `paging_early_takeover()`；在 PMM/VM 初始化之前切换到自有 CR3。
- 全部 usable RAM：LoaderCode/Data、BootServicesCode/Data、Conventional 全部同时建立 identity 与 high-half direct map。
- Page-table bootstrap：页表页从最大 Conventional descriptor 高端切出，随后 PMM 看不到这些页；建图仍覆盖 donor 原始完整范围，所以页表页自身也已映射。
- Mapping policy：每段 RAM 优先 1G，其次 2M，最后 4K；1G 支持由 CPUID 检测。
- MMIO/framebuffer/runtime：不进入普通 WB direct map，后续按各自 cache/property 单独映射。
- PMM：在自有页表生效后初始化；容量统计含 Loader/BootServices/Conventional，当前 seed 仍只 Conventional；4K:2M = 1:7。
- VM：PMM 后初始化，并把 `[VM_DIRECT_MAP_BASE, highest_usable_pa)` 统一登记为 `VM_REGION_DIRECT_MAP`；物理 hole 保持 unmapped。
- VM free-space：Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- VM used-space：独立 RB-tree，保存 allocated region 的 start/size/type/attrs。
- kernel VA：完整高 canonical half 128 TiB，由 VM 统一管理。
- 下一步：把 kernel/stack/BOOT_INFO/PMM 内部 pointer 切换到 high-half direct map，再删除 bootstrap identity map。
