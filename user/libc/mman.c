#include "libc/libc.h"

#include <limits.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include <uapi/linux_abi.h>
#include <uapi/memory.h>

#ifndef _SC_NPROCESSORS_CONF
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#endif
#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 85
#endif
#ifndef _SC_AVPHYS_PAGES
#define _SC_AVPHYS_PAGES 86
#endif

#define LIBC_PAGE_SIZE 4096U

static bool libc_linux_personality(void) {
    return __libc_linux_personality();
}

static long linux_processor_count(void) {
    int saved_errno = errno;
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo == 0) {
        errno = saved_errno;
        return 0L;
    }

    long count = 0L;
    char line[32];
    while (fgets(line, sizeof(line), cpuinfo) != 0) {
        if (strncmp(line, "processor", 9U) == 0) ++count;
    }
    (void)fclose(cpuinfo);
    errno = saved_errno;
    return count;
}

static int invalid_mapping(void) {
    errno = EINVAL;
    return -1;
}

static int round_mapping_length(size_t length, size_t *rounded) {
    size_t remainder;
    if (rounded == 0 || length == 0U ||
        length > SIZE_MAX - (LIBC_PAGE_SIZE - 1U)) {
        return invalid_mapping();
    }
    remainder = length & (LIBC_PAGE_SIZE - 1U);
    *rounded = remainder == 0U ? length :
               length + LIBC_PAGE_SIZE - remainder;
    return 0;
}

static int supported_madvise(int advice) {
    switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_DONTNEED:
    case MADV_DONTFORK:
    case MADV_DOFORK:
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
    case MADV_DONTDUMP:
    case MADV_DODUMP:
        return 1;
    default:
        return 0;
    }
}

static int translate_protection(int protection, uint32_t *result) {
    uint32_t translated = 0U;
    if (result == 0 ||
        (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return invalid_mapping();
    }
    if ((protection & PROT_READ) != 0) translated |= OS_VM_READ;
    if ((protection & PROT_WRITE) != 0) translated |= OS_VM_WRITE;
    if ((protection & PROT_EXEC) != 0) translated |= OS_VM_EXEC;
    *result = translated;
    return 0;
}

void *mmap(void *address, size_t length, int protection, int flags,
           int descriptor, off_t offset) {
    /* Linux 兼容模式下打开兼容路径（尤其是 /tmp 共享内存文件）会安装 Linux fd 表项。
     * 这些映射也应通过相同 ABI 处理，同时保留其余 POSIX 文件路径使用的原生 VFS 描述符。 */
    if (libc_linux_personality() &&
        (((flags & MAP_ANONYMOUS) != 0) ||
         __libc_descriptor_is_linux_fd(descriptor))) {
        int64_t status = __libc_linux_syscall6(
            LINUX_SYS_MMAP, (uint64_t)(uintptr_t)address, length,
            (uint64_t)(uint32_t)protection, (uint64_t)(uint32_t)flags,
            (uint64_t)(int64_t)descriptor, (uint64_t)(int64_t)offset);
        if (status < 0) {
            (void)__libc_status_result(status);
            return MAP_FAILED;
        }
        return (void *)(uintptr_t)status;
    }
    const int supported_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED |
                                MAP_ANONYMOUS | MAP_STACK;
    os_vm_map_args_t request = {0};
    size_t mapped_length;
    uint32_t translated_protection;
    os_handle_t object = OS_INVALID_HANDLE;
    int64_t status;

    if (round_mapping_length(length, &mapped_length) < 0 ||
        translate_protection(protection, &translated_protection) < 0 ||
        (flags & ~supported_flags) != 0 ||
        ((flags & (MAP_SHARED | MAP_PRIVATE)) ==
         (MAP_SHARED | MAP_PRIVATE)) ||
        (flags & (MAP_SHARED | MAP_PRIVATE)) == 0 ||
        offset < 0 || ((uint64_t)offset & (LIBC_PAGE_SIZE - 1U)) != 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_FIXED) != 0 &&
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_ANONYMOUS) == 0) {
        if (descriptor < 0) {
            errno = EBADF;
            return MAP_FAILED;
        }
        if (__libc_descriptor_is_socket(descriptor)) {
            errno = EBADF;
            return MAP_FAILED;
        }
        object = __libc_handle_for_descriptor(descriptor);
        if (object == OS_INVALID_HANDLE) return MAP_FAILED;
    } else if (offset != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.address = (uint64_t)(uintptr_t)address;
    request.length = mapped_length;
    request.offset = (uint64_t)offset;
    request.object = object;
    request.prot = translated_protection;
    request.flags = (flags & MAP_SHARED) != 0 ? OS_VM_SHARED : OS_VM_PRIVATE;
    if ((flags & MAP_FIXED) != 0) request.flags |= OS_VM_FIXED;
    if ((flags & MAP_STACK) != 0) request.flags |= OS_VM_STACK;
    status = __libc_native_syscall6(OS_SYS_VM_MAP,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 || request.address == 0U) {
        if (status < 0) (void)__libc_status_result(status);
        else errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)request.address;
}

