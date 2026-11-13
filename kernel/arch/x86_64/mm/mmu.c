#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/mm.h>

#include "mmu_internal.h"

#define IA32_PAT 0x00000277U

static uint64_t read_msr(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t index, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

static void x86_program_pat(void) {
    /* 对所有 MMU 调用方，PAT 索引 0/1/2/3 始终对应 WB/WC/UC-/UC。 */
    const uint64_t pat = 0x0007010600070106ULL;
    if (read_msr(IA32_PAT) != pat) {
        __asm__ volatile ("wbinvd" : : : "memory");
        write_msr(IA32_PAT, pat);
        __asm__ volatile ("wbinvd" : : : "memory");
    }
}

void x86_paging_arch_init(void) {
    x86_page_table_arch_init();
    x86_program_pat();
}

void x86_paging_arch_init_secondary(void) {
    /* PAT 是每个逻辑 CPU 独有的；AP 不会继承 BSP 的 MSR 值。 */
    x86_program_pat();
}

uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large_page) {
    (void)large_page;
    switch (mode) {
        case X86_CACHE_WB:
            return 0;
        case X86_CACHE_WC:
            return PTE_WRITE_THROUGH;
        case X86_CACHE_UC:
            return PTE_WRITE_THROUGH | PTE_CACHE_DISABLE;
        default:
            return PTE_WRITE_THROUGH | PTE_CACHE_DISABLE;
    }
}

bool x86_is_canonical(uint64_t address) {
    uint64_t upper = address >> X86_64_VA_BITS;
    return upper == 0U || upper == 0xFFFFU;
}

paddr_t x86_current_root_table(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return paddr_make(cr3 & PTE_ADDRESS_MASK);
}

uint64_t x86_cr3_value(paddr_t root, uint16_t pcid) {
    uint64_t value = root.value & PTE_ADDRESS_MASK;
    if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        value |= (uint64_t)(pcid & 0xFFFU);
    }
    return value;
}

void x86_activate_root_table_pcid(paddr_t root, uint16_t pcid) {
    uint64_t value = root.value & PTE_ADDRESS_MASK;
    if (value == 0U || x86_current_root_table().value == value) return;
    value = x86_cr3_value(root, pcid);
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

void x86_activate_root_table(paddr_t root) {
    x86_activate_root_table_pcid(root, 0U);
}
