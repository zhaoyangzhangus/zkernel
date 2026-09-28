# 0001：早期物理页分配器直接复用 UEFI memory map

第一版 PMM 不建立 bitmap、frame array、buddy 或 free list，也不支持释放。

`pmm_alloc_pages(n)` 分配 `n` 个物理连续的 4 KiB 页。分配器扫描 UEFI memory map，只使用 `MEM_CONVENTIONAL` 描述符。找到足够大的范围后，从低地址端切走请求的页，并直接推进该描述符的 `physical_start`、减少 `number_of_pages`。因此 UEFI memory map 本身就是剩余 free range 的元数据。

暂时不回收 `LoaderCode`、`LoaderData` 和 `BootServicesCode/Data`。当前内核映像、`BOOT_INFO` 和最终 memory map 缓冲区仍可能位于这些类型中；在没有显式保留这些占用范围之前，把这些类型直接加入 free pool 会有覆盖自身的风险。

这个阶段接受每次分配 O(descriptor_count) 扫描。UEFI descriptor 数量很少，而且无 free 意味着不会产生运行期碎片管理需求。后续需要释放、并发、NUMA 或高频分配时再替换为正式 PMM。
