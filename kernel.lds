OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_main)

/*
 * 内核链接脚本。
 *
 * 装载地址由 Makefile 的 KERNEL_LOAD_ADDR 通过 --defsym=_kernel_load_addr=...
 * 传入，写进每个段落落的地址里。引导程序不再关心这个常量：它按 ELF 程序头
 * 里的 p_paddr 装载，入口取自 ENTRY() 生成的 e_entry。
 *
 * 这里没有用 AT()，所以 p_paddr == p_vaddr；UEFI 的恒等映射页表让内核可以
 * 按物理地址直接执行。
 *
 * .bss 不需要额外处理：它落在可写段里，装载器会按 p_memsz 大于 p_filesz
 * 的那部分自动清零（扁平二进制就得内核自己清了）。
 */

/*
 * 显式定义两个 PT_LOAD 段（引导程序只认 PT_LOAD）：
 *   text —— R E，放代码和只读数据
 *   data —— RW，放 .data 和 .bss，bss 靠 p_memsz > p_filesz 表示
 * 不写 PHDRS 的话链接器会把所有节塞进一个 RWE 段，虽然能跑，
 * 但段权限就失去意义了。
 */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = DEFINED(_kernel_load_addr) ? _kernel_load_addr : 0x100000;

    .text : ALIGN(16) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } :text

    .data : ALIGN(16) {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)   /* 需要重定位的只读数据（如 const 指针数组） */
    } :data

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
