#include "nv_port.h"

void nv_port_compiler_barrier(void) {
    __asm__ volatile ("" : : : "memory");
}

void nv_port_mb(void) {
    atomic_thread_fence(memory_order_seq_cst);
    __asm__ volatile ("mfence" : : : "memory");
}

void nv_port_rmb(void) {
    atomic_thread_fence(memory_order_acquire);
    __asm__ volatile ("lfence" : : : "memory");
}

void nv_port_wmb(void) {
    atomic_thread_fence(memory_order_release);
    __asm__ volatile ("sfence" : : : "memory");
}

void nv_port_dma_rmb(void) {
    nv_port_rmb();
}

void nv_port_dma_wmb(void) {
    nv_port_wmb();
}

void nv_port_wc_drain(void) {
    __asm__ volatile ("sfence" : : : "memory");
}

static bool cache_range_valid(const void *address, size_t length,
                              uintptr_t *start, uintptr_t *end) {
    uintptr_t value;

    if (address == 0 || length == 0 || start == 0 || end == 0) return false;
    value = (uintptr_t)address;
    if (value > UINTPTR_MAX - (length - 1U)) return false;
    *start = value & ~(uintptr_t)(CACHELINE_SIZE - 1U);
    *end = (value + length - 1U) | (uintptr_t)(CACHELINE_SIZE - 1U);
    if (*end == UINTPTR_MAX) return false;
    ++*end;
    return true;
}

void nv_port_cache_flush(const void *address, size_t length) {
    uintptr_t start;
    uintptr_t end;

    if (!cache_range_valid(address, length, &start, &end)) return;
    for (uintptr_t cursor = start; cursor < end; cursor += CACHELINE_SIZE) {
        __asm__ volatile ("clflush (%0)" : : "r"((const void *)cursor)
                          : "memory");
    }
    nv_port_mb();
}

void nv_port_cache_invalidate(const void *address, size_t length) {
    /* x86 CLFLUSH 会执行回写并失效，且由 MFENCE 保证顺序。 */
    nv_port_cache_flush(address, length);
}

bool nv_port_cache_self_test(void) {
    static uint8_t buffer[CACHELINE_SIZE * 2U + 7U]
        __attribute__((aligned(CACHELINE_SIZE)));

    buffer[0] = 0xA5U;
    buffer[sizeof(buffer) - 1U] = 0x5AU;
    nv_port_compiler_barrier();
    nv_port_wmb();
    nv_port_dma_wmb();
    nv_port_cache_flush(buffer + 1U, sizeof(buffer) - 2U);
    nv_port_cache_invalidate(buffer, sizeof(buffer));
    nv_port_rmb();
    nv_port_dma_rmb();
    return buffer[0] == 0xA5U && buffer[sizeof(buffer) - 1U] == 0x5AU;
}
