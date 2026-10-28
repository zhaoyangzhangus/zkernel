#define main chrome_start_program_main
#define setenv chrome_test_setenv
#define mkdir chrome_test_mkdir
#include "../../user/runtime/chrome_start.c"
#undef setenv
#undef mkdir
#undef main

extern int setenv(const char *, const char *, int);
static int g_gpu_env_error;
int chrome_test_setenv(const char *name, const char *value, int overwrite) {
    int valid_mode = strcmp(name, "OS_CHROME_GPU_MODE") == 0 &&
        (strcmp(value, "software") == 0 || strcmp(value, "vulkan") == 0);
    int valid_icd = strcmp(name, "VK_DRIVER_FILES") == 0 &&
        strcmp(value, "/usr/apps/chrome/vulkan/nvidia_icd.json") == 0;
    int valid_temporary = strcmp(name, "TMPDIR") == 0 &&
        strcmp(value, "/usr/apps/chrome/run") == 0;
    int valid_egl = strcmp(name, "__EGL_VENDOR_LIBRARY_FILENAMES") == 0 &&
        strcmp(value, "/usr/apps/chrome/egl/10_nvidia.json") == 0;
    if ((!valid_mode && !valid_icd && !valid_temporary && !valid_egl) ||
        overwrite != 1 || g_gpu_env_error) {
        errno = ENOMEM;
        return -1;
    }
    return setenv(name, value, overwrite);
}

int chrome_test_mkdir(const char *path, mode_t mode) {
    if (strcmp(path, "/usr/apps/chrome/run") || mode != 0700) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static const char *g_exec_path;
static const char *g_exec_arguments[CHROME_START_ARGUMENT_LIMIT];
static size_t g_exec_argument_count;
static unsigned g_exec_calls;
static int g_bad_debug_mark;
static char g_argument_storage[CHROME_START_ARGUMENT_LIMIT][512];
static char g_proxy_argument[CHROME_PROXY_ARGUMENT_LIMIT];
static const char *g_profile;
static size_t g_profile_size, g_profile_position;
static int g_profile_error, g_profile_interrupt;
static unsigned g_profile_closes;
static int g_network_error;
static os_net_status_t g_network = {
    .flags = OS_NET_STATUS_HARDWARE_PRESENT | OS_NET_STATUS_LINK_UP,
    .ipv4_address = 0x0A00020FU,
    .mac = {0x52, 0x54, 0, 0x12, 0x34, 0x56},
};

int64_t __libc_native_syscall6(uint64_t number, uint64_t pointer,
    uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (number != OS_SYS_NET_GET_STATUS || !pointer || a || b || c || d || e)
        return -22;
    os_net_status_t *status = (os_net_status_t *)(uintptr_t)pointer;
    if (status->hdr.size != sizeof(*status) ||
        status->hdr.version != OS_SYSCALL_ABI_VERSION) return -22;
    if (g_network_error) return -5;
    *status = g_network;
    return 0;
}

int open(const char *path, int flags, ...) {
    if (strcmp(path, "/usr/apps/chrome/gpu-mode") != 0 || flags != O_RDONLY) {
        errno = EINVAL;
        return -1;
    }
    if (g_profile == 0) {
        errno = g_profile_error ? g_profile_error : ENOENT;
        return -1;
    }
    g_profile_position = 0U;
    return 777;
}

ssize_t read(int descriptor, void *buffer, size_t length) {
    if (descriptor != 777) { errno = EBADF; return -1; }
    if (g_profile_interrupt) { g_profile_interrupt = 0; errno = EINTR; return -1; }
    if (g_profile_error) { errno = g_profile_error; return -1; }
    size_t remaining = g_profile_size - g_profile_position;
    if (length > remaining) length = remaining;
    if (length > 3U) length = 3U;
    memcpy(buffer, g_profile + g_profile_position, length);
    g_profile_position += length;
    return (ssize_t)length;
}

int close(int descriptor) {
    if (descriptor != 777) { errno = EBADF; return -1; }
    ++g_profile_closes;
    return 0;
}

static int test_string_equal(const char *left, const char *right) {
    if (left == 0 || right == 0) return left == right;
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return 0;
        ++left;
        ++right;
    }
    return *left == *right;
}

static int test_has_argument(const char *expected) {
    for (size_t index = 0U; index < g_exec_argument_count; ++index) {
        if (test_string_equal(g_exec_arguments[index], expected)) return 1;
    }
    return 0;
}

