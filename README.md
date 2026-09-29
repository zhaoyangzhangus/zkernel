# zkernel

最小 x86_64 UEFI 内核实验项目。UEFI loader 读取 `kernel.elf`，按 PT_LOAD
装入物理内存，取得 GOP 与最终 memory map，`ExitBootServices()` 后跳入内核。

## 构建

```sh
make
make run
make clean
```

只保留调试构建，产物固定在 `build/debug/`。内核 `printf` 通过
QEMU debugcon `0xE9` 直接输出宿主终端。

## PMM

初始化第一步统计最终归内核所有的 RAM：

```text
LoaderCode
LoaderData
BootServicesCode
BootServicesData
Conventional
```

这些类型共同参与 pool 容量和 4K/2M 配额计算。当前真正填入 pool 的仍只有
`MEM_CONVENTIONAL`，因为内核还在使用 UEFI 留下的 stack/page tables，
并且 BOOT_INFO、memory-map、kernel image 仍可能位于 Loader 内存。
等建立自己的 stack/page tables 后再回收 Loader/BootServices。

目标物理容量比例：

```text
PTE / 4K = 1/8
PDE / 2M = 7/8
```

先统计每个 usable descriptor 的天然 4K 边缘和 2M 对齐块。天然 4K 边缘
优先计入 PTE 配额；如果还不足总 usable RAM 的 1/8，就把若干完整 2M block
规划为 512 个 4K PTE frames。PTE 比例按完整 2M block 调整，因此最多只有
不到 2 MiB 的取整误差。

metadata 仍只计算一次，然后由 bootstrap allocator 从 ConventionalMemory
切出；不做自举收敛循环。

global PTE/PDE pool 都是动态长度的三层 64 叉；per-CPU PTE cache 是两层
64 叉。frame 是 opaque 64-bit 值，没有 allocated bitmap，也不做 frame→slot 反查。

## 调试

VS Code 直接按 F5；或：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```
