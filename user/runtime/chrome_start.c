/* 为 Debian 构建的 Chrome ELF 提供确定性的首次运行启动器。 */

#include <libc/libc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stddef.h>
#include <uapi/linux_abi.h>
#include <uapi/network.h>
#include <uapi/syscall.h>

#define CHROME_START_ARGUMENT_LIMIT 32U
#define CHROME_PROXY_ARGUMENT_LIMIT 256U

/* System 网络可能无法完成异步推测请求。Chromium 的预取响应读取器目前会在此失败路径
 * 触发 CHECK 并中止，因此在此处支持并验证其生命周期前，保持推测预取/预渲染关闭。
 * 仅聚焦地址栏时，Omnibox 搜索引擎预热也会启动推测渲染器；避免交互路径因此承担
 * fork/VM 锁延迟。WebUI Omnibox 下拉菜单聚焦时也会独立创建 WebContents（以及备用渲染器）。
 * 在 System 上使用现有原生 Views 弹出窗口，保留建议功能，避免每次点击都启动渲染器。
 * 普通导航不受影响。 */
/* 当前应用包未提供本地模型后端，关闭模型执行，避免后台服务加载失败后反复创建进程。 */
#define CHROME_DISABLED_FEATURES \
    "PrefetchUseContentRefactor,PrefetchPrerenderIntegration,Prewarm," \
    "WebUIOmniboxPopup,WebUIOmniboxFullPopup,WebUIOmniboxAimPopup," \
    "OptimizationGuideModelExecution"
#define CHROME_DISABLE_FEATURES_ARGUMENT \
    "--disable-features=" CHROME_DISABLED_FEATURES

/* QEMU 用户网络的主机别名可直接访问主机回环代理；代理不可用时保留直接连接作为后备。 */
#define CHROME_QEMU_DEFAULT_PROXY "http://10.0.2.2:7890,direct://"

static char *const g_default_arguments[] = {
    "/usr/apps/chrome/chrome",
    "--no-sandbox",
    /* 此程序运行于 QEMU 测试环境。Chromium 测试模式会隐藏启动提示栏，
     * 但不会伪装成已启用沙箱。 */
    "--test-type",
    /* 将警告/错误送入内核有界的 Chrome stderr 捕获通道，使渲染器/导航故障能显示在
     * QEMU 串口日志中，而无需启用 Chromium 的高频详细日志。 */
    "--enable-logging=stderr",
    "--log-level=1",
    "--no-zygote",
    "--in-process-gpu",
    "--disable-dev-shm-usage",
    "--disable-background-networking",
    "--disable-component-update",
    "--disable-default-apps",
    "--disable-extensions",
    "--disable-sync",
    "--disable-breakpad",
    /* 一字节上限会淘汰所有响应，却仍要承担 SQLite 创建/写入/淘汰开销。
     * 在应用数据目录中保留实用且有界的缓存：标准 4 GiB 客户机可缓存资源，
     * 同时避免无限增长的默认缓存或推测请求。配置与缓存随应用目录保留，重启后继续使用。 */
    "--disk-cache-size=67108864",
    "--media-cache-size=16777216",
    CHROME_DISABLE_FEATURES_ARGUMENT,
    "--no-first-run",
    "--no-default-browser-check",
    "--ozone-platform=nativeguest",
    "--profile-directory=Default",
    "--user-data-dir=/usr/apps/chrome/data",
    "https://www.google.com/",
};

static char *const g_software_arguments[] = {
    "--disable-gpu", "--disable-gpu-compositing", "--disable-gpu-program-cache",
};
static char *const g_vulkan_arguments[] = {
    "--enable-features=Vulkan", "--use-vulkan=native",
    "--disable-vulkan-surface", "--use-gl=disabled",
    "--enable-gpu-rasterization",
};

/* 仅在匹配的浏览器和资源准备完成后，暂存器才会发布此配置。
 * 配置缺失时沿用旧版纯软件映像；格式错误或 I/O 失败会明确报告，不会静默宣称
 * 已启用硬件加速。 */
static int chrome_gpu_profile(void) {
    char buffer[17];
    size_t length = 0U;
    int descriptor = open("/usr/apps/chrome/gpu-mode", O_RDONLY);
    if (descriptor < 0) return errno == ENOENT ? 0 : -1;
    while (length < sizeof(buffer) - 1U) {
        ssize_t count = read(descriptor, buffer + length, sizeof(buffer) - 1U - length);
        if (count < 0) {
            if (errno == EINTR) continue;
            (void)close(descriptor);
            return -1;
        }
        if (count == 0) break;
        length += (size_t)count;
    }
    (void)close(descriptor);
    if (length == 7U && memcmp(buffer, "vulkan\n", 7U) == 0) return 1;
    if (length == 9U && memcmp(buffer, "software\n", 9U) == 0) return 0;
    return -1;
}

static void chrome_start_debug_mark(const char *text, size_t length) {
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
}

static bool chrome_has_explicit_proxy(int argc, char **argv) {
    static const char *const switches[] = {
        "--proxy-server=", "--proxy-pac-url=", "--no-proxy-server",
        "--proxy-auto-detect",
    };
    for (int index = 1; index < argc; ++index) {
        if (argv == 0 || argv[index] == 0) continue;
        for (size_t option = 0U;
             option < sizeof(switches) / sizeof(switches[0]); ++option) {
            size_t length = strlen(switches[option]);
            if (strncmp(argv[index], switches[option], length) == 0 &&
                (switches[option][length - 1U] == '=' ||
                 argv[index][length] == '\0')) return true;
        }
    }
    return false;
}

