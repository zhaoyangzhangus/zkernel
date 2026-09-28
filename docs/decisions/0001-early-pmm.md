# 0001：早期物理内存分配器直接复用 UEFI memory map

第一版 PMM 不建立 bitmap、frame array、buddy 或 free list，也不支持释放。

公开分配接口只有 `pmm_alloc(size)`。参数 `size` 是字节数，内部自动向上对齐到 4 KiB；因此任意非零大小都会占用整数个物理页，并返回一段足以容纳该大小的物理连续内存。

分配器只扫描 `MEM_CONVENTIONAL` 描述符。找到足够大的范围后，从低地址端切走对齐后的大小，并直接推进该描述符的 `physical_start`、减少 `number_of_pages`。因此 UEFI memory map 本身就是剩余 free range 的元数据。

物理地址 0 是合法结果，所以 `pmm_alloc()` 不能以 0 表示失败。失败统一返回 `PMM_ALLOC_FAILED`（`UINT64_MAX`）；该值不可能是 4 KiB 对齐的物理页起始地址。

LoaderCode/Data、BootServicesCode/Data、RuntimeServices、ACPI、MMIO 等类型当前全部不参与分配。

这个阶段接受每次分配 O(descriptor_count) 扫描。后续需要释放、并发、NUMA 或高频分配时再替换为正式 PMM。
