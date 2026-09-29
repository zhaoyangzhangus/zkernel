#ifndef __KERNEL_MM_VM_H__
#define __KERNEL_MM_VM_H__

#include <stdbool.h>
#include <stdint.h>

#include "pmm.h"

#define VM_PAGE_SIZE 0x1000ULL

typedef uint64_t vaddr_t;

struct vm_range;

typedef struct {
    vaddr_t base;
    vaddr_t end;
    uint64_t free_bytes;
    struct vm_range *free;
    pmm_cpu_t *cpu;
} vm_space_t;

bool vm_space_init(vm_space_t *space, pmm_cpu_t *cpu,
                   vaddr_t base, uint64_t size);
bool vm_alloc(vm_space_t *space, uint64_t size, uint64_t align,
              vaddr_t *out_addr);
bool vm_reserve(vm_space_t *space, vaddr_t addr, uint64_t size);
bool vm_free(vm_space_t *space, vaddr_t addr, uint64_t size);

#endif
