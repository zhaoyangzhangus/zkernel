# zkernel

最小 x86_64 UEFI 内核实验项目。UEFI loader 读取 `kernel.elf`，按 PT_LOAD
装入物理内存，取得 GOP 与最终 memory map，`ExitBootServices()` 后跳入内核。

## 构建

项目只保留调试构建路径：

```sh
make        # build/debug/
make test   # host 单元测试
make run    # QEMU + OVMF
make clean
```

产物：

```text
build/debug/
├── EFI/BOOT/BOOTX64.EFI
├── kernel.elf
└── OVMF_VARS.fd
```

QEMU 直接把 `build/debug/` 作为 vvfat 启动盘，不生成 ESP 镜像。

## 调试

VS Code 直接按 F5。也可以手动：

```sh
make run-gdb
gdb build/debug/kernel.elf
(gdb) target remote :1234
```

`run-gdb` 使用 `-S`，CPU 在复位向量暂停。入口断点使用硬件断点，因为
GDB 连接时 `kernel.elf` 尚未由 loader 写入目标物理地址。

## 当前内核

- COM1 串口和 freestanding `printf`
- UEFI memory map
- `pmm_alloc(size)`：仅从 `MEM_CONVENTIONAL` 分配，size 按 4 KiB 向上取整，不释放
- LiteOS 移植的 12x24 A8 ASCII framebuffer 字体

目录：

```text
boot/       UEFI loader、ELF/UEFI/BOOT_INFO 定义
init/       kernel_main
mm/         早期物理内存
graphics/   12x24 A8 字体与 framebuffer 绘制
drivers/    串口
lib/        printf
tests/      host 测试
tools/      QEMU/GDB 辅助脚本
```
