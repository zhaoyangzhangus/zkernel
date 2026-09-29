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
QEMU debugcon `0xE9` 直接输出到宿主终端。

## PMM

PMM 只使用 UEFI `MEM_CONVENTIONAL`。

初始化采用一次计算：

1. 扫描所有 ConventionalMemory。
2. 计算其中能形成的 2 MiB PDE frame 和边缘 4 KiB PTE frame 数。
3. 用这些数量直接计算 PTE/PDE pool 的 slot、leaf、middle 和 metadata 大小。
4. PTE capacity 额外预留最多 511 个 slot，用来覆盖 bootstrap allocation
   破坏一个 2 MiB 对齐边界后可能新产生的 4 KiB 前缀。
5. bootstrap allocator 从一个 Conventional descriptor 低地址端切出 metadata。
6. 再扫描已经收缩后的 memory map，把实际剩余 frame 填入 pools。

不再做 metadata 大小的迭代/收敛计算。capacity 是上限，最终没被实际 frame
占用的尾部 slot 保持空即可。

global PTE/PDE pool 都是动态长度的三层 64 叉：

```text
slot_count   = 实际需要的 capacity
leaf_count   = ceil(slot_count / 64)
middle_count = ceil(leaf_count / 64)
root         = 1 x uint64_t
```

单 pool 最大仍为 `64^3 = 262144` slots。

per-CPU PTE cache 是两层 64 叉，容量最多 4096 slots；当前只有 bootstrap CPU。
frame 是 opaque 64-bit 值，不保存 allocated bitmap，也不做 frame→slot 反查。

## 调试

VS Code 直接按 F5；或：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```
