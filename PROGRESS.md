# 当前状态

- 构建：仅 `build/debug/`；F5 按 `make all` → QEMU → GDB，连接后自动 continue；没有用户断点不暂停，命中用户断点才停。VS Code tasks 使用 shared terminal，优先复用已有空闲终端。
- 页表：继续使用 UEFI identity/direct map；不建立新页表、不修改 CR3。
- PMM：容量统计含 Loader/BootServices/Conventional；当前 seed 仅 Conventional；4K:2M = 1:7。
- VM：独立 VA manager，不分配数据页、不做 mapping。
- VM free-space：改为 Linux vmalloc 风格 augmented RB-tree + address-sorted doubly-linked list。
- RB augmentation：每个 node 维护 `subtree_max`，alloc 可跳过最大空洞不足的子树。
- free/merge：tree lower_bound 定位后，通过 list 的 prev/next O(1) 取得地址相邻 range。
- kernel VA arena：0xFFFF800000000000..0xFFFFC00000000000（64 TiB）。
- 下一步：单独实现 page-table mapping/page-fault 层。
