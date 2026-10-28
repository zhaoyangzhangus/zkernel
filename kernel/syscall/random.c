/* System 内核随机流：输入为 UEFI 熵，输出为 ChaCha20 数据块。 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <uapi/syscall.h>

#include "internal.h"

static atomic_flag g_random_lock = ATOMIC_FLAG_INIT;
static uint32_t g_random_state[16];
static bool g_random_ready;
static bool g_random_strong;

static uint32_t random_load32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static uint32_t random_rotate_left(uint32_t value, uint32_t shift) {
    return (value << shift) | (value >> (32U - shift));
}

static void random_quarter_round(uint32_t *state, uint32_t a, uint32_t b,
                                 uint32_t c, uint32_t d) {
    state[a] += state[b];
    state[d] = random_rotate_left(state[d] ^ state[a], 16U);
    state[c] += state[d];
    state[b] = random_rotate_left(state[b] ^ state[c], 12U);
    state[a] += state[b];
    state[d] = random_rotate_left(state[d] ^ state[a], 8U);
    state[c] += state[d];
    state[b] = random_rotate_left(state[b] ^ state[c], 7U);
}

static bool random_rdrand_supported(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1U), "c"(0U));
    (void)eax;
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0U;
}

static bool random_rdrand64(uint64_t *value) {
    uint8_t success;
    __asm__ volatile ("rdrand %0; setc %1"
                      : "=r"(*value), "=qm"(success) : : "cc");
    return success != 0U;
}

static bool random_hardware_seed(uint8_t seed[32]) {
    if (!random_rdrand_supported()) return false;
    for (uint32_t word = 0U; word < 4U; ++word) {
        uint64_t value = 0U;
        bool generated = false;
        for (uint32_t attempt = 0U; attempt < 16U; ++attempt) {
            if (random_rdrand64(&value)) {
                generated = true;
                break;
            }
        }
        if (!generated) return false;
        for (uint32_t byte = 0U; byte < sizeof(value); ++byte) {
            seed[word * sizeof(value) + byte] = (uint8_t)(value >> (byte * 8U));
        }
    }
    return true;
}

void random_init(const OS_BOOT_INFO *boot_info) {
    uint8_t seed[32] = {0};
    bool strong = false;
    uint64_t clock = x86_read_tsc();
    if (boot_info != 0 &&
        (boot_info->Flags & OS_BOOTINFO_HAS_RNG) != 0U) {
        for (uint32_t index = 0U; index < sizeof(seed); ++index) {
            seed[index] = boot_info->RandomSeed[index];
            strong |= seed[index] != 0U;
        }
    }
    if (!strong) strong = random_hardware_seed(seed);
    for (uint32_t index = 0U; index < sizeof(seed); ++index) {
        seed[index] ^= (uint8_t)(clock >> ((index & 7U) * 8U));
        clock = clock * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    g_random_state[0] = 0x61707865U;
    g_random_state[1] = 0x3320646EU;
    g_random_state[2] = 0x79622D32U;
    g_random_state[3] = 0x6B206574U;
    for (uint32_t index = 0U; index < 8U; ++index) {
        g_random_state[4U + index] = random_load32(seed + index * 4U);
    }
    g_random_state[12] = 1U;
    g_random_state[13] = random_load32(seed + 0U) ^ (uint32_t)clock;
    g_random_state[14] = random_load32(seed + 4U) ^ (uint32_t)(clock >> 32U);
    g_random_state[15] = random_load32(seed + 8U) ^ (uint32_t)x86_read_tsc();
    g_random_strong = strong;
    g_random_ready = true;
}

static void random_lock(void) {
    sched_preempt_disable();
    while (atomic_flag_test_and_set_explicit(&g_random_lock,
                                              memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
}

static void random_unlock(void) {
    atomic_flag_clear_explicit(&g_random_lock, memory_order_release);
    sched_preempt_enable();
}

static void random_clear_words(uint32_t words[16]) {
    volatile uint32_t *clear = words;
    for (uint32_t index = 0U; index < 16U; ++index) clear[index] = 0U;
}

static void random_block(uint8_t output[64]) {
    uint32_t state[16];
    uint32_t working[16];
    /* 在线程固定于此 CPU 时预留唯一的流数据块。ChaCha 轮计算和可能缺页的用户态复制都无需
     * 持有全局流锁。 */
    random_lock();
    for (uint32_t index = 0U; index < 16U; ++index) {
        state[index] = g_random_state[index];
    }
    ++g_random_state[12];
    if (g_random_state[12] == 0U) ++g_random_state[13];
    random_unlock();
    for (uint32_t index = 0U; index < 16U; ++index)
        working[index] = state[index];
    for (uint32_t round = 0U; round < 10U; ++round) {
        random_quarter_round(working, 0U, 4U, 8U, 12U);
        random_quarter_round(working, 1U, 5U, 9U, 13U);
        random_quarter_round(working, 2U, 6U, 10U, 14U);
        random_quarter_round(working, 3U, 7U, 11U, 15U);
        random_quarter_round(working, 0U, 5U, 10U, 15U);
        random_quarter_round(working, 1U, 6U, 11U, 12U);
        random_quarter_round(working, 2U, 7U, 8U, 13U);
        random_quarter_round(working, 3U, 4U, 9U, 14U);
    }
    for (uint32_t index = 0U; index < 16U; ++index) {
        uint32_t value = working[index] + state[index];
        output[index * 4U] = (uint8_t)value;
        output[index * 4U + 1U] = (uint8_t)(value >> 8U);
        output[index * 4U + 2U] = (uint8_t)(value >> 16U);
        output[index * 4U + 3U] = (uint8_t)(value >> 24U);
    }
    random_clear_words(state);
    random_clear_words(working);
}

int64_t syscall_random_get(uint64_t buffer, uint64_t length,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    uint64_t offset = 0U;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (length == 0U) return 0;
    if (buffer == 0U || length > (uint64_t)SIZE_MAX ||
        length > (uint64_t)INT64_MAX || buffer > UINT64_MAX - length ||
        !x86_user_range_valid((const void __user *)(uintptr_t)buffer,
                               (size_t)length)) {
        return K_EACCES;
    }
    if (!g_random_ready || !g_random_strong) return K_ENOSYS;
    while (offset < length) {
        uint8_t block[64];
        uint64_t remaining = length - offset;
        size_t count = remaining < sizeof(block) ? (size_t)remaining : sizeof(block);
        random_block(block);
        kstatus_t status = copy_to_user(
            (void __user *)(uintptr_t)(buffer + offset), block, count);
        if (status != K_OK) {
            return status;
        }
        offset += count;
    }
    return (int64_t)length;
}
