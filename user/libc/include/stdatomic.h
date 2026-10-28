#pragma once

#include <stdint.h>
#include <stddef.h>

#define ATOMIC_BOOL_LOCK_FREE __GCC_ATOMIC_BOOL_LOCK_FREE
#define ATOMIC_CHAR_LOCK_FREE __GCC_ATOMIC_CHAR_LOCK_FREE
#define ATOMIC_CHAR16_T_LOCK_FREE __GCC_ATOMIC_CHAR16_T_LOCK_FREE
#define ATOMIC_CHAR32_T_LOCK_FREE __GCC_ATOMIC_CHAR32_T_LOCK_FREE
#define ATOMIC_WCHAR_T_LOCK_FREE __GCC_ATOMIC_WCHAR_T_LOCK_FREE
#define ATOMIC_SHORT_LOCK_FREE __GCC_ATOMIC_SHORT_LOCK_FREE
#define ATOMIC_INT_LOCK_FREE __GCC_ATOMIC_INT_LOCK_FREE
#define ATOMIC_LONG_LOCK_FREE __GCC_ATOMIC_LONG_LOCK_FREE
#define ATOMIC_LLONG_LOCK_FREE __GCC_ATOMIC_LLONG_LOCK_FREE
#define ATOMIC_POINTER_LOCK_FREE __GCC_ATOMIC_POINTER_LOCK_FREE

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

#define ATOMIC_VAR_INIT(value) (value)

/* Clang 的 _n 内建函数不接受指向 C11 _Atomic 限定对象的指针。使用通用内建函数，
 * 并保留 C11 表达式语义，使整数和指针原子操作都能工作。这些也是 GCC freestanding
 * stdatomic.h 使用的 GNU C 扩展。 */
#define atomic_init(object, value)                                              \
    __extension__                                                               \
    ({                                                                           \
        __typeof_unqual__(*(object)) __atomic_init_value = (value);              \
        __atomic_store((__typeof_unqual__(*(object)) *)(object),                 \
                       &__atomic_init_value, __ATOMIC_RELAXED);                 \
    })
#define atomic_is_lock_free(object) __atomic_is_lock_free(sizeof(*(object)), (object))
#define atomic_store_explicit(object, value, order)                              \
    __extension__                                                               \
    ({                                                                           \
        __auto_type __atomic_store_ptr = (object);                              \
        __typeof_unqual__(*__atomic_store_ptr) __atomic_store_value = (value);   \
        __atomic_store((__typeof_unqual__(*__atomic_store_ptr) *)                 \
                           __atomic_store_ptr,                                   \
                       &__atomic_store_value, (order));                          \
    })
#define atomic_load_explicit(object, order)                                      \
    __extension__                                                               \
    ({                                                                           \
        __auto_type __atomic_load_ptr = (object);                               \
        __typeof_unqual__(*__atomic_load_ptr) __atomic_load_value;               \
        __atomic_load((__typeof_unqual__(*__atomic_load_ptr) *)                  \
                          __atomic_load_ptr,                                     \
                      &__atomic_load_value, (order));                            \
        __atomic_load_value;                                                     \
    })
#define atomic_exchange_explicit(object, value, order)                           \
    __extension__                                                               \
    ({                                                                           \
        __auto_type __atomic_exchange_ptr = (object);                           \
        __typeof_unqual__(*__atomic_exchange_ptr) __atomic_exchange_value =      \
            (value);                                                             \
        __typeof_unqual__(*__atomic_exchange_ptr) __atomic_exchange_old;         \
        __atomic_exchange((__typeof_unqual__(*__atomic_exchange_ptr) *)          \
                              __atomic_exchange_ptr,                             \
                          &__atomic_exchange_value,                              \
                          &__atomic_exchange_old, (order));                     \
        __atomic_exchange_old;                                                   \
    })
#define atomic_compare_exchange_strong_explicit(object, expected, desired,      \
                                                success, failure)                \
    __extension__                                                               \
    ({                                                                           \
        __auto_type __atomic_compare_ptr = (object);                            \
        __typeof_unqual__(*__atomic_compare_ptr) __atomic_compare_desired =      \
            (desired);                                                           \
        __atomic_compare_exchange(                                               \
            (__typeof_unqual__(*__atomic_compare_ptr) *)__atomic_compare_ptr,     \
                                  (expected),                                     \
                                  &__atomic_compare_desired, 0, (success),      \
                                  (failure));                                    \
    })
