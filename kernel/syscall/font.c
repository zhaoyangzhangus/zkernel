/* REFACTOR_SYSCALL_FONT_OWNER：运行时 TrueType 缓存导出。 */

#include <arch/x86_64/uaccess.h>
#include <ascii_font.h>
#include <kernel/kmem.h>
#include <uapi/font.h>

#include "internal.h"

int64_t syscall_font_cache(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_font_cache_request_t arguments;
    uint64_t bytes_u64;
    uint8_t *cache;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || arguments_pointer == 0U) return K_EPERM;

    status = copy_from_user(
        &arguments,
        (const void __user *)(uintptr_t)arguments_pointer,
        sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.width == 0U || arguments.height == 0U ||
        arguments.width > OS_FONT_MAX_WIDTH ||
        arguments.height > OS_FONT_MAX_HEIGHT ||
        arguments.stride != 0U || arguments.glyph_count != 0U ||
        arguments.bytes_written != 0U || arguments.pixels == 0U) {
        return K_EINVAL;
    }

    bytes_u64 = (uint64_t)arguments.width * arguments.height *
        OS_FONT_GLYPH_COUNT;
    if (bytes_u64 == 0U || bytes_u64 > SIZE_MAX ||
        arguments.capacity < bytes_u64 ||
        !x86_user_range_valid(
            (void __user *)(uintptr_t)arguments.pixels,
            (size_t)bytes_u64)) {
        return K_EINVAL;
    }

    cache = (uint8_t *)kmalloc((size_t)bytes_u64, 0U);
    if (cache == 0) return K_ENOMEM;
    if (!ascii_font_copy_cache(arguments.width, arguments.height,
                               cache, (size_t)bytes_u64)) {
        kfree(cache);
        return K_EIO;
    }
    status = copy_to_user(
        (void __user *)(uintptr_t)arguments.pixels,
        cache, (size_t)bytes_u64);
    kfree(cache);
    if (status != K_OK) return status;

    arguments.stride = arguments.width;
    arguments.glyph_count = OS_FONT_GLYPH_COUNT;
    arguments.bytes_written = bytes_u64;
    return copy_to_user(
        (void __user *)(uintptr_t)arguments_pointer,
        &arguments, sizeof(arguments));
}