static int test_fail(const char *message) {
    size_t length = 0U;
    while (message[length] != '\0') ++length;
    (void)write(STDERR_FILENO, message, length);
    (void)write(STDERR_FILENO, "\n", 1U);
    return 1;
}

int64_t __libc_linux_syscall6(uint64_t number, uint64_t argument0,
                              uint64_t argument1, uint64_t argument2,
                              uint64_t argument3, uint64_t argument4,
                              uint64_t argument5) {
    const char *mark = (const char *)(uintptr_t)argument0;
    if (number != OS_SYS_DEBUG_WRITE || mark == 0 ||
        argument1 != strlen(mark) || argument2 != 0U ||
        argument3 != 0U || argument4 != 0U || argument5 != 0U ||
        (!test_string_equal(mark, "OS_CHROME_START_MAIN\n") &&
         !test_string_equal(mark, "OS_CHROME_START_BEFORE_EXEC\n") &&
         !test_string_equal(mark, "OS_CHROME_START_EXEC_RETURN\n") &&
         !test_string_equal(mark, "OS_CHROME_START_GPU_PROFILE_INVALID\n") &&
         !test_string_equal(mark, "OS_CHROME_START_GPU_ENV_FAILED\n")))
        g_bad_debug_mark = 1;
    return number == OS_SYS_DEBUG_WRITE ? (int64_t)argument1 : -1;
}

int execv(const char *path, char *const arguments[]) {
    ++g_exec_calls;
    g_exec_path = path;
    g_exec_argument_count = 0U;
    g_proxy_argument[0] = '\0';
    while (arguments[g_exec_argument_count] != 0 &&
           g_exec_argument_count < CHROME_START_ARGUMENT_LIMIT) {
        static const char proxy_prefix[] = "--proxy-server=";
        if (strncmp(arguments[g_exec_argument_count], proxy_prefix,
                    sizeof(proxy_prefix) - 1U) == 0) {
            size_t length = strlen(arguments[g_exec_argument_count]);
            if (length < sizeof(g_proxy_argument)) {
                memcpy(g_proxy_argument, arguments[g_exec_argument_count],
                       length + 1U);
            }
        }
        size_t argument_length = strlen(arguments[g_exec_argument_count]);
        if (argument_length >= sizeof(g_argument_storage[0])) return -1;
        memcpy(g_argument_storage[g_exec_argument_count],
               arguments[g_exec_argument_count], argument_length + 1U);
        g_exec_arguments[g_exec_argument_count] = g_argument_storage[g_exec_argument_count];
        ++g_exec_argument_count;
    }
    return -1;
}

