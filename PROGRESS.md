# 当前状态

- 构建：仅 `build/debug/`；F5 单后台 task，GDB 连接后自动 continue。
- UEFI map debug：在任何 `boot_alloc_pages()` 修改 descriptor 前，完整打印 ExitBootServices 后传入内核的原始 memory map。
- Early paging：打印原始 map 后立即 `paging_early_takeover()`；在 PMM/VM 初始化之前切换到自有 CR3。
- 全部 usable RAM：LoaderCode/Data、BootServicesCode/Data、Conventional 全部同时建立 identity 与 high-half direct map。
- Page-table bootstrap：不再预估/整块预留；每缺一个 4K table page 就直接调用 `boot_alloc_pages(1)`，从最低 Conventional 物理地址向上分配，页表页永久保留并从后续 PMM 可见范围中消失。
- Mapping policy：仅合并“UEFI type 相同且物理地址连续”的 descriptor；合并后的每个 extent 再按 1G→2M→4K 映射；不同 type 永不跨边界合并。
- MMIO/framebuffer/runtime：不进入普通 WB direct map，后续按各自 cache/property 单独映射。
- PMM：在自有页表生效后初始化；容量统计含 Loader/BootServices/Conventional，当前 seed 仍只 Conventional；4K:2M = 1:7。
- VM：PMM 后初始化，并把 `[VM_DIRECT_MAP_BASE, highest_usable_pa)` 统一登记为 `VM_REGION_DIRECT_MAP`；物理 hole 保持 unmapped。
- VM free-space：Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- VM used-space：独立 RB-tree，保存 allocated region 的 start/size/type/attrs。
- kernel VA：完整高 canonical half 128 TiB，由 VM 统一管理。
- 下一步：把 kernel/stack/BOOT_INFO/PMM 内部 pointer 切换到 high-half direct map，再删除 bootstrap identity map。