static bool chrome_video_disabled(int argc, char **argv) {
    static const char *const switches[] = {
        "--disable-gpu", "--disable-accelerated-video-decode",
    };
    for (int index = 1; index < argc; ++index) {
        if (argv == 0 || argv[index] == 0) continue;
        for (size_t option = 0U; option < sizeof(switches) / sizeof(switches[0]); ++option) {
            size_t length = strlen(switches[option]);
            if (strncmp(argv[index], switches[option], length) == 0 &&
                (argv[index][length] == '\0' || argv[index][length] == '=')) return true;
        }
    }
    return false;
}

static const char *chrome_default_proxy(void) {
    os_net_status_t status = {0};
    status.hdr.size = sizeof(status);
    status.hdr.version = OS_SYSCALL_ABI_VERSION;
    if (__libc_native_syscall6(OS_SYS_NET_GET_STATUS,
            (uint64_t)(uintptr_t)&status, 0U, 0U, 0U, 0U, 0U) < 0)
        return "";
    /* 主机别名仅在标准 QEMU 用户网络中存在。 */
    if ((status.flags & OS_NET_STATUS_HARDWARE_PRESENT) != 0U &&
        status.mac[0] == 0x52U && status.mac[1] == 0x54U && status.mac[2] == 0U &&
        (status.ipv4_address == 0U ||
         (status.ipv4_address & 0xFFFFFF00U) == 0x0A000200U))
        return CHROME_QEMU_DEFAULT_PROXY;
    return "";
}

int main(int argc, char **argv) {
    char *chrome_arguments[CHROME_START_ARGUMENT_LIMIT];
    char proxy_argument[CHROME_PROXY_ARGUMENT_LIMIT];
    static const char proxy_prefix[] = "--proxy-server=";
    const char *proxy_server = getenv("OS_CHROME_PROXY_SERVER");
    size_t default_count = sizeof(g_default_arguments) /
                           sizeof(g_default_arguments[0]);
    size_t count = 0U;

    if (argc < 0 || (argc > 0 && argv == 0)) return 2;
    /* 运行锁、套接字及临时文件留在应用自己的易失目录，配置目录继续持久保存。 */
    if (mkdir("/usr/apps/chrome/run", 0700) != 0 && errno != EEXIST) return 2;
    if (setenv("TMPDIR", "/usr/apps/chrome/run", 1) != 0) return 2;
    int gpu_profile = chrome_gpu_profile();
    if (gpu_profile < 0) {
        chrome_start_debug_mark("OS_CHROME_START_GPU_PROFILE_INVALID\n",
                               sizeof("OS_CHROME_START_GPU_PROFILE_INVALID\n") - 1U);
        return 2;
    }
    char *const *gpu_arguments = gpu_profile ? g_vulkan_arguments : g_software_arguments;
    /* 渲染器命令行不会保留浏览器的所有 GPU 开关。通过 exec 传递所选模式，确保软件
     * 诊断路径继续使用软件实现。 */
    if (setenv("OS_CHROME_GPU_MODE",
               gpu_profile && !chrome_video_disabled(argc, argv) ? "vulkan" : "software", 1) != 0)
        return 2;
    size_t gpu_count = gpu_profile ?
        sizeof(g_vulkan_arguments) / sizeof(g_vulkan_arguments[0]) :
        sizeof(g_software_arguments) / sizeof(g_software_arguments[0]);
    /* 设置存在但值为空时，显式选择直接访问。 */

    chrome_start_debug_mark("OS_CHROME_START_MAIN\n",
                           sizeof("OS_CHROME_START_MAIN\n") - 1U);

    if (default_count + gpu_count + 2U >= CHROME_START_ARGUMENT_LIMIT) return 2;
    for (size_t index = 0U; index + 1U < default_count; ++index) {
        chrome_arguments[count++] = g_default_arguments[index];
    }
    for (size_t index = 0U; index < gpu_count; ++index) {
        chrome_arguments[count++] = gpu_arguments[index];
    }
    if (proxy_server == 0) proxy_server = chrome_default_proxy();
    if (!chrome_has_explicit_proxy(argc, argv) && proxy_server[0] != '\0') {
        size_t proxy_length = strlen(proxy_server);
        if (proxy_length > sizeof(proxy_argument) - sizeof(proxy_prefix))
            return 2;
        memcpy(proxy_argument, proxy_prefix, sizeof(proxy_prefix) - 1U);
        memcpy(proxy_argument + sizeof(proxy_prefix) - 1U, proxy_server,
               proxy_length + 1U);
        chrome_arguments[count++] = proxy_argument;
    }
    chrome_arguments[count++] = g_default_arguments[default_count - 1U];
    for (int index = 1; index < argc; ++index) {
        if (count + 1U >= CHROME_START_ARGUMENT_LIMIT) return 2;
        if (argv == 0 || argv[index] == 0) return 2;
        chrome_arguments[count++] = argv[index];
    }
    chrome_arguments[count] = 0;

    if (gpu_profile && setenv("VK_DRIVER_FILES",
            "/usr/apps/chrome/vulkan/nvidia_icd.json", 1) != 0) {
        chrome_start_debug_mark("OS_CHROME_START_GPU_ENV_FAILED\n",
                               sizeof("OS_CHROME_START_GPU_ENV_FAILED\n") - 1U);
        return 2;
    }
    if (gpu_profile && setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
            "/usr/apps/chrome/egl/10_nvidia.json", 1) != 0)
        return 2;
    chrome_start_debug_mark("OS_CHROME_START_BEFORE_EXEC\n",
                           sizeof("OS_CHROME_START_BEFORE_EXEC\n") - 1U);
    (void)execv(chrome_arguments[0], chrome_arguments);
    chrome_start_debug_mark("OS_CHROME_START_EXEC_RETURN\n",
                           sizeof("OS_CHROME_START_EXEC_RETURN\n") - 1U);
    return 127;
}