int main(void) {
    char *wrapper_arguments[] = {
        "chrome-start",
        "--chrome-launch-regression-test",
        "https://override.invalid/",
        0,
    };
    if (unsetenv("LITEOS_CHROME_PROXY_SERVER") != 0)
        return test_fail("could not clear proxy test environment");
    if (unsetenv("OS_CHROME_PROXY_SERVER") != 0)
        return test_fail("could not clear neutral proxy test environment");
    if (!test_string_equal(CHROME_QEMU_DEFAULT_PROXY,
                          "http://10.0.2.2:7890,direct://"))
        return test_fail("default proxy must use QEMU's direct host alias");
    int status = chrome_start_program_main(3, wrapper_arguments);

    if (strlen(CHROME_DISABLE_FEATURES_ARGUMENT) >= 256U) {
        return test_fail("Chrome feature switch exceeds LiteOS exec argument limit");
    }
    if (!test_has_argument("--disable-features=PrefetchUseContentRefactor,"
                           "PrefetchPrerenderIntegration,Prewarm,"
                           "WebUIOmniboxPopup,WebUIOmniboxFullPopup,"
                           "WebUIOmniboxAimPopup,OptimizationGuideModelExecution")) {
        return test_fail("omnibox must use native suggestions without WebUI renderer startup");
    }
    if (status != 127) return test_fail("execv failure path was not reached");
    if (!test_string_equal(g_exec_path, "/usr/apps/chrome/chrome")) {
        return test_fail("chrome-start did not exec /usr/apps/chrome/chrome");
    }
    if (g_exec_argument_count == 0U ||
        !test_string_equal(g_exec_arguments[0], "/usr/apps/chrome/chrome")) {
        return test_fail("Chrome argv[0] is incorrect");
    }
    if (test_has_argument("--single-process")) {
        return test_fail("Chrome was launched with the crashing single-process mode");
    }
    if (!test_has_argument("--in-process-gpu")) {
        return test_fail("Chrome GPU service must share the system surface registry");
    }
    if (!test_has_argument("--no-sandbox") ||
        !test_has_argument("--test-type") ||
        !test_has_argument("--enable-logging=stderr") ||
        !test_has_argument("--log-level=1") ||
        !test_has_argument("--no-zygote") ||
        !test_has_argument("--ozone-platform=nativeguest") ||
        !test_has_argument("--disk-cache-size=67108864") ||
        !test_has_argument("--media-cache-size=16777216") ||
        !test_has_argument(CHROME_DISABLE_FEATURES_ARGUMENT) ||
        !test_has_argument("--chrome-launch-regression-test") ||
        !test_has_argument("https://override.invalid/")) {
        return test_fail("Chrome launch arguments were not preserved");
    }
    if (!test_string_equal(g_proxy_argument,
            "--proxy-server=" CHROME_QEMU_DEFAULT_PROXY))
        return test_fail("desktop launch did not select the QEMU host proxy");
    status = chrome_start_program_main(0, 0);
    if (status != 127 ||
        !test_string_equal(g_exec_path, "/usr/apps/chrome/chrome") ||
        !test_has_argument("https://www.google.com/") ||
        !test_has_argument("--test-type") ||
        !test_has_argument("--disk-cache-size=67108864") ||
        !test_has_argument("--media-cache-size=16777216") ||
        !test_string_equal(g_proxy_argument,
            "--proxy-server=" CHROME_QEMU_DEFAULT_PROXY))
        return test_fail("zero-argument desktop launch was not preserved");
    if (setenv("OS_CHROME_PROXY_SERVER", "http://10.0.2.100:7890", 1) != 0)
        return test_fail("could not set proxy test environment");
    status = chrome_start_program_main(3, wrapper_arguments);
    if (status != 127 || !test_string_equal(
            g_proxy_argument, "--proxy-server=http://10.0.2.100:7890"))
        return test_fail("Chrome did not inherit the configured proxy");
    char *direct_arguments[] = {
        "chrome-start", "--no-proxy-server", 0,
    };
    status = chrome_start_program_main(2, direct_arguments);
    if (status != 127 || g_proxy_argument[0] != '\0' ||
        !test_has_argument("--no-proxy-server"))
        return test_fail("explicit direct mode did not override the proxy");
    char *other_proxy_arguments[] = {
        "chrome-start", "--proxy-server=http://other-proxy:8080", 0,
    };
    status = chrome_start_program_main(2, other_proxy_arguments);
    if (status != 127 || !test_string_equal(
            g_proxy_argument, "--proxy-server=http://other-proxy:8080"))
        return test_fail("explicit proxy did not override the default");
    if (setenv("OS_CHROME_PROXY_SERVER", "", 1) != 0)
        return test_fail("could not disable proxy in test environment");
    status = chrome_start_program_main(3, wrapper_arguments);
    if (status != 127 || g_proxy_argument[0] != '\0')
        return test_fail("empty proxy environment did not disable the default");
    (void)unsetenv("OS_CHROME_PROXY_SERVER");
    if (setenv("LITEOS_CHROME_PROXY_SERVER", "http://legacy:7890", 1) != 0)
        return test_fail("could not configure retired proxy setting");
    status = chrome_start_program_main(3, wrapper_arguments);
    if (status != 127 || !test_string_equal(g_proxy_argument,
            "--proxy-server=" CHROME_QEMU_DEFAULT_PROXY))
        return test_fail("retired proxy setting must not change launch configuration");
    if (setenv("OS_CHROME_PROXY_SERVER", "http://neutral:8080", 1) != 0)
        return test_fail("could not configure neutral proxy precedence");
    status = chrome_start_program_main(3, wrapper_arguments);
    if (status != 127 || !test_string_equal(g_proxy_argument,
            "--proxy-server=http://neutral:8080"))
        return test_fail("neutral proxy setting did not override legacy setting");
    if (setenv("OS_CHROME_PROXY_SERVER", "", 1) != 0)
        return test_fail("could not configure neutral direct mode");
    status = chrome_start_program_main(3, wrapper_arguments);
    if (status != 127 || g_proxy_argument[0] != '\0')
        return test_fail("empty neutral setting must override legacy proxy");
    (void)unsetenv("LITEOS_CHROME_PROXY_SERVER");
    (void)unsetenv("OS_CHROME_PROXY_SERVER");
    const char *explicit_options[] = {
        "--proxy-server=http://explicit:7890", "--proxy-pac-url=http://pac/config",
        "--no-proxy-server", "--proxy-auto-detect",
    };
    for (size_t i = 0; i < sizeof(explicit_options)/sizeof(explicit_options[0]); ++i) {
        char *options[] = {"chrome-start", (char *)explicit_options[i], 0};
        status = chrome_start_program_main(2, options);
        if (status != 127 || !test_has_argument(explicit_options[i]))
            return test_fail("explicit proxy mode was lost");
        if (i != 0 && g_proxy_argument[0] != '\0')
            return test_fail("explicit proxy mode retained default proxy");
    }
    char *near_option[] = {"chrome-start", "--no-proxy-server-extra", 0};
    status = chrome_start_program_main(2, near_option);
    if (status != 127 || !test_string_equal(g_proxy_argument,
            "--proxy-server=" CHROME_QEMU_DEFAULT_PROXY))
        return test_fail("near-name option incorrectly disabled the proxy");
    char proxy[CHROME_PROXY_ARGUMENT_LIMIT];
    size_t maximum = sizeof(proxy) - sizeof("--proxy-server=");
    memset(proxy, 'x', maximum); proxy[maximum] = '\0';
    if (setenv("OS_CHROME_PROXY_SERVER", proxy, 1) != 0)
        return test_fail("could not configure maximum proxy length");
    status = chrome_start_program_main(0, 0);
    if (status != 127 || strlen(g_proxy_argument) != CHROME_PROXY_ARGUMENT_LIMIT - 1U)
        return test_fail("maximum fitting proxy length was rejected");
    proxy[maximum++] = 'x'; proxy[maximum] = '\0';
    if (setenv("OS_CHROME_PROXY_SERVER", proxy, 1) != 0)
        return test_fail("could not configure oversized proxy");
    unsigned calls = g_exec_calls;
    if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
        return test_fail("oversized proxy reached exec");
    (void)unsetenv("OS_CHROME_PROXY_SERVER");
    if (chrome_start_program_main(-1, wrapper_arguments) != 2 ||
        chrome_start_program_main(1, 0) != 2 || g_exec_calls != calls)
        return test_fail("invalid argument vector reached exec");
    char *missing_argument[] = {"chrome-start", 0};
    if (chrome_start_program_main(2, missing_argument) != 2 || g_exec_calls != calls)
        return test_fail("missing argument reached exec");
    char *many[CHROME_START_ARGUMENT_LIMIT + 1U];
    for (size_t i = 0; i < CHROME_START_ARGUMENT_LIMIT; ++i) many[i] = "--test";
    many[CHROME_START_ARGUMENT_LIMIT] = 0;
    if (chrome_start_program_main(CHROME_START_ARGUMENT_LIMIT, many) != 2 ||
        g_exec_calls != calls)
        return test_fail("overflowing argument vector reached exec");
    g_profile = "vulkan\n"; g_profile_size = 7U; g_profile_interrupt = 1;
    unsigned closes = g_profile_closes;
    if (chrome_start_program_main(0, 0) != 127 || g_profile_closes != closes + 1U ||
        !test_has_argument("--enable-features=Vulkan") ||
        !test_has_argument("--use-vulkan=native") ||
        !test_has_argument("--disable-vulkan-surface") ||
        !test_has_argument("--use-gl=disabled") ||
        !test_has_argument("--enable-gpu-rasterization") ||
        !test_has_argument("--in-process-gpu") ||
        test_has_argument("--disable-gpu") || test_has_argument("--disable-gpu-compositing") ||
        test_has_argument("--disable-gpu-program-cache") ||
        !test_has_argument("https://www.google.com/"))
        return test_fail("Vulkan desktop profile or partial/EINTR read routing failed");
    if (chrome_start_program_main(3, wrapper_arguments) != 127 ||
        !test_has_argument("--chrome-launch-regression-test") ||
        !test_has_argument("https://override.invalid/"))
        return test_fail("Vulkan profile lost explicit arguments");
    if (!test_string_equal(getenv("VK_DRIVER_FILES"),
                           "/usr/apps/chrome/vulkan/nvidia_icd.json"))
        return test_fail("Vulkan profile did not select the deployed NVIDIA ICD");
    if (!test_string_equal(getenv("__EGL_VENDOR_LIBRARY_FILENAMES"),
                           "/usr/apps/chrome/egl/10_nvidia.json"))
        return test_fail("Vulkan profile lost the private EGL vendor configuration");
    if (!test_string_equal(getenv("OS_CHROME_GPU_MODE"), "vulkan"))
        return test_fail("Vulkan mode was not propagated to child processes");
    char *disable_video[] = {"chrome-start", "--disable-accelerated-video-decode", 0};
    if (chrome_start_program_main(2, disable_video) != 127 ||
        !test_string_equal(getenv("OS_CHROME_GPU_MODE"), "software"))
        return test_fail("explicit video disable was not propagated");
    char *near_disable[] = {"chrome-start", "--disable-gpu-compositing", 0};
    if (chrome_start_program_main(2, near_disable) != 127 ||
        !test_string_equal(getenv("OS_CHROME_GPU_MODE"), "vulkan"))
        return test_fail("compositing-only switch incorrectly disabled video");
    if (setenv("VK_DRIVER_FILES", "/invalid/inherited.json", 1) != 0)
        return test_fail("could not prepare inherited ICD regression");
    if (chrome_start_program_main(0, 0) != 127 ||
        !test_string_equal(getenv("VK_DRIVER_FILES"),
                           "/usr/apps/chrome/vulkan/nvidia_icd.json"))
        return test_fail("inherited ICD overrode the hardware profile");
    calls = g_exec_calls;
    g_gpu_env_error = 1;
    if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
        return test_fail("GPU environment failure reached exec");
    g_gpu_env_error = 0;
    size_t fitting = CHROME_START_ARGUMENT_LIMIT - 1U -
        (sizeof(g_default_arguments)/sizeof(g_default_arguments[0]) +
         sizeof(g_vulkan_arguments)/sizeof(g_vulkan_arguments[0]) + 1U);
    if (chrome_start_program_main((int)fitting + 1, many) != 127 ||
        g_exec_argument_count != CHROME_START_ARGUMENT_LIMIT - 1U)
        return test_fail("Vulkan profile rejected the fitting argument boundary");
    calls = g_exec_calls;
    if (chrome_start_program_main((int)fitting + 2, many) != 2 || g_exec_calls != calls)
        return test_fail("Vulkan profile accepted overflowing arguments");
    g_profile = "software\n"; g_profile_size = 9U;
    if (chrome_start_program_main(0, 0) != 127 ||
        !test_has_argument("--disable-gpu") || !test_has_argument("--disable-gpu-compositing") ||
        test_has_argument("--use-vulkan=native") ||
        test_has_argument("--enable-gpu-rasterization"))
        return test_fail("software profile retained Vulkan switches");
    if (!test_string_equal(getenv("OS_CHROME_GPU_MODE"), "software"))
        return test_fail("software mode was not propagated to child processes");
    static const char *const invalid_profiles[] = {
        "", "vulkan", "VULKAN\n", "vulkan\nextra", "software\nextra", "staging\n",
        "vulkan-profile-that-is-too-long",
    };
    calls = g_exec_calls;
    for (size_t i = 0U; i < sizeof(invalid_profiles)/sizeof(invalid_profiles[0]); ++i) {
        g_profile = invalid_profiles[i]; g_profile_size = strlen(g_profile);
        if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
            return test_fail("invalid GPU profile reached exec");
    }
    g_profile = "vulkan\0"; g_profile_size = 7U;
    if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
        return test_fail("embedded NUL GPU profile reached exec");
    g_profile_error = EIO; g_profile = "vulkan\n"; g_profile_size = 7U;
    if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
        return test_fail("GPU profile read failure silently fell back");
    g_profile = 0;
    if (chrome_start_program_main(0, 0) != 2 || g_exec_calls != calls)
        return test_fail("GPU profile open failure silently fell back");
    g_profile_error = 0;
    g_network.mac[0] = 0xA8;
    g_network.ipv4_address = 0xC0A80164U;
    if (chrome_start_program_main(0, 0) != 127 || g_proxy_argument[0])
        return test_fail("physical LAN inherited QEMU proxy");
    g_network.mac[0] = 0x52;
    if (chrome_start_program_main(0, 0) != 127 || g_proxy_argument[0])
        return test_fail("non-user-network subnet inherited QEMU proxy");
    g_network_error = 1;
    if (chrome_start_program_main(0, 0) != 127 || g_proxy_argument[0])
        return test_fail("failed network query inherited QEMU proxy");
    if (g_bad_debug_mark) return test_fail("incorrect startup diagnostic marker or length");
    return 0;
}