int munmap(void *address, size_t length) {
    size_t mapped_length;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (libc_linux_personality()) {
        status = __libc_linux_syscall6(LINUX_SYS_MUNMAP,
                                 (uint64_t)(uintptr_t)address, mapped_length,
                                 0U, 0U, 0U, 0U);
        return __libc_status_result(status);
    }
    status = __libc_native_syscall6(OS_SYS_VM_UNMAP,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             0U, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int mprotect(void *address, size_t length, int protection) {
    size_t mapped_length;
    uint32_t translated_protection;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        translate_protection(protection, &translated_protection) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (libc_linux_personality()) {
        status = __libc_linux_syscall6(LINUX_SYS_MPROTECT,
                                 (uint64_t)(uintptr_t)address, mapped_length,
                                 (uint64_t)(uint32_t)protection, 0U, 0U, 0U);
        return __libc_status_result(status);
    }
    status = __libc_native_syscall6(OS_SYS_VM_PROTECT,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             translated_protection, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int msync(void *address, size_t length, int flags) {
    size_t mapped_length;
    int sync_mode = flags & (MS_ASYNC | MS_SYNC);
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
        (sync_mode == 0 || sync_mode == (MS_ASYNC | MS_SYNC))) {
        errno = EINVAL;
        return -1;
    }
    status = __libc_native_syscall6(OS_SYS_VM_SYNC,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             (uint64_t)(uint32_t)flags, 0U, 0U, 0U);
    return __libc_status_result(status);
}

int madvise(void *address, size_t length, int advice) {
    size_t mapped_length;
    int64_t status;
    if (address == MAP_FAILED ||
        ((uintptr_t)address & (LIBC_PAGE_SIZE - 1U)) != 0U ||
        round_mapping_length(length, &mapped_length) < 0 ||
        !supported_madvise(advice)) {
        errno = EINVAL;
        return -1;
    }
    if (libc_linux_personality()) {
        status = __libc_linux_syscall6(LINUX_SYS_MADVISE,
                                 (uint64_t)(uintptr_t)address, mapped_length,
                                 (uint64_t)(uint32_t)advice, 0U, 0U, 0U);
        return __libc_status_result(status);
    }
    status = __libc_native_syscall6(OS_SYS_VM_ADVISE,
                             (uint64_t)(uintptr_t)address, mapped_length,
                             (uint64_t)(uint32_t)advice, 0U, 0U, 0U);
    return __libc_status_result(status);
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return LIBC_PAGE_SIZE;
    case _SC_OPEN_MAX:
        return 256L;
    case _SC_CLK_TCK:
        return 100L;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        if (libc_linux_personality()) {
            long count = linux_processor_count();
            if (count > 0L) return count;
        }
        /* Native test images use the run-qemu.sh four-vCPU default. */
        return 4L;
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
        os_memory_info_t info;
        uint64_t pages;
        if (__libc_query_memory_info(&info) < 0) return -1L;
        if (info.page_size != LIBC_PAGE_SIZE ||
            info.free_pages > info.total_pages) {
            errno = EIO;
            return -1L;
        }
        pages = name == _SC_PHYS_PAGES ? info.total_pages : info.free_pages;
        if (pages > (uint64_t)LONG_MAX) {
            errno = EOVERFLOW;
            return -1L;
        }
        return (long)pages;
    }
    default:
        errno = EINVAL;
        return -1L;
    }
}

int getpagesize(void) {
    return LIBC_PAGE_SIZE;
}
