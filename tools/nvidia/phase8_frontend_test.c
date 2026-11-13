/* 阶段 8 专用的 Linux/glibc NVIDIA 前端探测程序。 */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <uapi/nvidia_frontend.h>

/* 即使主机没有单独的 linux-libc-dev 头文件，也要确保此探测程序能使用固定版本的
 * glibc 开发包构建。这些是 open(2) 使用的稳定 x86-64 Linux 值。 */
#define P8_O_RDWR 2
#define P8_O_CLOEXEC 02000000

extern int open(const char *path, int flags, ...);
extern int ioctl(int descriptor, unsigned long request, ...);

static void p8_emit(const char *text)
{
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static int p8_fail(const char *text)
{
    p8_emit(text);
    return 1;
}

int main(void)
{
    nv_frontend_card_info_t card = {0};
    nv_frontend_rm_api_version_t version = {0};
    nv_frontend_query_device_intr_t query = {0};
    void *mapping;
    int control = -1;
    int device = -1;
    int result = 1;

    p8_emit("OS_P8_FRONTEND_PROBE_BEGIN\n");
    control = open("/dev/nvidiactl", P8_O_RDWR | P8_O_CLOEXEC);
    if (control < 0)
        return p8_fail("OS_P8_OPEN_NVIDIACTL_FAIL\n");
    device = open("/dev/nvidia0", P8_O_RDWR | P8_O_CLOEXEC);
    if (device < 0) {
        (void)close(control);
        return p8_fail("OS_P8_OPEN_NVIDIA0_FAIL\n");
    }
    p8_emit("OS_P8_OPEN_NVIDIACTL_PASS\n");
    p8_emit("OS_P8_OPEN_NVIDIA0_PASS\n");

    if (ioctl(control, NV_FRONTEND_IOCTL_CARD_INFO, &card) != 0 ||
        card.valid == 0U || card.reg_address == 0U ||
        card.reg_size < 4096U)
        goto cleanup;
    p8_emit("OS_P8_DEVICE_QUERY_PASS\n");

    version.cmd = NV_FRONTEND_RM_API_VERSION_CMD_QUERY;
    if (ioctl(control, NV_FRONTEND_IOCTL_CHECK_VERSION, &version) != 0 ||
        version.reply != NV_FRONTEND_RM_API_VERSION_REPLY_RECOGNIZED)
        goto cleanup;
    p8_emit("OS_P8_RM_VERSION_QUERY_PASS\n");

    if (ioctl(device, NV_FRONTEND_IOCTL_QUERY_DEVICE_INTR, &query) != 0 ||
        query.status != 0U)
        goto cleanup;
    p8_emit("OS_P8_DEVICE_INTR_QUERY_PASS\n");

    mapping = mmap(NULL, 4096U, PROT_READ, MAP_SHARED, device,
                   (off_t)card.reg_address);
    if (mapping == MAP_FAILED)
        goto cleanup;
    if (munmap(mapping, 4096U) != 0)
        goto cleanup;
    p8_emit("OS_P8_FRONTEND_MMAP_PASS\n");
    result = 0;

cleanup:
    if (close(device) != 0)
        result = 1;
    if (close(control) != 0)
        result = 1;
    if (result != 0)
        return p8_fail("OS_P8_FRONTEND_PROBE_FAIL\n");
    p8_emit("OS_P8_CLOSE_PASS\n");
    p8_emit("OS_P8_PASS\n");
    return 0;
}
