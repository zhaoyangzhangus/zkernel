# zkernel

最小 x86_64 UEFI 内核实验项目。UEFI loader 读取 `kernel.elf`，按 PT_LOAD
装入物理内存，取得 GOP 与最终 memory map，`ExitBootServices()` 后跳入内核。

## 构建

```sh
make
make run
make clean
```

只保留调试构建，产物固定在 `build/debug/`。内核 `printf` 直接通过
QEMU debugcon `0xE9` 输出到启动 QEMU 的终端。

## PMM

PMM 只接受 UEFI `MEM_CONVENTIONAL`。

运行期不维护 allocated bitmap，也不做 frame→slot 反查。frame 是 opaque
64-bit 资源值；分配后只由使用者持有，free 时写回任意空 slot。

- global PTE pool：三层 64 叉，`64^3 = 262144` slots
- global PDE pool：三层 64 叉，`64^3 = 262144` slots
- per-CPU PTE pool：两层 64 叉，`64^2 = 4096` slots
- CPU PTE pool 与 global PTE pool 每次 refill/drain 64 frames
- bitmap：`1 = slot/subtree 有可分配 frame`，`0 = 空/耗尽`

初始化阶段把 Conventional range 的 2 MiB 对齐完整块放入 PDE pool，
两端 4 KiB 页放入 PTE pool。运行期 allocator 不再解析物理地址。

## 调试

VS Code 直接按 F5；或：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```
