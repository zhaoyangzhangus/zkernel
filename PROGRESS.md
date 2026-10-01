# 当前状态

- 构建：仅 `build/debug/`；F5 单后台 task，GDB 连接后自动 continue。
- UEFI map debug：在任何 `boot_alloc_pages()` 修改 descriptor 前，完整打印 ExitBootServices 后传入内核的原始 memory map。
- Early paging：打印原始 map 后立即 `paging_early_takeover()`；在 PMM/VM 初始化之前切换到自有 CR3。
- 全部 usable RAM：LoaderCode/Data、BootServicesCode/Data、Conventional 全部同时建立 identity 与 high-half direct map。
- Page-table bootstrap：不再预估/整块预留；每缺一个 4K table page 就直接调用 `boot_alloc_pages(1)`，从最高 Conventional 物理地址向下分配，页表页永久保留并从后续 PMM 可见范围中消失。
- Mapping policy：仅合并“UEFI type 相同且物理地址连续”的 descriptor；合并后的每个 extent 再按 1G→2M→4K 映射；不同 type 永不跨边界合并。
- MMIO/framebuffer/runtime：不进入普通 WB direct map，后续按各自 cache/property 单独映射。
- PMM：在自有页表生效后初始化；容量统计含 Loader/BootServices/Conventional，当前 seed 仍只 Conventional；4K:2M = 1:7。
- VM：PMM 后初始化；direct map 不再按 `[base, max_pa)` 整段占用，而是扫描当前页表，只把真正 PRESENT 的连续 direct-map extent 登记为 `VM_REGION_DIRECT_MAP`；物理 hole 对应 VA 保持可分配。
- VM free-space：Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- VM used-space：独立 RB-tree，保存 allocated region 的 start/size/type/attrs。
- kernel VA：完整高 canonical half 128 TiB，由 VM 统一管理。
- 下一步：把 kernel/stack/BOOT_INFO/PMM 内部 pointer 切换到 high-half direct map，再删除 bootstrap identity map。

- Page fault v1：建立最小 IDT 的 #PF gate；VM_ALLOC 的 LAZY 区域在首次 not-present fault 时通过 vm_query() 校验已分配范围，PMM 分配 4K demand-zero frame，再写当前页表并 iret 重试。
- Runtime page-table update：PML4[511] 作为 512 GiB recursive mapping window；缺失的 PDPT/PD/PT 可在 #PF 路径中从 PMM 4K pool 动态补齐。当前生成 PRESENT/WRITE/USER，并按 VM cache attrs 编码 PAT；NX 尚未接入。

- PAT：IA32_PAT 采用固定布局，完整支持 WB/WC/UC-/UC/WT/WP；4K 使用 PAT bit7，2M/1G 使用 PAT bit12；每个 CPU 单独初始化。
- Framebuffer：GOP 传递物理 base + FrameBufferSize；kernel VM 为其分配独立 VA，按 4K 显式映射为 WC，并将可访问 VA 写入 fb_base。

- IDT：完整安装 256 个 vector stub；统一汇编入口保存 GPR、规范 exception error-code 并进入 C dispatcher。
- Interrupt vectors：0x20..0xEF 为动态设备向量池，支持 alloc/alloc_at/free；0xF0..0xFF 预留 kernel timer/IPI/spurious。
- IRQ handler registry：支持每向量注册一个 handler + context；注册/分配状态使用原子操作；#PF 继续走 demand paging。

- bootmem：删除未使用的 boot_release_pages()；bootstrap allocator 现在明确为只分配、不回收。
- VM debug：新增 vm_for_each_region()，启动时按地址顺序打印所有已占用虚拟区间及 type/attrs。

- VM region naming：PML4[511] 的 512 GiB recursive page-table window 改为专用 VM_REGION_RECURSIVE，不再标记为 RESERVED。

- VM free：vm_free() 现在逐页解除 PRESENT 4K PTE 并 invlpg；VM_ATTR_PMM_OWNED region 的 leaf frame 会归还 PMM，未 fault 的 LAZY 页直接跳过。
- Mapping ownership：新增 VM_ATTR_PMM_OWNED，区分由 VM/PMM 拥有的 RAM frame 与 framebuffer/MMIO 等外部物理映射。
- Runtime unmap：新增 paging_unmap_4k_current()；当前不拆 2M/1G large leaf，也暂不回收空 PT/PD/PDPT。
- Free self-test：lazy heap fault 成功后执行 vm_free()，启动日志会打印回收成功并再次输出 VMM layout。
