# 当前状态

- 构建：仅 `build/debug/`；F5 单后台 task，GDB 连接后自动 continue。
- 页表：继续使用 UEFI identity/direct map；不建立新页表、不修改 CR3。
- PMM：容量统计含 Loader/BootServices/Conventional；当前 seed 仅 Conventional；4K:2M = 1:7。
- VM free-space：Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- VM used-space：新增独立 RB-tree，保存 allocated region 的 start/end/type/attrs。
- VM region type：generic/kernel/heap/stack/DMA/MMIO/framebuffer/ACPI/direct-map/reserved/user。
- VM attrs：RWX/user/guard/pinned/lazy + WB/WC/UC cache policy；当前仅保存 metadata，不写页表。
- VM query：可由 region 内任意 VA 查询所属 region；free 直接由 region metadata 得到 size。
- VM 是 kernel VA 的统一 owner；固定用途用 reserve 登记，动态用途用 alloc，不设 DMA/MMIO/heap 等独立 VA allocator。
- kernel VA：管理完整高 canonical half 0xFFFF800000000000..0xFFFFFFFFFFFFFFFF（128 TiB）；tree 内部使用相对 base offset。
- 下一步：page-table mapping 层读取 VM region attrs，生成实际 PTE/PAT 映射属性。
