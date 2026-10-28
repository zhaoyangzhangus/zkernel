#include "libc/libc.h"

#include <sys/mman.h>
#include <unistd.h>

#include <uapi/linux_abi.h>

#define LIBC_PAGE_SIZE 4096U
#define LIBC_ALLOC_MAGIC 0x4C4954454F53414CULL
#define LIBC_DEFAULT_ALIGNMENT 16U

typedef struct libc_allocation {
    uint64_t magic;
    uint64_t mapping;
    uint64_t mapping_size;
    uint64_t requested_size;
} libc_allocation_t;

static bool libc_linux_personality(void) {
    return __libc_linux_personality();
}

static bool is_power_of_two(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static size_t page_align(size_t size) {
    size_t remainder;
    if (size == 0U || size > SIZE_MAX - (LIBC_PAGE_SIZE - 1U)) return 0U;
    remainder = size & (LIBC_PAGE_SIZE - 1U);
    return remainder == 0U ? size : size + LIBC_PAGE_SIZE - remainder;
}

static void *allocate_aligned(size_t size, size_t alignment) {
    os_vm_map_args_t request = {0};
    libc_allocation_t *allocation;
    uintptr_t base;
    uintptr_t user_address;
    size_t overhead;
    size_t total;
    int64_t status;
    bool linux_personality;

    if (size == 0U) size = 1U;
    if (alignment < LIBC_DEFAULT_ALIGNMENT) alignment = LIBC_DEFAULT_ALIGNMENT;
    if (!is_power_of_two(alignment) || alignment > (size_t)UINTPTR_MAX ||
        size > SIZE_MAX - sizeof(*allocation) ||
        size + sizeof(*allocation) > SIZE_MAX - (alignment - 1U)) {
        errno = EINVAL;
        return 0;
    }
    overhead = sizeof(*allocation) + alignment - 1U;
    total = page_align(size + overhead);
    if (total == 0U) {
        errno = EOVERFLOW;
        return 0;
    }
    linux_personality = libc_linux_personality();
    if (linux_personality) {
        void *mapping = mmap(0, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) return 0;
        request.address = (uint64_t)(uintptr_t)mapping;
    } else {
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.length = total;
    request.prot = OS_VM_READ | OS_VM_WRITE;
    request.flags = OS_VM_PRIVATE;
    status = __libc_native_syscall6(OS_SYS_VM_MAP,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 || request.address == 0U) {
        if (status < 0) errno = (int)(-status);
        else errno = ENOMEM;
        return 0;
    }
    }
    base = (uintptr_t)request.address;
    if (base > UINTPTR_MAX - sizeof(*allocation) - (alignment - 1U)) {
        if (linux_personality) {
            (void)munmap((void *)(uintptr_t)request.address, total);
        } else {
        (void)__libc_native_syscall6(OS_SYS_VM_UNMAP, request.address, total,
                              0U, 0U, 0U, 0U);
        }
        errno = EOVERFLOW;
        return 0;
    }
    user_address = (base + sizeof(*allocation) + alignment - 1U) &
                   ~((uintptr_t)alignment - 1U);
    allocation = (libc_allocation_t *)(user_address - sizeof(*allocation));
    allocation->magic = LIBC_ALLOC_MAGIC;
    allocation->mapping = request.address;
    allocation->mapping_size = total;
    allocation->requested_size = size;
    return (void *)user_address;
}

void *malloc(size_t size) {
    return allocate_aligned(size, LIBC_DEFAULT_ALIGNMENT);
}

void *calloc(size_t count, size_t size) {
    size_t total;
    void *pointer;
    if (count != 0U && size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return 0;
    }
    total = count * size;
    pointer = malloc(total);
    if (pointer != 0) memset(pointer, 0, total == 0U ? 1U : total);
    return pointer;
}

void *reallocarray(void *pointer, size_t count, size_t size) {
    if (count != 0U && size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return 0;
    }
    return realloc(pointer, count * size);
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (!is_power_of_two(alignment) || alignment % sizeof(void *) != 0U ||
        size % alignment != 0U) {
        errno = EINVAL;
        return 0;
    }
    return allocate_aligned(size, alignment);
}

int posix_memalign(void **pointer, size_t alignment, size_t size) {
    void *result;
    if (pointer == 0) return EINVAL;
    if (!is_power_of_two(alignment) || alignment % sizeof(void *) != 0U) {
        return EINVAL;
    }
    result = allocate_aligned(size, alignment);
    if (result == 0) return errno != 0 ? errno : ENOMEM;
    *pointer = result;
    return 0;
}

void *memalign(size_t alignment, size_t size) {
    if (!is_power_of_two(alignment)) {
        errno = EINVAL;
        return 0;
    }
    return allocate_aligned(size, alignment);
}

void *valloc(size_t size) {
    return allocate_aligned(size, LIBC_PAGE_SIZE);
}

static libc_allocation_t *allocation_from_pointer(const void *pointer) {
    libc_allocation_t *allocation;
    if (pointer == 0) return 0;
    allocation = (libc_allocation_t *)((uintptr_t)pointer - sizeof(*allocation));
    if (allocation->magic != LIBC_ALLOC_MAGIC || allocation->mapping_size == 0U) {
        return 0;
    }
    return allocation;
}

size_t malloc_usable_size(void *pointer) {
    libc_allocation_t *allocation = allocation_from_pointer(pointer);
    return allocation == 0 ? 0U : (size_t)allocation->requested_size;
}

static void libc_free_impl(void *pointer) {
    libc_allocation_t *allocation = allocation_from_pointer(pointer);
    if (allocation == 0) return;
    allocation->magic = 0U;
    if (libc_linux_personality()) {
        if (munmap((void *)(uintptr_t)allocation->mapping,
                   (size_t)allocation->mapping_size) < 0) {
            errno = EIO;
        }
    } else if (__libc_native_syscall6(OS_SYS_VM_UNMAP, allocation->mapping,
                        allocation->mapping_size, 0U, 0U, 0U, 0U) < 0) {
        errno = EIO;
    }
}

void free(void *pointer) {
    libc_free_impl(pointer);
}

/*
 * Chromium 会导出自己的 malloc/free 垫片。此共享 libc 内部的调用通过 -Bsymbolic
 * 绑定到本地实现，但此处分配的指针仍可能跨越 ABI 边界并由 Chromium 释放。
 * 为该边界保留私有入口，避免两个分配器通过符号插入调用对方的实现。
 */
void __system_free(void *pointer) {
    libc_free_impl(pointer);
}

size_t __system_malloc_usable_size(void *pointer) {
    return malloc_usable_size(pointer);
}

void *realloc(void *pointer, size_t size) {
    libc_allocation_t *allocation;
    size_t previous_size;
    void *replacement;

    if (pointer == 0) return malloc(size);
    if (size == 0U) {
        free(pointer);
        return 0;
    }
    allocation = allocation_from_pointer(pointer);
    if (allocation == 0) {
        errno = EINVAL;
        return 0;
    }
    previous_size = (size_t)allocation->requested_size;
    if (size <= previous_size) {
        allocation->requested_size = size;
        return pointer;
    }
    replacement = malloc(size);
    if (replacement == 0) return 0;
    memcpy(replacement, pointer, previous_size);
    libc_free_impl(pointer);
    return replacement;
}

void *__system_realloc(void *pointer, size_t size) {
    return realloc(pointer, size);
}
