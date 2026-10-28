/* Real Linux/glibc x86-64 ABI acceptance binary for Phase 7. */
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define P7_THREAD_ITERATIONS 10000U

static __thread uint64_t p7_tls_value;
static volatile int p7_thread_failure;

static void p7_emit(const char *text) {
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static int p7_fail(const char *text) {
    p7_emit(text);
    return 1;
}

static void *p7_thread_entry(void *opaque) {
    uint64_t expected = (uint64_t)(uintptr_t)opaque;
    p7_tls_value = expected ^ UINT64_C(0x5a5a5a5a5a5a5a5a);
    if (p7_tls_value != (expected ^ UINT64_C(0x5a5a5a5a5a5a5a5a))) {
        p7_thread_failure = 1;
    }
    return (void *)(uintptr_t)p7_tls_value;
}

int main(void) {
    static const char expected_data[] = "system-phase7-file-io\n";
    char file_data[sizeof(expected_data)];
    const size_t expected_data_size = sizeof(expected_data) - 1U;
    struct stat file_stat;
    pthread_t thread;
    void *thread_result = NULL;
    int descriptor;
    void *allocation;

    p7_emit("OS_P7_GLIBC_HELLO\n");

    allocation = malloc(16384U);
    if (allocation == NULL) return p7_fail("OS_P7_MALLOC_FREE_FAIL\n");
    memset(allocation, 0xa5, 16384U);
    if (((unsigned char *)allocation)[0] != 0xa5U ||
        ((unsigned char *)allocation)[16383U] != 0xa5U) {
        free(allocation);
        return p7_fail("OS_P7_MALLOC_FREE_FAIL\n");
    }
    free(allocation);
    p7_emit("OS_P7_MALLOC_FREE_PASS\n");

    descriptor = open("/usr/apps/p7-glibc/p7-data.txt", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0 ||
        read(descriptor, file_data, expected_data_size) !=
            (ssize_t)expected_data_size ||
        memcmp(file_data, expected_data, expected_data_size) != 0 ||
        fstat(descriptor, &file_stat) != 0 ||
        file_stat.st_size != (off_t)expected_data_size ||
        close(descriptor) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return p7_fail("OS_P7_FILE_IO_FAIL\n");
    }
    p7_emit("OS_P7_FILE_IO_PASS\n");

    p7_tls_value = UINT64_C(0x1122334455667788);
    if (pthread_create(&thread, NULL, p7_thread_entry,
                       (void *)(uintptr_t)1U) != 0 ||
        pthread_join(thread, &thread_result) != 0 || p7_thread_failure != 0 ||
        thread_result != (void *)(uintptr_t)(
            UINT64_C(1) ^ UINT64_C(0x5a5a5a5a5a5a5a5a)) ||
        p7_tls_value != UINT64_C(0x1122334455667788)) {
        return p7_fail("OS_P7_PTHREAD_TLS_FAIL\n");
    }
    p7_emit("OS_P7_PTHREAD_CREATE_JOIN_PASS\n");
    p7_emit("OS_P7_TLS_PASS\n");

    for (uint64_t iteration = 0; iteration < P7_THREAD_ITERATIONS; ++iteration) {
        uint64_t expected = iteration + 2U;
        p7_thread_failure = 0;
        thread_result = NULL;
        if (pthread_create(&thread, NULL, p7_thread_entry,
                           (void *)(uintptr_t)expected) != 0 ||
            pthread_join(thread, &thread_result) != 0 ||
            p7_thread_failure != 0 ||
            thread_result != (void *)(uintptr_t)(
                expected ^ UINT64_C(0x5a5a5a5a5a5a5a5a))) {
            return p7_fail("OS_P7_THREAD_10000_FAIL\n");
        }
    }
    p7_emit("OS_P7_THREAD_10000_PASS\n");
    p7_emit("OS_P7_PASS\n");
    return 0;
}
