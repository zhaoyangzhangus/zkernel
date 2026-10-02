OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_main)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /*
     * 内核物理装载地址与虚拟链接地址彻底分离：
     *   LMA/p_paddr = 2 MiB（UEFI loader 实际拷贝的位置）
     *   VMA/p_vaddr = 0xFFFFFFFF80000000（内核执行地址）
     *
     * 两者保持固定 delta，所以每个 PT_LOAD 都能由 loader 直接按
     * p_paddr 装载，而 e_entry/符号地址保持在高半区。
     */
    __kernel_phys_base = DEFINED(_kernel_load_addr)
                       ? _kernel_load_addr : 0x200000;
    __kernel_virt_base = DEFINED(_kernel_virt_addr)
                       ? _kernel_virt_addr : 0xFFFFFFFF80000000;
    __kernel_delta = __kernel_virt_base - __kernel_phys_base;

    . = __kernel_virt_base;

    .text : AT(ADDR(.text) - __kernel_delta) {
        *(.text .text.*)
    } :text

    . = ALIGN(16);
    .rodata : AT(ADDR(.rodata) - __kernel_delta) {
        *(.rodata .rodata.*)
    } :text

    /* RW segment 从独立 4K 页开始，后续收紧页权限时不会和 RX 共页。 */
    . = ALIGN(0x1000);
    .data : AT(ADDR(.data) - __kernel_delta) {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :data

    . = ALIGN(16);
    .bss : AT(ADDR(.bss) - __kernel_delta) {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    __kernel_virt_end = .;
    __kernel_phys_end = __kernel_virt_end - __kernel_delta;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
