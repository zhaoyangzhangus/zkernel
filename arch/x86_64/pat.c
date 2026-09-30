#include "pat.h"

#include <stdint.h>

#define IA32_PAT_MSR UINT32_C(0x277)
#define CPUID_1_EDX_PAT (UINT32_C(1) << 16)

#define CR0_NW (UINT64_C(1) << 29)
#define CR0_CD (UINT64_C(1) << 30)
#define RFLAGS_IF (UINT64_C(1) << 9)

#define PTE_PWT       UINT64_C(0x008)
#define PTE_PCD       UINT64_C(0x010)
#define PTE_PAT_4K    UINT64_C(0x080)
#define PTE_PAT_LARGE UINT64_C(0x1000)

#define PAT_TYPE_UC       UINT64_C(0x00)
#define PAT_TYPE_WC       UINT64_C(0x01)
#define PAT_TYPE_WT       UINT64_C(0x04)
#define PAT_TYPE_WP       UINT64_C(0x05)
#define PAT_TYPE_WB       UINT64_C(0x06)
#define PAT_TYPE_UC_MINUS UINT64_C(0x07)

/*
 * 统一 PAT 布局：
 *   0 WB
 *   1 WC
 *   2 UC-
 *   3 UC
 *   4 WT
 *   5 WP
 *   6 WB
 *   7 UC
 *
 * IA32_PAT 是 per-CPU MSR，AP bring-up 时每个 CPU 都要调用 pat_init_cpu()。
 */
#define PAT_LAYOUT (     (PAT_TYPE_WB       <<  0) |     (PAT_TYPE_WC       <<  8) |     (PAT_TYPE_UC_MINUS << 16) |     (PAT_TYPE_UC       << 24) |     (PAT_TYPE_WT       << 32) |     (PAT_TYPE_WP       << 40) |     (PAT_TYPE_WB       << 48) |     (PAT_TYPE_UC       << 56))

static bool cpu_has_pat(void)
{
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx = 0;
    uint32_t edx;

    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));

    return (edx & CPUID_1_EDX_PAT) != 0;
}

static uint64_t read_cr0(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr0" :: "r"(value) : "memory");
}

static uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static void write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
}

static uint64_t read_rflags(void)
{
    uint64_t value;
    __asm__ volatile("pushfq; popq %0" : "=r"(value));
    return value;
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);

    __asm__ volatile("wrmsr"
                     :: "c"(msr), "a"(lo), "d"(hi)
                     : "memory");
}

uint64_t pat_read(void)
{
    return rdmsr(IA32_PAT_MSR);
}

bool pat_init_cpu(void)
{
    if (!cpu_has_pat())
        return false;

    if (pat_read() == PAT_LAYOUT)
        return true;

    uint64_t rflags = read_rflags();
    uint64_t cr0 = read_cr0();
    uint64_t cr3 = read_cr3();

    __asm__ volatile("cli" ::: "memory");
    __asm__ volatile("wbinvd" ::: "memory");

    write_cr0((cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile("wbinvd" ::: "memory");

    wrmsr(IA32_PAT_MSR, PAT_LAYOUT);

    __asm__ volatile("wbinvd" ::: "memory");
    write_cr0(cr0);

    /* 当前内核还没有 GLOBAL mapping，重载 CR3 足够刷新当前 TLB。 */
    write_cr3(cr3);

    if ((rflags & RFLAGS_IF) != 0)
        __asm__ volatile("sti" ::: "memory");

    return pat_read() == PAT_LAYOUT;
}

bool pat_page_flags(pat_memory_type_t type, bool large_page,
                    uint64_t *out_flags)
{
    if (out_flags == 0)
        return false;

    unsigned index;

    switch (type) {
    case PAT_MEMORY_WB:
        index = 0;
        break;
    case PAT_MEMORY_WC:
        index = 1;
        break;
    case PAT_MEMORY_UC_MINUS:
        index = 2;
        break;
    case PAT_MEMORY_UC:
        index = 3;
        break;
    case PAT_MEMORY_WT:
        index = 4;
        break;
    case PAT_MEMORY_WP:
        index = 5;
        break;
    default:
        return false;
    }

    uint64_t flags = 0;

    if ((index & 1U) != 0)
        flags |= PTE_PWT;
    if ((index & 2U) != 0)
        flags |= PTE_PCD;
    if ((index & 4U) != 0)
        flags |= large_page ? PTE_PAT_LARGE : PTE_PAT_4K;

    *out_flags = flags;
    return true;
}
