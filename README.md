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

初始化不再固定预留 64³ 个 slot。流程：

1. 扫描实际 ConventionalMemory。
2. 模拟从 memory map 中预留 PMM metadata。
3. 按预留后的布局计算 PTE/PDE frame 数。
4. 由 frame 数动态计算 leaf/middle/slot 数量和 metadata 大小。
5. 如果 metadata 页数还不够，扩大模拟预留并重新计算。
6. 用最小 bootstrap allocator 真正从 `MEM_CONVENTIONAL` 切出 metadata。
7. 再扫描已经收缩后的 memory map，填充 PTE/PDE pools。

global PTE/PDE pool 都保持三层 64 叉，但只分配实际需要的数组：

```text
slot_count   = 实际 frame 数
leaf_count   = ceil(slot_count / 64)
middle_count = ceil(leaf_count / 64)
root         = 1 x uint64_t
```

单个三层 pool 最大仍是 `64^3 = 262144` slots。

per-CPU PTE cache 同样动态：

```text
capacity   = min(实际 PTE frame 数, 4096)
leaf_count = ceil(capacity / 64)
```

frame 仍是 opaque 64-bit 值；没有 allocated bitmap、没有 frame→slot 反查，
free 归还任意空 slot。PTE local/global refill/drain 每批最多 64 frames。

## 调试

VS Code 直接按 F5；或：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```
