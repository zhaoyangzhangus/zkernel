# 0001：早期物理内存分配器直接复用 UEFI memory map

第一版 PMM 不建立 bitmap、frame array、buddy 或 free list，也不支持释放。

公开分配接口只有 `pmm_alloc(size)`。参数 `size` 是字节数，内部自动向上对齐到 4 KiB；因此任意非零大小都会占用整数个物理页，并返回一段足以容纳该大小的物理连续内存。

分配器直接扫描并原地收缩 UEFI memory map 描述符。由于调用发生在 `ExitBootServices()` 成功之后，可分配类型包括：

- `MEM_CONVENTIONAL`
- `MEM_BOOT_SERVICES_CODE`
- `MEM_BOOT_SERVICES_DATA`

找到足够大的范围后，从低地址端切走对齐后的大小，并推进该描述符的 `physical_start`、减少 `number_of_pages`。因此 UEFI memory map 本身就是剩余 free range 的元数据。

`LoaderCode/LoaderData` 暂时不回收，因为当前 kernel ELF 装载区、`BOOT_INFO` 和最终 memory map buffer 仍可能位于这些类型中；直接回收会覆盖仍在使用的数据。RuntimeServices、ACPI NVS、MMIO 等也继续保留。等这些 loader 占用范围能被精确排除后，再考虑回收 Loader 类型。

这个阶段接受每次分配 O(descriptor_count) 扫描。UEFI descriptor 数量很少，而且无 free 意味着不会产生运行期碎片管理需求。后续需要释放、并发、NUMA 或高频分配时再替换为正式 PMM。
