# zkernel

最小 x86_64 UEFI 内核实验项目。UEFI loader 读取 `kernel.elf`，按 PT_LOAD
装入物理内存，取得 GOP 与最终 memory map，`ExitBootServices()` 后跳入内核。

## 构建

只保留调试构建：

```sh
make
make run
make clean
```

产物固定在 `build/debug/`。QEMU 直接把这个目录作为 vvfat 启动盘。

## 终端输出

内核 `printf` 直接写 I/O port `0xE9`。QEMU 使用：

```text
-debugcon stdio -global isa-debugcon.iobase=0xe9
```

因此内核日志直接出现在启动 QEMU 的终端，不再初始化或维护 16550/COM1 串口驱动。

## 调试

VS Code 直接按 F5。手动调试：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```

`run-gdb` 使用 `-S` 暂停 CPU，入口使用硬件断点。

## 当前内核

- debugcon `printf` 终端输出
- 最终 UEFI memory map
- `pmm_alloc(size)`：仅从 `MEM_CONVENTIONAL` 分配，按 4 KiB 向上取整，不释放
- LiteOS 12x24 A8 ASCII framebuffer 字体

```text
boot/       UEFI loader 与共享定义
init/       kernel_main
mm/         早期物理内存
graphics/   A8 字体和 framebuffer 绘制
lib/        printf
tools/      QEMU/GDB 调试脚本
```
