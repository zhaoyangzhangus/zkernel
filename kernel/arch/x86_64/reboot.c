#include <arch/x86_64/acpi.h>
#include <arch/x86_64/reboot.h>

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_delay(void) {
    port_out8(0x80U, 0U);
}

static void keyboard_controller_reset(void) {
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        if ((port_in8(0x64U) & 0x02U) == 0U) {
            port_out8(0x64U, 0xFEU);
            io_delay();
            return;
        }
    }
}

__attribute__((noreturn)) static void triple_fault(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty_idt = {0U, 0U};
    __asm__ volatile ("lidt %0; int3" : : "m"(empty_idt) : "memory");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((noreturn)) void x86_system_reboot(void) {
    __asm__ volatile ("cli" : : : "memory");
    if (x86_acpi_reset()) {
        for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
            __asm__ volatile ("pause");
        }
    }

    /* 常见 PC 固件和 QEMU 都提供 PCI 复位控制。 */
    port_out8(0xCF9U, 0x06U);
    io_delay();
    port_out8(0xCF9U, 0x0EU);
    io_delay();
    keyboard_controller_reset();
    triple_fault();
}

