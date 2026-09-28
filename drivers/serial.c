/*
 * serial.c —— 16550 兼容串口驱动（COM1）
 *
 * 只用到轮询（polling）方式收发，不需要中断，够开机阶段用了。
 */
#include "serial.h"

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void)
{
    outb(SERIAL_COM1 + 1, 0x00);   /* 关中断 */
    outb(SERIAL_COM1 + 3, 0x80);   /* 打开 DLAB 以设置波特率 */
    outb(SERIAL_COM1 + 0, 0x03);   /* 分频低字节：115200 bps */
    outb(SERIAL_COM1 + 1, 0x00);   /* 分频高字节 */
    outb(SERIAL_COM1 + 3, 0x03);   /* 8 位数据、无校验、1 位停止位，关 DLAB */
    outb(SERIAL_COM1 + 2, 0xC7);   /* 打开 FIFO，清空，14 字节触发 */
    outb(SERIAL_COM1 + 4, 0x0B);   /* DTR / RTS / OUT2 */
}

void serial_putc(char c)
{
    /* 位 5（LSR.THRE）为 1 表示发送保持寄存器已空 */
    while ((inb(SERIAL_COM1 + 5) & 0x20) == 0)
        ;

    if (c == '\n')
        outb(SERIAL_COM1, '\r');
    outb(SERIAL_COM1, (uint8_t)c);
}

void serial_write(const char *str)
{
    while (*str != '\0')
        serial_putc(*str++);
}
