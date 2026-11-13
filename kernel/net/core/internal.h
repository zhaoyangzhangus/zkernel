#pragma once

#include <kernel/string.h>

/* REFACTOR_P8_NET_INTERNAL：共用的私有清零字节操作原语。 */
static inline void net_zero(void *destination, size_t length) {
    memset(destination, 0, length);
}

static inline bool net_mac_equal(const uint8_t left[6],
                                 const uint8_t right[6]) {
    for (uint32_t index = 0U; index < 6U; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

static inline bool net_address6_equal(const uint8_t left[16],
                                     const uint8_t right[16]) {
    uint8_t difference = 0U;
    for (uint32_t index = 0U; index < 16U; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}
