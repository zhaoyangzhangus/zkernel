# zkernel

最小 x86_64 UEFI 内核实验项目。当前继续使用 UEFI 留下来的页表和 identity/direct
mapping，不建立新页表，也不切换 CR3。

## 构建

```sh
make
make run
make clean
```

只保留调试构建，内核 `printf` 通过 QEMU debugcon `0xE9` 输出宿主终端。

## PMM

初始容量统计包含 LoaderCode/Data、BootServicesCode/Data 和 Conventional。
当前实际 seed 仍只使用 Conventional。4K(PTE) : 2M(PDE) 按物理容量规划为 1:7。

## VM

`mm/vm.c` 仍然只管理虚拟地址范围，不做任何页表映射。

free VA 管理改为 Linux vmalloc 风格的两套索引：

```text
augmented RB-tree
  key = range.start
  subtree_max = 子树中最大的 free range

address-sorted doubly-linked list
  prev / next = 地址上直接相邻的 free range
```

因此：

- `vm_alloc()` 用 `subtree_max` 跳过不可能容纳请求的整棵子树，不再从链表头顺序扫描。
- `vm_reserve()` 用 RB-tree 按地址定位包含指定 VA 的 free range。
- `vm_free()` 用 RB-tree 找 lower_bound，然后通过 `prev/next` O(1) 取得左右邻居并合并。
- RB-tree 的插入/删除/旋转同步维护 `subtree_max`。
- range node 不足时仍从 4K PMM 取一页作为 metadata。

当前 kernel VM arena：

```text
0xFFFF800000000000 .. 0xFFFFC00000000000
```

共 64 TiB。这只是 VA ownership；不会写 PTE/PDE，也不会访问返回的 VA。

## 调试

VS Code 直接按 F5；或使用 `make run-gdb`。
