"""原生批量枚举：执行真实处理程序，检查边界、错误传播及用户输出范围。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/filesystem.c').read_text()
handler = source[source.index('int64_t syscall_file_enumerate_batch('):
                 source.index('int64_t syscall_file_seek(')]
assert '[OS_SYS_FILE_ENUMERATE_BATCH] = syscall_file_enumerate_batch' in (
    root / 'kernel/syscall/handlers.c').read_text()
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/base.h>
#include <uapi/file.h>
#include <uapi/syscall.h>
static bool process_present = true, range_valid = true;
static int from_error, copy_error_at, copies, backend_calls;
static kstatus_t backend_status;
static uint32_t backend_count = 2, expected_index;
static size_t output_bytes;
static const char path[] = "/sbin";
static os_file_info_t output[OS_FILE_ENUMERATE_BATCH_MAX];
static void *current_process(void) { return process_present ? output : 0; }
static bool versioned_header_valid(const os_versioned_header_t *h, size_t n) {
    return h->size == n && h->version == OS_SYSCALL_ABI_VERSION && !h->flags;
}
static kstatus_t copy_from_user(void *d, const void *s, size_t n) {
    if (from_error) return from_error;
    memcpy(d, s, n); return K_OK;
}
static bool x86_user_range_valid(const void *p, size_t n) {
    assert(p == output); assert(n && n <= sizeof(output)); return range_valid;
}
static kstatus_t copy_to_user(void *d, const void *s, size_t n) {
    ++copies;
    if (copies == copy_error_at) return K_EFAULT;
    if (d == output) output_bytes = n;
    memcpy(d, s, n); return K_OK;
}
static kstatus_t vfs_enumerate_batch(const char *p, uint32_t i,
                                    os_file_info_t *out, uint32_t cap, uint32_t *count) {
    ++backend_calls;
    assert(p == path && i == expected_index && cap <= OS_FILE_ENUMERATE_BATCH_MAX);
    if (backend_status) return backend_status;
    *count = backend_count;
    for (uint32_t j = 0; j < backend_count && j < cap; ++j) {
        memset(out + j, 0, sizeof(*out));
        out[j].type = OS_FILE_TYPE_REGULAR; out[j].size = i + j + 100;
        snprintf(out[j].name, sizeof(out[j].name), "entry-%u", i + j);
    }
    return K_OK;
}
''' + handler + r'''
static os_file_enumerate_batch_t reset(void) {
    process_present = range_valid = true;
    from_error = copy_error_at = copies = backend_calls = 0;
    backend_status = K_OK; backend_count = 2; expected_index = 17; output_bytes = 0;
    memset(output, 0xa5, sizeof(output));
    return (os_file_enumerate_batch_t){
        .hdr = {sizeof(os_file_enumerate_batch_t), OS_SYSCALL_ABI_VERSION, 0},
        .path = (uintptr_t)path, .entries = (uintptr_t)output,
        .index = expected_index, .capacity = OS_FILE_ENUMERATE_BATCH_MAX, .count = 0x12345678
    };
}
static int64_t invoke(os_file_enumerate_batch_t *r) {
    return syscall_file_enumerate_batch((uintptr_t)r, 1, 2, 3, 4, 5);
}
int main(void) {
    _Static_assert(sizeof(os_file_enumerate_batch_t) == 40, "ABI layout");
    _Static_assert(OS_SYS_FILE_ENUMERATE_BATCH == 0x030c, "ABI number");
    os_file_enumerate_batch_t r = reset();
    process_present = false; assert(invoke(&r) == K_EPERM && !backend_calls && !copies);
    r = reset(); from_error = K_EFAULT; assert(invoke(&r) == K_EFAULT && !backend_calls);
    for (unsigned bad = 0; bad < 8; ++bad) {
        r = reset();
        switch (bad) {
        case 0: r.hdr.size--; break;
        case 1: r.hdr.version++; break;
        case 2: r.hdr.flags = 1; break;
        case 3: r.path = 0; break;
        case 4: r.entries = 0; break;
        case 5: r.reserved = 1; break;
        case 6: r.capacity = 0; break;
        case 7: r.capacity = OS_FILE_ENUMERATE_BATCH_MAX + 1; break;
        }
        assert(invoke(&r) == K_EINVAL && !backend_calls && !copies);
    }
    r = reset(); range_valid = false;
    assert(invoke(&r) == K_EACCES && !backend_calls && !copies);
    const kstatus_t errors[] = {K_ENOENT, K_ENOMEM, K_ENOTDIR, K_EIO};
    for (unsigned i = 0; i < sizeof(errors)/sizeof(*errors); ++i) {
        r = reset(); backend_status = errors[i];
        assert(invoke(&r) == errors[i] && backend_calls == 1 && !copies);
        assert(r.count == 0x12345678);
    }
    for (unsigned bad = 0; bad < 2; ++bad) {
        r = reset(); backend_count = bad ? r.capacity + 1 : 0;
        assert(invoke(&r) == K_EIO && !copies);
    }
    for (uint32_t n = 1; n <= OS_FILE_ENUMERATE_BATCH_MAX; ++n) {
        r = reset(); backend_count = n;
        assert(invoke(&r) == K_OK && r.count == n && copies == 2 && backend_calls == 1);
        assert(output_bytes == n * sizeof(*output));
        for (uint32_t i = 0; i < n; ++i) {
            os_file_info_t expected = {.type = OS_FILE_TYPE_REGULAR, .size = 117 + i};
            snprintf(expected.name, sizeof(expected.name), "entry-%u", 17 + i);
            assert(!memcmp(output + i, &expected, sizeof(expected)));
        }
        const unsigned char *bytes = (const unsigned char *)output;
        for (size_t i = output_bytes; i < sizeof(output); ++i) assert(bytes[i] == 0xa5);
    }
    r = reset(); r.capacity = backend_count = 1;
    assert(invoke(&r) == K_OK && output_bytes == sizeof(*output));
    for (int at = 1; at <= 2; ++at) {
        r = reset(); copy_error_at = at;
        assert(invoke(&r) == K_EFAULT && copies == at && r.count == 0x12345678);
    }
    puts("Native batch enumeration: ABI, bounds, EOF, errors, partial/full output and untouched tail PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-native-batch-') as directory:
    source_path = Path(directory) / 'test.c'
    executable = Path(directory) / 'test'
    source_path.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-I', str(root / 'include'), str(source_path), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