#define atomic_compare_exchange_weak_explicit(object, expected, desired,        \
                                              success, failure)                  \
    __extension__                                                               \
    ({                                                                           \
        __auto_type __atomic_compare_ptr = (object);                            \
        __typeof_unqual__(*__atomic_compare_ptr) __atomic_compare_desired =      \
            (desired);                                                           \
        __atomic_compare_exchange(                                               \
            (__typeof_unqual__(*__atomic_compare_ptr) *)__atomic_compare_ptr,     \
                                  (expected),                                     \
                                  &__atomic_compare_desired, 1, (success),      \
                                  (failure));                                    \
    })
#define atomic_fetch_add_explicit(object, operand, order)                        \
    __atomic_fetch_add((__typeof_unqual__(*(object)) *)(object), (operand),     \
                       (order))
#define atomic_fetch_sub_explicit(object, operand, order)                        \
    __atomic_fetch_sub((__typeof_unqual__(*(object)) *)(object), (operand),     \
                       (order))
#define atomic_fetch_or_explicit(object, operand, order)                         \
    __atomic_fetch_or((__typeof_unqual__(*(object)) *)(object), (operand),      \
                      (order))
#define atomic_fetch_xor_explicit(object, operand, order)                        \
    __atomic_fetch_xor((__typeof_unqual__(*(object)) *)(object), (operand),     \
                       (order))
#define atomic_fetch_and_explicit(object, operand, order)                        \
    __atomic_fetch_and((__typeof_unqual__(*(object)) *)(object), (operand),     \
                       (order))

#define atomic_store(object, value) atomic_store_explicit((object), (value), memory_order_seq_cst)
#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_exchange(object, value) atomic_exchange_explicit((object), (value), memory_order_seq_cst)
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_fetch_add(object, operand) atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_sub(object, operand) atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or(object, operand) atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor(object, operand) atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and(object, operand) atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(size_t) atomic_size_t;
/* 即使在当前目标上解析为相同的底层整数类型，C11 的 least/fast-width 别名仍是不同的
 * typedef 名称。许多可移植 C 库（包括 pthreadpool）使用这些别名，而不是精确宽度类型。 */
typedef _Atomic(int8_t) atomic_int_least8_t;
typedef _Atomic(uint8_t) atomic_uint_least8_t;
typedef _Atomic(int16_t) atomic_int_least16_t;
typedef _Atomic(uint16_t) atomic_uint_least16_t;
typedef _Atomic(int32_t) atomic_int_least32_t;
typedef _Atomic(uint32_t) atomic_uint_least32_t;
typedef _Atomic(int64_t) atomic_int_least64_t;
typedef _Atomic(uint64_t) atomic_uint_least64_t;
typedef _Atomic(int8_t) atomic_int_fast8_t;
typedef _Atomic(uint8_t) atomic_uint_fast8_t;
typedef _Atomic(int16_t) atomic_int_fast16_t;
typedef _Atomic(uint16_t) atomic_uint_fast16_t;
typedef _Atomic(int32_t) atomic_int_fast32_t;
typedef _Atomic(uint32_t) atomic_uint_fast32_t;
typedef _Atomic(int64_t) atomic_int_fast64_t;
typedef _Atomic(uint64_t) atomic_uint_fast64_t;
typedef _Atomic(intmax_t) atomic_intmax_t;
typedef _Atomic(uintmax_t) atomic_uintmax_t;
typedef struct { atomic_bool value; } atomic_flag;

#define ATOMIC_FLAG_INIT { false }
#define atomic_flag_test_and_set_explicit(object, order) \
    atomic_exchange_explicit(&(object)->value, true, (order))
#define atomic_flag_clear_explicit(object, order) \
    atomic_store_explicit(&(object)->value, false, (order))
#define atomic_flag_test_and_set(object) \
    atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear(object) \
    atomic_flag_clear_explicit((object), memory_order_seq_cst)
