/* 仅在来宾系统检查 FAT 写入、同步和扩容；不得在宿主执行。 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static unsigned char payload[1024U * 1024U + 130U];
static unsigned char received[sizeof(payload)];

static bool verify_contents(int fd, size_t length) {
    struct stat status;
    memset(received, 0xcc, length);
    return fstat(fd, &status) == 0 && status.st_size == (off_t)length &&
        lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, received, length) == (ssize_t)length &&
        memcmp(payload, received, length) == 0;
}

static int growth_probe(void) {
    const char *path = "/growth-qemu-qa";
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    int result = 1;
    if (fd < 0) goto report;
    /* 先写入非零数据，再截短，避免零初始化介质掩盖旧数据泄露。 */
    memset(payload, 0xa5, 4097U);
    if (write(fd, payload, 4097U) != 4097 || fsync(fd) != 0 ||
        ftruncate(fd, 17) != 0) goto cleanup;
    memset(payload + 17U, 0, 70004U - 17U);
    if (ftruncate(fd, 100) != 0 || !verify_contents(fd, 100U) ||
        ftruncate(fd, 65537) != 0 || !verify_contents(fd, 65537U)) goto cleanup;
    /* 越过 EOF 的空洞必须为零；零字节写入不能改变文件长度。 */
    payload[70003U] = 0x42;
    if (lseek(fd, 70003, SEEK_SET) != 70003 ||
        write(fd, payload + 70003U, 1U) != 1 ||
        lseek(fd, 100000, SEEK_SET) != 100000 || write(fd, "", 0U) != 0 ||
        fsync(fd) != 0 || !verify_contents(fd, 70004U)) goto cleanup;
    if (close(fd) != 0) { fd = -1; goto cleanup; }
    fd = open(path, O_RDONLY);
    if (fd >= 0 && verify_contents(fd, 70004U)) result = 0;
cleanup:
    if (fd >= 0 && close(fd) != 0) result = 1;
    if (unlink(path) != 0) result = 1;
report:
    const char *message = result ? "FAT GROWTH FAIL\n" : "FAT GROWTH PASS\n";
    (void)write(1, message, strlen(message));
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--growth") == 0) return growth_probe();
    const char *path = "/fsync-qemu-qa";
    const size_t length = sizeof(payload) - 7U;
    bool keep = argc == 2 && strcmp(argv[1], "--keep") == 0;
    for (size_t i = 0U; i < sizeof(payload); ++i)
        payload[i] = (unsigned char)((i * 37U + (i >> 9U)) & 255U);
    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        int verify_fd = open(path, O_RDONLY);
        int verify_result = 1;
        if (verify_fd >= 0) {
            struct stat status;
            if (fstat(verify_fd, &status) == 0 && status.st_size == (off_t)length &&
                read(verify_fd, received + 3U, length) == (ssize_t)length &&
                memcmp(payload + 7U, received + 3U, length) == 0) verify_result = 0;
            if (close(verify_fd) != 0) verify_result = 1;
            if (verify_result == 0 && unlink(path) != 0) verify_result = 1;
        }
        const char *message = verify_result ? "FAT PERSIST FAIL\n" : "FAT PERSIST PASS\n";
        (void)write(1, message, strlen(message));
        return verify_result;
    }
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    int result = 1;
    if (fd < 0) {
        (void)write(1, "FAT FSYNC CREATE FAIL\n", 22);
        return result;
    }
    /* 非对齐地址和长度覆盖部分 DMA 页与拆分 SCSI 请求。
     * --keep 保留文件供离线核验，防止只读缓存造成误判。 */
    if (write(fd, payload + 7U, length) == (ssize_t)length &&
        fsync(fd) == 0 && lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, received + 3U, length) == (ssize_t)length &&
        memcmp(payload + 7U, received + 3U, length) == 0) result = 0;
    if (close(fd) != 0) result = 1;
    if (!keep && unlink(path) != 0) result = 1;
    const char *message = result ? "FAT FSYNC FAIL\n" : "FAT FSYNC PASS\n";
    (void)write(1, message, strlen(message));
    return result;
}
