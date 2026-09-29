# 当前状态

- 构建：仅 `build/debug/`；debugcon 0xE9 输出宿主终端。
- 页表：继续使用 UEFI identity/direct map；不建立新页表、不修改 CR3。
- PMM：容量统计含 Loader/BootServices/Conventional；当前 seed 仅 Conventional；4K:2M = 1:7。
- VM：新增独立 VA range manager，只 reserve/free 虚拟地址，不分配数据页、不做 mapping。
- VM：sorted free-range list + first-fit + free 合并；range metadata page 从 4K PMM 获取。
- kernel VA arena：0xFFFF800000000000..0xFFFFC00000000000（64 TiB）。
- 下一步：单独实现 page-table mapping/page-fault 层，把物理页按需挂到已 reserve 的 VA。
