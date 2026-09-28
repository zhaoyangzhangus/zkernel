/*
 * serial.h —— 16550 兼容串口驱动（COM1）
 *
 * 内核最早的调试输出通道：QEMU 加 -serial stdio 就能在终端看到。
 * lib/printf.c 默认就往这里输出。
 */
#ifndef __KERNEL_DRIVERS_SERIAL_H__
#define __KERNEL_DRIVERS_SERIAL_H__

#define SERIAL_COM1 0x3F8

/* 初始化 COM1：115200 8N1，打开 FIFO。可以重复调用 */
void serial_init(void);

/* 输出一个字符。遇到 '\n' 会自动补一个 '\r'，适配终端显示 */
void serial_putc(char c);

/* 输出字符串，不做任何换行处理 */
void serial_write(const char *str);

#endif /* __KERNEL_DRIVERS_SERIAL_H__ */
