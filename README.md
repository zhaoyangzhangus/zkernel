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

`mm/vm.c` 是独立的虚拟地址区间管理器，只管理 VA 所有权。

不会修改页表、不会写 PTE/PDE、不会切 CR3、不会建立映射，也不会访问分配出的 VA。

```c
vm_space_init()
vm_alloc()
vm_reserve()
vm_free()
```

第一版使用按地址排序的 free-range list：first-fit 分配，free 自动合并相邻区间。
range node 不够时，从 4K PMM 取一页作为 VM metadata；这些 metadata 页依靠当前
UEFI identity map 访问。

当前 kernel VM arena：

```text
0xFFFF800000000000 .. 0xFFFFC00000000000
```

共 64 TiB。这里的地址只是被 reserve，不代表已经可访问。

## 调试

VS Code 直接按 F5；或使用 `make run-gdb`。
