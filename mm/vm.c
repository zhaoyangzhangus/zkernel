#include "vm.h"

#include <stddef.h>
#include <stdint.h>

typedef struct vm_range {
    uint64_t start;
    uint64_t end;
    struct vm_range *next;
} vm_range_t;

static vm_range_t *g_free_nodes;

static bool canonical(uint64_t address)
{
    uint64_t top = address >> 48;
    return top == 0 || top == UINT64_C(0xFFFF);
}

static bool power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool page_round(uint64_t size, uint64_t *out)
{
    if (size == 0 || size > UINT64_MAX - (VM_PAGE_SIZE - 1))
        return false;

    *out = (size + VM_PAGE_SIZE - 1) & ~(VM_PAGE_SIZE - 1);
    return true;
}

static bool align_up(uint64_t value, uint64_t align, uint64_t *out)
{
    uint64_t mask = align - 1;

    if (value > UINT64_MAX - mask)
        return false;

    *out = (value + mask) & ~mask;
    return true;
}

static bool node_refill(pmm_cpu_t *cpu)
{
    pmm_frame_t frame;

    if (!pmm_alloc4k(cpu, &frame))
        return false;

    vm_range_t *nodes = (vm_range_t *)(uintptr_t)frame;
    uint32_t count = (uint32_t)(VM_PAGE_SIZE / sizeof(vm_range_t));

    for (uint32_t i = 0; i < count; ++i) {
        nodes[i].next = g_free_nodes;
        g_free_nodes = &nodes[i];
    }

    return true;
}

static vm_range_t *node_get(pmm_cpu_t *cpu)
{
    if (g_free_nodes == NULL && !node_refill(cpu))
        return NULL;

    vm_range_t *node = g_free_nodes;
    g_free_nodes = node->next;
    return node;
}

static void node_put(vm_range_t *node)
{
    node->next = g_free_nodes;
    g_free_nodes = node;
}

static bool normalize_range(const vm_space_t *space,
                            uint64_t addr, uint64_t size,
                            uint64_t *out_end)
{
    uint64_t bytes;

    if (space == NULL || !page_round(size, &bytes) ||
        (addr & (VM_PAGE_SIZE - 1)) != 0 ||
        addr < space->base ||
        addr > UINT64_MAX - bytes)
        return false;

    uint64_t end = addr + bytes;

    if (end > space->end)
        return false;

    *out_end = end;
    return true;
}

bool vm_space_init(vm_space_t *space, pmm_cpu_t *cpu,
                   vaddr_t base, uint64_t size)
{
    if (space == NULL || cpu == NULL ||
        size == 0 ||
        (base & (VM_PAGE_SIZE - 1)) != 0 ||
        (size & (VM_PAGE_SIZE - 1)) != 0 ||
        base > UINT64_MAX - size)
        return false;

    uint64_t end = base + size;

    if (!canonical(base) || !canonical(end - 1) ||
        ((base >> 47) & 1U) != (((end - 1) >> 47) & 1U))
        return false;

    vm_range_t *initial = node_get(cpu);
    if (initial == NULL)
        return false;

    initial->start = base;
    initial->end = end;
    initial->next = NULL;

    space->base = base;
    space->end = end;
    space->free_bytes = size;
    space->free = initial;
    space->cpu = cpu;
    return true;
}

static bool reserve_range(vm_space_t *space, uint64_t start, uint64_t end)
{
    vm_range_t *prev = NULL;
    vm_range_t *cur = space->free;

    while (cur != NULL && cur->end <= start) {
        prev = cur;
        cur = cur->next;
    }

    if (cur == NULL || start < cur->start || end > cur->end)
        return false;

    if (start == cur->start && end == cur->end) {
        if (prev != NULL)
            prev->next = cur->next;
        else
            space->free = cur->next;
        node_put(cur);
    } else if (start == cur->start) {
        cur->start = end;
    } else if (end == cur->end) {
        cur->end = start;
    } else {
        vm_range_t *right = node_get(space->cpu);
        if (right == NULL)
            return false;

        right->start = end;
        right->end = cur->end;
        right->next = cur->next;

        cur->end = start;
        cur->next = right;
    }

    space->free_bytes -= end - start;
    return true;
}

bool vm_reserve(vm_space_t *space, vaddr_t addr, uint64_t size)
{
    uint64_t end;

    if (!normalize_range(space, addr, size, &end))
        return false;

    return reserve_range(space, addr, end);
}

bool vm_alloc(vm_space_t *space, uint64_t size, uint64_t align,
              vaddr_t *out_addr)
{
    uint64_t bytes;

    if (space == NULL || out_addr == NULL || !page_round(size, &bytes))
        return false;

    if (align < VM_PAGE_SIZE)
        align = VM_PAGE_SIZE;

    if (!power_of_two(align) || (align & (VM_PAGE_SIZE - 1)) != 0)
        return false;

    for (vm_range_t *cur = space->free; cur != NULL; cur = cur->next) {
        uint64_t start;

        if (!align_up(cur->start, align, &start) ||
            start < cur->start ||
            start > UINT64_MAX - bytes)
            continue;

        uint64_t end = start + bytes;

        if (end > cur->end)
            continue;

        if (!reserve_range(space, start, end))
            return false;

        *out_addr = start;
        return true;
    }

    return false;
}

bool vm_free(vm_space_t *space, vaddr_t addr, uint64_t size)
{
    uint64_t end;

    if (!normalize_range(space, addr, size, &end))
        return false;

    vm_range_t *prev = NULL;
    vm_range_t *cur = space->free;

    while (cur != NULL && cur->start < addr) {
        prev = cur;
        cur = cur->next;
    }

    if ((prev != NULL && addr < prev->end) ||
        (cur != NULL && end > cur->start))
        return false;

    bool merge_prev = prev != NULL && prev->end == addr;
    bool merge_next = cur != NULL && end == cur->start;

    if (merge_prev && merge_next) {
        prev->end = cur->end;
        prev->next = cur->next;
        node_put(cur);
    } else if (merge_prev) {
        prev->end = end;
    } else if (merge_next) {
        cur->start = addr;
    } else {
        vm_range_t *node = node_get(space->cpu);
        if (node == NULL)
            return false;

        node->start = addr;
        node->end = end;
        node->next = cur;

        if (prev != NULL)
            prev->next = node;
        else
            space->free = node;
    }

    space->free_bytes += end - addr;
    return true;
}
