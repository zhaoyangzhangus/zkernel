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

VM 仍然只管理虚拟地址，不做页表 mapping，但现在同时管理 free space 和
allocated region metadata。

```text
free_root
  augmented RB-tree
  subtree_max = 子树最大空洞
  + address-sorted doubly list

used_root
  RB-tree
  保存每个已占用 region：
    start/end
    type
    attrs
```

region type 当前包含 generic/kernel/heap/stack/DMA/MMIO/framebuffer/ACPI/user。

region attrs 当前保存 RWX、user、guard、pinned、lazy 和 WB/WC/UC cache policy。
这些属性暂时只作为 metadata；后续 page-table mapping 层根据它们生成真正的
PTE/PAT 属性。

接口：

```c
vm_alloc(... type, attrs, &va)
vm_reserve(... type, attrs)
vm_query(space, any_va_inside_region, &info)
vm_free(space, region_start)
```

`vm_query()` 可以通过 region 内任意 VA 查出它属于哪一个区间以及该区间类型和属性。
`vm_free()` 不再需要调用者重复传 size，因为 used region 自己保存 end。

当前 kernel VM arena 为
`0xFFFF800000000000..0xFFFFC00000000000`（64 TiB）。

## 调试

F5 只有一个后台 task；GDB 连接后自动 continue，没有用户断点就不停。
