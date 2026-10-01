#include "page_fault.h"

#include "paging.h"
#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#define PFEC_PRESENT UINT64_C(1)
#define PFEC_WRITE   UINT64_C(2)
#define PFEC_USER    UINT64_C(4)
#define PFEC_RSVD    UINT64_C(8)
#define PFEC_INSTR   UINT64_C(16)

static vm_space_t *g_fault_space;

static inline void debug_char(char c)
{
    __asm__ volatile("outb %0, $0xe9" :: "a"((uint8_t)c));
}

static void zero_frame(pmm_frame_t frame)
{
    uint64_t *p =
        (uint64_t *)(uintptr_t)(VM_DIRECT_MAP_BASE + frame);

    for (uint32_t i = 0; i < PMM_PAGE_4K / sizeof(uint64_t); ++i)
        p[i] = 0;
}

static bool anonymous_region(vm_region_type_t type)
{
    return type == VM_REGION_GENERIC ||
           type == VM_REGION_KERNEL ||
           type == VM_REGION_HEAP ||
           type == VM_REGION_STACK ||
           type == VM_REGION_USER;
}

void page_fault_bind_space(vm_space_t *space)
{
    g_fault_space = space;
}

bool page_fault_handle(uint64_t address, uint64_t error_code)
{
    if (g_fault_space == NULL)
        return false;

    /*
     * 第一版只修复 not-present fault。
     * protection fault / reserved-bit fault 说明已有映射或页表本身有错。
     */
    if ((error_code & (PFEC_PRESENT | PFEC_RSVD)) != 0)
        return false;

    vm_region_info_t region;
    if (!vm_query(g_fault_space, address, &region))
        return false;

    debug_char('Q');

    /*
     * VM region 本身就是“该 VA 合法”的声明。
     * PMM_OWNED 表示它的普通 RAM backing 由 PMM 按需提供；
     * 因此任何合法的 PMM-owned not-present page 都直接 demand-zero。
     */
    if ((region.attrs & VM_ATTR_PMM_OWNED) == 0 ||
        (region.attrs & VM_ATTR_GUARD) != 0 ||
        !anonymous_region(region.type))
        return false;

    if ((error_code & PFEC_WRITE) != 0) {
        if ((region.attrs & VM_ATTR_WRITE) == 0)
            return false;
    } else if ((error_code & PFEC_INSTR) != 0) {
        if ((region.attrs & VM_ATTR_EXEC) == 0)
            return false;
    } else if ((region.attrs & VM_ATTR_READ) == 0) {
        return false;
    }

    if ((error_code & PFEC_USER) != 0 &&
        (region.attrs & VM_ATTR_USER) == 0)
        return false;

    debug_char('A');

    /*
     * MMIO/WC/UC 不走这里；PMM-owned anonymous page 当前只接受普通 WB。
     */
    uint64_t cache = region.attrs & VM_ATTR_CACHE_MASK;
    if (cache != 0 && cache != VM_ATTR_CACHE_WB)
        return false;

    pmm_frame_t frame;


    /*
     * 优先尝试 2M huge page。
     */
    uint64_t huge_va =
        address & ~(PMM_PAGE_2M - 1);


    if (huge_va >= region.start &&
        huge_va + PMM_PAGE_2M <= region.start + region.size &&
        paging_can_map_2m_current(huge_va) &&
        pmm_alloc2m(&frame)) {


        zero_frame(frame);


        if (paging_map_2m_current(g_fault_space->cpu,
                                  huge_va,
                                  frame,
                                  region.attrs))
            return true;


        pmm_free2m(frame);
    }



    if (!pmm_alloc4k(g_fault_space->cpu,
                     &frame))
        return false;


    zero_frame(frame);


    vaddr_t page =
        address & ~(VM_PAGE_SIZE - 1);


    if (!paging_map_4k_current(g_fault_space->cpu,
                               page,
                               frame,
                               region.attrs)) {

        pmm_free4k(g_fault_space->cpu,
                   frame);

        return false;
    }

    debug_char('K');
    return true;
}
