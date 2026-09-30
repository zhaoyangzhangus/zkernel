#ifndef __KERNEL_ARCH_X86_64_PAT_H__
#define __KERNEL_ARCH_X86_64_PAT_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAT_MEMORY_WB = 0,
    PAT_MEMORY_WC,
    PAT_MEMORY_UC_MINUS,
    PAT_MEMORY_UC,
    PAT_MEMORY_WT,
    PAT_MEMORY_WP
} pat_memory_type_t;

bool pat_init_cpu(void);
uint64_t pat_read(void);

/*
 * large_page=false: 4K PTE，PAT 位在 bit 7。
 * large_page=true : 2M/1G leaf，PAT 位在 bit 12。
 */
bool pat_page_flags(pat_memory_type_t type, bool large_page,
                    uint64_t *out_flags);

#endif
