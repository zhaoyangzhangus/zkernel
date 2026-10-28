#include "libc/libc.h"

#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <net/if.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <uapi/memory.h>

/*
 * System 刻意保持原生 ABI 精简。即使 Chromium、libc++ 和 fontconfig 的相关功能
 * 在运行时被禁用，它们仍会引用少量 Linux/POSIX 入口。将这些兼容函数集中在一个编译单元，
 * 使 freestanding libc 导出一致的 C 链接接口。
 */

static int parse_decimal(const char **input, unsigned int digits,
                         int *value) {
    unsigned int index;
    int result = 0;
    if (input == 0 || *input == 0 || value == 0) return -1;
    for (index = 0U; index < digits; ++index) {
        unsigned char character = (unsigned char)(*input)[index];
        if (character < '0' || character > '9') return -1;
        result = result * 10 + (int)(character - '0');
    }
    *input += digits;
    *value = result;
    return 0;
}

static const char *parse_strptime_format(const char *input,
                                         const char *format,
                                         struct tm *value) {
    while (*format != '\0') {
        if (*format != '%') {
            if (isspace((unsigned char)*format)) {
                while (isspace((unsigned char)*input)) ++input;
            } else if (*input != *format) {
                return 0;
            } else {
                ++input;
            }
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            if (*input++ != '%') return 0;
        } else if (*format == 'F') {
            input = parse_strptime_format(input, "%Y-%m-%d", value);
        } else if (*format == 'T') {
            input = parse_strptime_format(input, "%H:%M:%S", value);
        } else {
            int parsed;
            unsigned int digits;
            switch (*format) {
            case 'Y':
                if (parse_decimal(&input, 4U, &parsed) < 0) return 0;
                value->tm_year = parsed - 1900;
                break;
            case 'y':
                if (parse_decimal(&input, 2U, &parsed) < 0) return 0;
                value->tm_year = (parsed < 69 ? parsed + 100 : parsed) - 0;
                break;
            case 'm':
                digits = 2U;
                if (parse_decimal(&input, digits, &parsed) < 0) return 0;
                value->tm_mon = parsed - 1;
                break;
            case 'd':
            case 'e':
                while (isspace((unsigned char)*input)) ++input;
                if (parse_decimal(&input, 2U, &parsed) < 0) return 0;
                value->tm_mday = parsed;
                break;
            case 'H':
                if (parse_decimal(&input, 2U, &parsed) < 0) return 0;
                value->tm_hour = parsed;
                break;
            case 'M':
                if (parse_decimal(&input, 2U, &parsed) < 0) return 0;
                value->tm_min = parsed;
                break;
            case 'S':
                if (parse_decimal(&input, 2U, &parsed) < 0) return 0;
                value->tm_sec = parsed;
                break;
            case 'n':
            case 't':
                while (isspace((unsigned char)*input)) ++input;
                break;
            default:
                return 0;
            }
        }
        if (input == 0) return 0;
        ++format;
    }
    return input;
}

char *strptime(const char *text, const char *format, struct tm *value) {
    const char *result;
    if (text == 0 || format == 0 || value == 0) {
        errno = EINVAL;
        return 0;
    }
    result = parse_strptime_format(text, format, value);
    return (char *)result;
}

unsigned int alarm(unsigned int seconds) {
    /* 信号仅通过显式的 System 信号 ABI 投递。 */
    (void)seconds;
    return 0U;
}

int sched_yield(void) {
    return syscall(LINUX_SYS_SCHED_YIELD) < 0 ? -1 : 0;
}

int sched_getcpu(void) {
    /* System 当前不提供用户态 CPU 查询；CPU 0 是有效值。 */
    return 0;
}

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *mask) {
    long result;
    if (mask == 0 || size < sizeof(*mask)) {
        errno = EINVAL;
        return -1;
    }
    result = syscall(LINUX_SYS_SCHED_GETAFFINITY, pid, size, mask);
    if (result < 0) return -1;
    return 0;
}

int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *mask) {
    (void)pid;
    (void)size;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

int sched_setscheduler(pid_t pid, int policy,
                       const struct sched_param *parameter) {
    (void)pid;
    if (policy == SCHED_OTHER &&
        (parameter == 0 || parameter->sched_priority == 0)) return 0;
    errno = ENOSYS;
    return -1;
}

int sched_getscheduler(pid_t pid) {
    (void)pid;
    return SCHED_OTHER;
}

int sched_setparam(pid_t pid, const struct sched_param *parameter) {
    return sched_setscheduler(pid, SCHED_OTHER, parameter);
}

int sched_getparam(pid_t pid, struct sched_param *parameter) {
    (void)pid;
    if (parameter == 0) {
        errno = EINVAL;
        return -1;
    }
    parameter->sched_priority = 0;
    return 0;
}

int sched_get_priority_min(int policy) {
    if (policy != SCHED_OTHER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sched_get_priority_max(int policy) {
    if (policy != SCHED_OTHER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sigaltstack(const stack_t *stack, stack_t *old_stack) {
    if (old_stack != 0) {
        memset(old_stack, 0, sizeof(*old_stack));
        old_stack->ss_flags = SS_DISABLE;
    }
    if (stack != 0 && (stack->ss_flags & SS_DISABLE) == 0 &&
        (stack->ss_sp == 0 || stack->ss_size < MINSIGSTKSZ)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Linux eventfd 由内核兼容模式实现。 */
int eventfd(unsigned int initial_value, int flags) {
    long result = syscall(LINUX_SYS_EVENTFD2, initial_value, flags);
    if (result < 0) return -1;
    return __libc_adopt_linux_fd(
        (int)result,
        O_RDWR | ((flags & EFD_NONBLOCK) != 0 ? O_NONBLOCK : 0) |
            ((flags & EFD_CLOEXEC) != 0 ? O_CLOEXEC : 0));
}

int eventfd_read(int descriptor, eventfd_t *value) {
    long result;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    result = syscall(LINUX_SYS_READ, descriptor, value, sizeof(*value));
    return result == (long)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int descriptor, eventfd_t value) {
    long result = syscall(LINUX_SYS_WRITE, descriptor, value, sizeof(value));
    return result == (long)sizeof(value) ? 0 : -1;
}

/* Linux 兼容 epoll 入口。内核兼容模式已提供 Linux 系统调用号；libc 接口保持精简，
 * 使 Chromium 的 MessagePumpEpoll 和 WebRTC 能使用原生等待路径。 */
int epoll_create(int size) {
    long result;
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    result = syscall(LINUX_SYS_EPOLL_CREATE, size);
    return result < 0 ? -1 : __libc_adopt_linux_fd((int)result, O_RDWR);
}

int epoll_create1(int flags) {
    long result = syscall(LINUX_SYS_EPOLL_CREATE1, flags);
    if (result < 0) return -1;
    return __libc_adopt_linux_fd(
        (int)result, O_RDWR |
            ((flags & EPOLL_CLOEXEC) != 0 ? O_CLOEXEC : 0));
}

int epoll_ctl(int epoll_descriptor, int operation, int target_descriptor,
              struct epoll_event *event) {
    long result = syscall(LINUX_SYS_EPOLL_CTL, epoll_descriptor, operation,
                          target_descriptor, event);
    return result < 0 ? -1 : 0;
}

int epoll_wait(int epoll_descriptor, struct epoll_event *events,
               int maxevents, int timeout) {
    long result;
    if (events == 0 || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    result = syscall(LINUX_SYS_EPOLL_WAIT, epoll_descriptor, events,
                     maxevents, timeout);
    return result < 0 ? -1 : (int)result;
}

int epoll_pwait(int epoll_descriptor, struct epoll_event *events,
                int maxevents, int timeout, const void *signal_mask) {
    long result;
    if (events == 0 || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    result = syscall(LINUX_SYS_EPOLL_PWAIT, epoll_descriptor, events,
                     maxevents, timeout, signal_mask, sizeof(uint64_t));
    return result < 0 ? -1 : (int)result;
}

int timerfd_create(clockid_t clock_id, int flags) {
    (void)clock_id;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int timerfd_settime(int descriptor, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    (void)descriptor;
    (void)flags;
    (void)new_value;
    (void)old_value;
    errno = ENOSYS;
    return -1;
}

int timerfd_gettime(int descriptor, struct itimerspec *value) {
    (void)descriptor;
    (void)value;
    errno = ENOSYS;
    return -1;
}

int uname(struct utsname *name) {
    if (name == 0) {
        errno = EFAULT;
        return -1;
    }
    memset(name, 0, sizeof(*name));
    strncpy(name->sysname, "System", sizeof(name->sysname) - 1U);
    strncpy(name->nodename, "localhost", sizeof(name->nodename) - 1U);
    strncpy(name->release, "0.1", sizeof(name->release) - 1U);
    strncpy(name->version, "System Chromium", sizeof(name->version) - 1U);
    strncpy(name->machine, "x86_64", sizeof(name->machine) - 1U);
    return 0;
}

int sysinfo(struct sysinfo *info) {
    os_memory_info_t memory;
    uint64_t total_bytes;
    uint64_t free_bytes;
    if (info == 0) {
        errno = EFAULT;
        return -1;
    }
    if (__libc_query_memory_info(&memory) < 0) return -1;
    if (memory.page_size == 0U || memory.free_pages > memory.total_pages ||
        memory.total_pages > ULONG_MAX / memory.page_size) {
        errno = EOVERFLOW;
        return -1;
    }
    total_bytes = memory.total_pages * memory.page_size;
    free_bytes = memory.free_pages * memory.page_size;
    memset(info, 0, sizeof(*info));
    info->totalram = (unsigned long)total_bytes;
    info->freeram = (unsigned long)free_bytes;
    info->mem_unit = 1U;
    return 0;
}

int getpriority(int which, id_t who) {
    (void)who;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int setpriority(int which, id_t who, int priority) {
    (void)who;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
    if (priority != 0) {
        errno = ENOSYS;
        return -1;
    }
    return 0;
}

struct libc_locale {
    int category_mask;
};

static struct libc_locale g_c_locale_object = {LC_ALL_MASK};
static locale_t g_current_locale = &g_c_locale_object;

locale_t newlocale(int category_mask, const char *locale, locale_t base) {
    (void)category_mask;
    if (locale != 0 && strcmp(locale, "C") != 0 &&
        strcmp(locale, "POSIX") != 0 && locale[0] != '\0') {
        errno = ENOENT;
        return 0;
    }
    return base != 0 && base != LC_GLOBAL_LOCALE ? base :
           &g_c_locale_object;
}

void freelocale(locale_t locale) {
    (void)locale;
}

locale_t uselocale(locale_t locale) {
    locale_t previous = g_current_locale;
    if (locale != 0 && locale != LC_GLOBAL_LOCALE) g_current_locale = locale;
    return previous;
}

double strtod_l(const char *text, char **end, locale_t locale) {
    (void)locale;
    return strtod(text, end);
}

float strtof_l(const char *text, char **end, locale_t locale) {
    (void)locale;
    return strtof(text, end);
}

long double strtold_l(const char *text, char **end, locale_t locale) {
    (void)locale;
    return strtold(text, end);
}

int toupper_l(int value, locale_t locale) {
    (void)locale;
    return toupper(value);
}

int tolower_l(int value, locale_t locale) {
    (void)locale;
    return tolower(value);
}

int strcoll_l(const char *left, const char *right, locale_t locale) {
    (void)locale;
    return strcoll(left, right);
}

size_t strxfrm_l(char *destination, const char *source, size_t length,
                 locale_t locale) {
    (void)locale;
    return strxfrm(destination, source, length);
}

int iswctype_l(wint_t value, wctype_t type, locale_t locale) {
    (void)locale;
    return iswctype(value, type);
}

#define LIBC_WIDE_LOCALE_WRAPPER(name) \
    int name##_l(wint_t value, locale_t locale) { \
        (void)locale; \
        return name(value); \
    }

LIBC_WIDE_LOCALE_WRAPPER(iswspace)
LIBC_WIDE_LOCALE_WRAPPER(iswprint)
LIBC_WIDE_LOCALE_WRAPPER(iswcntrl)
LIBC_WIDE_LOCALE_WRAPPER(iswupper)
LIBC_WIDE_LOCALE_WRAPPER(iswlower)
LIBC_WIDE_LOCALE_WRAPPER(iswalpha)
LIBC_WIDE_LOCALE_WRAPPER(iswblank)
LIBC_WIDE_LOCALE_WRAPPER(iswdigit)
LIBC_WIDE_LOCALE_WRAPPER(iswpunct)
LIBC_WIDE_LOCALE_WRAPPER(iswxdigit)

#undef LIBC_WIDE_LOCALE_WRAPPER

wint_t towupper_l(wint_t value, locale_t locale) {
    (void)locale;
    return towupper(value);
}

wint_t towlower_l(wint_t value, locale_t locale) {
    (void)locale;
    return towlower(value);
}

int wcscoll_l(const wchar_t *left, const wchar_t *right, locale_t locale) {
    (void)locale;
    return wcscoll(left, right);
}

size_t wcsxfrm_l(wchar_t *destination, const wchar_t *source, size_t length,
                 locale_t locale) {
    (void)locale;
    return wcsxfrm(destination, source, length);
}

size_t strftime_l(char *buffer, size_t capacity, const char *format,
                  const struct tm *value, locale_t locale) {
    (void)locale;
    return strftime(buffer, capacity, format, value);
}

/* System 映像没有账户数据库。返回稳定的合成记录，使诊断和策略路径扩展能够继续，
 * 而无需将宿主 NSS 实现引入 freestanding 映像。 */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

static struct passwd g_libc_passwd;
static char g_libc_user_name[] = "user";
static char g_libc_user_password[] = "x";
static char g_libc_user_gecos[] = "System user";
static char g_libc_user_home[] = "/tmp";
static char g_libc_user_shell[] = "/usr/apps/gshell/gshell";

struct passwd *getpwuid(uid_t uid) {
    g_libc_passwd.pw_name = g_libc_user_name;
    g_libc_passwd.pw_passwd = g_libc_user_password;
    g_libc_passwd.pw_uid = uid;
    g_libc_passwd.pw_gid = 0U;
    g_libc_passwd.pw_gecos = g_libc_user_gecos;
    g_libc_passwd.pw_dir = g_libc_user_home;
    g_libc_passwd.pw_shell = g_libc_user_shell;
    return &g_libc_passwd;
}

struct passwd *getpwnam(const char *name) {
    if (name == 0 || strcmp(name, g_libc_user_name) != 0) {
        errno = ENOENT;
        return 0;
    }
    return getpwuid(0U);
}

static struct group g_libc_group;
static char g_libc_group_name[] = "root";
static char g_libc_group_password[] = "x";
static char *g_libc_group_members[] = {0};

struct group *getgrgid(gid_t gid) {
    g_libc_group.gr_name = g_libc_group_name;
    g_libc_group.gr_passwd = g_libc_group_password;
    g_libc_group.gr_gid = gid;
    g_libc_group.gr_mem = g_libc_group_members;
    return &g_libc_group;
}

struct group *getgrnam(const char *name) {
    if (name == 0 || (strcmp(name, "root") != 0 &&
                      strcmp(name, "users") != 0 &&
                      strcmp(name, "system") != 0)) {
        errno = ENOENT;
        return 0;
    }
    return getgrgid(0U);
}

/* 当前 System VFS 不强制文件所有权。将这些操作视为成功的空操作，
 * 避免可移植权限设置导致程序失败。 */
int chown(const char *path, uid_t owner, gid_t group) {
    (void)path;
    (void)owner;
    (void)group;
    return 0;
}

int fchown(int descriptor, uid_t owner, gid_t group) {
    (void)descriptor;
    (void)owner;
    (void)group;
    return 0;
}

int lchown(const char *path, uid_t owner, gid_t group) {
    return chown(path, owner, group);
}

/* 内核当前没有目录监视对象。返回 ENOSYS 可让 Chromium 选择轮询/后备路径，
 * 而不是在 epoll 中留下无效 fd。 */
int inotify_init(void) {
    errno = ENOSYS;
    return -1;
}

int inotify_init1(int flags) {
    (void)flags;
    return inotify_init();
}

int inotify_add_watch(int descriptor, const char *path, uint32_t mask) {
    (void)descriptor;
    (void)path;
    (void)mask;
    errno = ENOSYS;
    return -1;
}

int inotify_rm_watch(int descriptor, int watch_descriptor) {
    (void)descriptor;
    (void)watch_descriptor;
    errno = ENOSYS;
    return -1;
}

/* System 当前没有网络接口列表 ABI。返回 ENOSYS 是 Chromium 网络发现代码使用的
 * 文档化后备行为。 */
struct ifaddrs;

int getifaddrs(struct ifaddrs **interfaces) {
    if (interfaces != 0) *interfaces = 0;
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs *interfaces) {
    (void)interfaces;
}

int socketpair(int domain, int type, int protocol, int descriptors[2]) {
    int32_t linux_descriptors[2] = {-1, -1};
    int left;
    int right;
    long result;
    int open_flags = O_RDWR |
        ((type & SOCK_NONBLOCK) != 0 ? O_NONBLOCK : 0) |
        ((type & SOCK_CLOEXEC) != 0 ? O_CLOEXEC : 0);
    if (descriptors == 0) {
        errno = EFAULT;
        return -1;
    }
    result = __libc_linux_syscall6(
        LINUX_SYS_SOCKETPAIR, (uint64_t)(uint32_t)domain,
        (uint64_t)(uint32_t)type, (uint64_t)(uint32_t)protocol,
        (uint64_t)(uintptr_t)linux_descriptors, 0U, 0U);
    if (result < 0) {
        (void)__libc_status_result(result);
        return -1;
    }
    left = __libc_adopt_linux_fd(linux_descriptors[0], open_flags);
    if (left < 0) {
        (void)__libc_linux_syscall6(
            LINUX_SYS_CLOSE, (uint64_t)(uint32_t)linux_descriptors[1],
            0U, 0U, 0U, 0U, 0U);
        return -1;
    }
    right = __libc_adopt_linux_fd(linux_descriptors[1], open_flags);
    if (right < 0) {
        int saved_errno = errno;
        (void)close(left);
        errno = saved_errno;
        return -1;
    }
    descriptors[0] = left;
    descriptors[1] = right;
    return 0;
}

/* Linux 兼容模式尚未提供文件系统时间戳修改功能。
 * 保留此 ABI，使仅需保留元数据的调用方仍可继续运行。 */
int utime(const char *path, const void *times) {
    (void)path;
    (void)times;
    return 0;
}

/* Dawn/Tint 的终端探测是可选功能。报告当前不是终端，而不是解引用 System 未实现的
 * 宿主 termios 布局。 */
int tcgetattr(int descriptor, void *attributes) {
    (void)descriptor;
    (void)attributes;
    errno = ENOTTY;
    return -1;
}

int tcsetattr(int descriptor, int action, const void *attributes) {
    (void)descriptor;
    (void)action;
    (void)attributes;
    errno = ENOTTY;
    return -1;
}

/* 此处有意将解析器状态设为不透明类型。DNS 配置读取器只需要生命周期接口；实际查询
 * 使用 System netdb。
 *
 * 不要把调用方提供的状态初始化为这个旧版私有 4096 字节缓冲区。Chromium 传入的是
 * 更小的 glibc 兼容 struct __res_state。通过该指针写入私有缓冲区大小会破坏对象后方
 * 紧邻的分配器元数据。 */
static unsigned char g_libc_resolver_state[4096];

void *__res_state(void) {
    return &g_libc_resolver_state;
}

int __res_ninit(void *state) {
    if (state == 0) {
        errno = EINVAL;
        return -1;
    }
    /* 调用方拥有符合 ABI 大小的状态，并已将其清零。 */
    return 0;
}

void __res_nclose(void *state) {
    (void)state;
}

/* 当前 freestanding 加载器不支持可选的宿主风格动态加载。返回常规查找失败，
 * 可让 Chromium 的功能探测正常关闭 Pulse/Vulkan/speech-dispatcher。 */
static const char g_libc_dl_error[] = "dynamic loading is unavailable";

void *dlopen(const char *file, int mode) {
    (void)file;
    (void)mode;
    return 0;
}

int dlclose(void *handle) {
    if (handle == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void *dlsym(void *handle, const char *name) {
    (void)handle;
    /* Chromium 的 Linux ScopedFD 封装会在启动时解析一次 RTLD_NEXT 的 "close"，并通过
     * 返回的指针转发后续所有 close 调用。System 将 libc 链接进同一映像，而非使用宿主
     * 加载器，因此此处为该查找公开本地 POSIX 入口。 */
    if (name != 0 && strcmp(name, "close") == 0) {
        return (void *)(uintptr_t)close;
    }
    /* Chromium 的沙箱 libc 拦截器使用 RTLD_NEXT，访问其 getaddrinfo 封装下方的解析器。
     * System 没有独立的宿主 libc，因此将该查找解析到本地 POSIX 实现。 */
    if (name != 0 && strcmp(name, "getaddrinfo") == 0) {
        return (void *)(uintptr_t)getaddrinfo;
    }
    /* Chromium 的沙箱 libc 拦截器会在启动时通过 RTLD_NEXT 查询宿主的 localtime 系列函数。
     * System 使用 UTC，且已提供对应的 gmtime 实现；返回这些符号可避开拦截器的错误日志
     * 路径，避免其持有 pthread_once 守卫时递归调用 localtime。 */
    if (name != 0 &&
        (strcmp(name, "localtime") == 0 ||
         strcmp(name, "localtime64") == 0)) {
        return (void *)(uintptr_t)gmtime;
    }
    if (name != 0 &&
        (strcmp(name, "localtime_r") == 0 ||
         strcmp(name, "localtime64_r") == 0)) {
        return (void *)(uintptr_t)gmtime_r;
    }
    return 0;
}

const char *dlerror(void) {
    return g_libc_dl_error;
}

int dladdr(const void *address, Dl_info *info) {
    (void)address;
    if (info != 0) memset(info, 0, sizeof(*info));
    return 0;
}

int bcmp(const void *left, const void *right, size_t length) {
    return memcmp(left, right, length);
}

char *dgettext(const char *domain, const char *message) {
    (void)domain;
    return (char *)(message == 0 ? "" : message);
}

time_t timegm(struct tm *value) {
    /* System 时区固定为 UTC，因此 mktime 已具备 timegm 语义。 */
    return mktime(value);
}

/* C++ 局部静态变量 ABI：编译器会直接检查首字节。在构造完成前，该字节必须保持为零。
 * 将初始化锁保存在第二字节；通过 release 存储将首字节设为 1，以发布对象。 */
int __cxa_guard_acquire(uint64_t *guard) {
    if (guard == 0) return 0;
    for (;;) {
        uint64_t current = __atomic_load_n(guard, __ATOMIC_ACQUIRE);
        if ((current & 1U) != 0U) return 0;
        uint64_t expected = 0U;
        if (__atomic_compare_exchange_n(guard, &expected, 0x100U, false,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) return 1;
        sched_yield();
    }
}

void __cxa_guard_release(uint64_t *guard) {
    if (guard != 0) {
        __atomic_store_n(guard, 1U, __ATOMIC_RELEASE);
    }
}

void __cxa_guard_abort(uint64_t *guard) {
    if (guard != 0) __atomic_store_n(guard, 0U, __ATOMIC_RELEASE);
}

__attribute__((weak)) void __cxa_pure_virtual(void) {
    _exit(134);
}

int __cxa_atexit(void (*function)(void *), void *argument,
                 void *dso_handle) {
    (void)function;
    (void)argument;
    (void)dso_handle;
    return 0;
}

void __cxa_finalize(void *dso_handle) {
    (void)dso_handle;
}

void openlog(const char *ident, int option, int facility) {
    (void)ident;
    (void)option;
    (void)facility;
}

void syslog(int priority, const char *format, ...) {
    (void)priority;
    (void)format;
}

void closelog(void) {}

int getrlimit(int resource, struct rlimit *limit) {
    if (limit == 0) {
        errno = EFAULT;
        return -1;
    }
    return syscall(__NR_prlimit64, (uint64_t)0U, (uint64_t)resource,
                   (uint64_t)0U, (uint64_t)(uintptr_t)limit) < 0 ? -1 : 0;
}

int setrlimit(int resource, const struct rlimit *limit) {
    (void)resource;
    if (limit == 0) {
        errno = EFAULT;
        return -1;
    }
    return 0;
}

int getrusage(int who, struct rusage *usage) {
    if (usage == 0 || (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    return 0;
}

unsigned int if_nametoindex(const char *ifname) {
    if (ifname != 0 && strcmp(ifname, "lo") == 0) return 1U;
    errno = ENODEV;
    return 0U;
}

char *if_indextoname(unsigned int ifindex, char *ifname) {
    if (ifname == 0) {
        errno = EFAULT;
        return 0;
    }
    if (ifindex != 1U) {
        errno = ENXIO;
        return 0;
    }
    strcpy(ifname, "lo");
    return ifname;
}

int flock(int descriptor, int operation) {
    (void)descriptor;
    (void)operation;
    return 0;
}

static char g_libc_backtrace_symbol[] = "<System backtrace unavailable>";

char **backtrace_symbols(void *const *buffer, int count) {
    char **symbols;
    int index;
    (void)buffer;
    if (count <= 0) return 0;
    symbols = (char **)calloc((size_t)count, sizeof(*symbols));
    if (symbols == 0) return 0;
    for (index = 0; index < count; ++index) {
        symbols[index] = g_libc_backtrace_symbol;
    }
    return symbols;
}

/* Chromium 在确定可丢弃内存大小时会查询 /dev/shm 的容量。System VFS 尚未公开 statfs
 * ABI，但返回有界的文件系统容量估算值可在 VFS 提供真实卷统计前维持现有行为。注意区分
 * 两种 x86-64 布局：statfs 以 f_type 开始，而 statvfs 以 f_bsize 开始。 */
typedef struct libc_statvfs_abi {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
    int f_spare[6];
} libc_statvfs_abi_t;

typedef struct libc_statfs_abi {
    long f_type;
    long f_bsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    int f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
} libc_statfs_abi_t;

static int fill_libc_statvfs(void *status) {
    libc_statvfs_abi_t *value = (libc_statvfs_abi_t *)status;
    if (value == 0) {
        errno = EFAULT;
        return -1;
    }
    memset(value, 0, sizeof(*value));
    value->f_bsize = 4096UL;
    value->f_frsize = 4096UL;
    value->f_blocks = 2097152UL; /* 总计 8 GiB */
    value->f_bfree = 1048576UL;
    value->f_bavail = 1048576UL; /* 可用 4 GiB */
    value->f_files = 65536UL;
    value->f_ffree = 65535UL;
    value->f_favail = 65535UL;
    value->f_namemax = 255UL;
    return 0;
}

static int fill_libc_statfs(void *status) {
    libc_statfs_abi_t *value = (libc_statfs_abi_t *)status;
    libc_statvfs_abi_t capacity;
    if (value == 0) {
        errno = EFAULT;
        return -1;
    }
    (void)fill_libc_statvfs(&capacity);
    memset(value, 0, sizeof(*value));
    /* 未查询到真实 VFS 卷信息时，不声明文件系统类型。 */
    value->f_bsize = (long)capacity.f_bsize;
    value->f_blocks = capacity.f_blocks;
    value->f_bfree = capacity.f_bfree;
    value->f_bavail = capacity.f_bavail;
    value->f_files = capacity.f_files;
    value->f_ffree = capacity.f_ffree;
    value->f_namelen = (long)capacity.f_namemax;
    value->f_frsize = (long)capacity.f_frsize;
    return 0;
}

int statvfs(const char *path, void *status) {
    if (path == 0) {
        errno = EINVAL;
        return -1;
    }
    return fill_libc_statvfs(status);
}

int statfs(const char *path, void *status) {
    if (path == 0) {
        errno = EINVAL;
        return -1;
    }
    return fill_libc_statfs(status);
}

int fstatfs(int descriptor, void *status) {
    (void)descriptor;
    return fill_libc_statfs(status);
}

ssize_t sendfile(int output_descriptor, int input_descriptor, off_t *offset,
             size_t count) {
    unsigned char buffer[16384];
    size_t total = 0U;
    off_t position;
    if (count == 0U) return 0;
    if (count > (size_t)SSIZE_MAX) count = (size_t)SSIZE_MAX;
    /* 使用定位读取可避免短写或遇到 EAGAIN 时消耗尚未发送的输入数据。
     * 未指定偏移时，每次写入后提交输入游标。 */
    position = offset != 0 ? *offset : lseek(input_descriptor, 0, SEEK_CUR);
    if (position < 0) {
        if (offset != 0) errno = EINVAL;
        return -1;
    }
    while (total < count) {
        size_t request = count - total;
        ssize_t read_count;
        size_t sent = 0U;
        if (request > sizeof(buffer)) request = sizeof(buffer);
        if ((uint64_t)position == INT64_MAX) {
            errno = EOVERFLOW;
            return total != 0U ? (ssize_t)total : -1;
        }
        if (request > (uint64_t)INT64_MAX - (uint64_t)position)
            request = (size_t)((uint64_t)INT64_MAX - (uint64_t)position);
        do {
            read_count = pread(input_descriptor, buffer, request, position);
        } while (read_count < 0 && errno == EINTR);
        if (read_count == 0) return (ssize_t)total;
        if (read_count < 0) return total != 0U ? (ssize_t)total : -1;
        if ((size_t)read_count > request) {
            errno = EIO;
            return total != 0U ? (ssize_t)total : -1;
        }
        while (sent < (size_t)read_count) {
            ssize_t written;
            do {
                written = write(output_descriptor, buffer + sent,
                                (size_t)read_count - sent);
            } while (written < 0 && errno == EINTR);
            if (written <= 0 || (size_t)written > (size_t)read_count - sent) {
                if (written >= 0) errno = EIO;
                return total != 0U ? (ssize_t)total : -1;
            }
            sent += (size_t)written;
            total += (size_t)written;
            position += written;
            if (offset != 0) *offset = position;
            else if (lseek(input_descriptor, position, SEEK_SET) < 0)
                return (ssize_t)total;
        }
    }
    return (ssize_t)total;
}

/* 即使 Chromium 不会调用所有操作，Rust 标准库仍会在最终 rlib 中保留完整的 Unix
 * 进程/文件系统 ABI。System 上这些入口有意采用保守行为：单用户映像中的身份更改和
 * chroot 是无害空操作；在原生进程管理器实现 posix_spawn 前，进程创建记账返回 ENOSYS。 */
int setuid(uid_t uid) {
    (void)uid;
    return 0;
}

int setgid(gid_t gid) {
    (void)gid;
    return 0;
}

int setgroups(size_t group_count, const gid_t *groups) {
    (void)group_count;
    (void)groups;
    return 0;
}

int chroot(const char *path) {
    (void)path;
    return 0;
}

int killpg(pid_t process_group, int signal_number) {
    return kill((pid_t)-process_group, signal_number);
}

void *mmap64(void *address, size_t length, int protection, int flags,
             int descriptor, off_t offset) {
    return mmap(address, length, protection, flags, descriptor, offset);
}

/* System VFS 尚未提供零拷贝 splice 操作。 */
ssize_t splice(int input_descriptor, off_t *input_offset,
               int output_descriptor, off_t *output_offset, size_t length,
               unsigned int flags) {
    (void)input_descriptor;
    (void)input_offset;
    (void)output_descriptor;
    (void)output_offset;
    (void)length;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int posix_spawnattr_init(void *attributes) {
    (void)attributes;
    return 0;
}

int posix_spawnattr_destroy(void *attributes) {
    (void)attributes;
    return 0;
}

int posix_spawnattr_setflags(void *attributes, short flags) {
    (void)attributes;
    (void)flags;
    return 0;
}

int posix_spawnattr_setpgroup(void *attributes, pid_t process_group) {
    (void)attributes;
    (void)process_group;
    return 0;
}

int posix_spawnattr_setsigdefault(void *attributes, const void *signals) {
    (void)attributes;
    (void)signals;
    return 0;
}

int posix_spawn_file_actions_init(void *actions) {
    (void)actions;
    return 0;
}

int posix_spawn_file_actions_destroy(void *actions) {
    (void)actions;
    return 0;
}

int posix_spawn_file_actions_adddup2(void *actions, int source_descriptor,
                                     int target_descriptor) {
    (void)actions;
    (void)source_descriptor;
    (void)target_descriptor;
    return 0;
}

int posix_spawnp(pid_t *process_id, const char *path, const void *actions,
                 const void *attributes, char *const argv[],
                 char *const environment[]) {
    (void)process_id;
    (void)path;
    (void)actions;
    (void)attributes;
    (void)argv;
    (void)environment;
    return ENOSYS;
}

int __xpg_strerror_r(int error_number, char *buffer, size_t length) {
    return strerror_r(error_number, buffer, length) == 0 ? 0 : ERANGE;
}

int __res_init(void) {
    return 0;
}

unsigned long getauxval(unsigned long type) {
    (void)type;
    errno = ENOENT;
    return 0UL;
}

const char *gnu_get_libc_version(void) {
    return "System libc";
}

int dl_iterate_phdr(int (*callback)(void *, size_t, void *), void *data) {
    (void)callback;
    (void)data;
    return 0;
}

int stat64(const char *path, struct stat *status) {
    return stat(path, status);
}

int fstat64(int descriptor, struct stat *status) {
    return fstat(descriptor, status);
}

int lstat64(const char *path, struct stat *status) {
    return lstat(path, status);
}

struct dirent *readdir64(DIR *directory) {
    return readdir(directory);
}

int linkat(int old_directory_descriptor, const char *old_path,
           int new_directory_descriptor, const char *new_path, int flags) {
    (void)old_directory_descriptor;
    (void)old_path;
    (void)new_directory_descriptor;
    (void)new_path;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int fstatat64(int directory_descriptor, const char *path,
              struct stat *status, int flags) {
    return fstatat(directory_descriptor, path, status, flags);
}

int sendfile64(int output_descriptor, int input_descriptor, off_t *offset,
               size_t count) {
    return sendfile(output_descriptor, input_descriptor, offset, count);
}

int futimens(int descriptor, const struct timespec times[2]) {
    (void)descriptor;
    (void)times;
    return 0;
}

int utimensat(int directory_descriptor, const char *path,
              const struct timespec times[2], int flags) {
    (void)directory_descriptor;
    (void)path;
    (void)times;
    (void)flags;
    return 0;
}

int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

/* 目标平台没有原生半精度转换指令时，Rust 浮点格式化和 Skia 颜色路径可能会请求编译器
 * 辅助函数。转换过程仅使用整数运算，避免此独立运行的 libc 引入宿主浮点运行时。 */
uint16_t __truncsfhf2(float value) {
    union {
        float value;
        uint32_t bits;
    } input;
    uint32_t sign;
    uint32_t exponent;
    uint32_t mantissa;
    int half_exponent;

    input.value = value;
    sign = (input.bits >> 16) & 0x8000U;
    exponent = (input.bits >> 23) & 0xffU;
    mantissa = input.bits & 0x7fffffU;
    if (exponent == 0xffU) {
        return (uint16_t)(sign | 0x7c00U |
                          (mantissa == 0U ? 0U : 0x0200U));
    }
    half_exponent = (int)exponent - 127 + 15;
    if (half_exponent >= 31) return (uint16_t)(sign | 0x7c00U);
    if (half_exponent <= 0) {
        uint32_t shifted;
        unsigned int shift;
        mantissa |= 0x800000U;
        if (half_exponent < -10) return (uint16_t)sign;
        shift = (unsigned int)(14 - half_exponent);
        shifted = mantissa >> shift;
        if ((mantissa & (1U << (shift - 1U))) != 0U) ++shifted;
        return (uint16_t)(sign | shifted);
    }
    mantissa >>= 13;
    if ((input.bits & 0x1000U) != 0U) ++mantissa;
    if (mantissa == 0x400U) {
        mantissa = 0U;
        ++half_exponent;
        if (half_exponent >= 31) return (uint16_t)(sign | 0x7c00U);
    }
    return (uint16_t)(sign | ((uint32_t)half_exponent << 10) | mantissa);
}

static int copy_passwd_record(uid_t uid, struct passwd *destination,
                              char *buffer, size_t buffer_length,
                              struct passwd **result) {
    struct passwd *source;
    size_t name_length;
    size_t password_length;
    size_t gecos_length;
    size_t home_length;
    size_t shell_length;
    size_t required;
    char *cursor;

    if (destination == 0 || result == 0) return EINVAL;
    *result = 0;
    source = getpwuid(uid);
    if (source == 0) return errno == 0 ? ENOENT : errno;
    name_length = strlen(source->pw_name) + 1U;
    password_length = strlen(source->pw_passwd) + 1U;
    gecos_length = strlen(source->pw_gecos) + 1U;
    home_length = strlen(source->pw_dir) + 1U;
    shell_length = strlen(source->pw_shell) + 1U;
    required = name_length + password_length + gecos_length + home_length +
               shell_length;
    if (buffer == 0 || buffer_length < required) return ERANGE;
    *destination = *source;
    cursor = buffer;
    destination->pw_name = cursor;
    memcpy(cursor, source->pw_name, name_length);
    cursor += name_length;
    destination->pw_passwd = cursor;
    memcpy(cursor, source->pw_passwd, password_length);
    cursor += password_length;
    destination->pw_gecos = cursor;
    memcpy(cursor, source->pw_gecos, gecos_length);
    cursor += gecos_length;
    destination->pw_dir = cursor;
    memcpy(cursor, source->pw_dir, home_length);
    cursor += home_length;
    destination->pw_shell = cursor;
    memcpy(cursor, source->pw_shell, shell_length);
    *result = destination;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *password, char *buffer,
               size_t buffer_length, struct passwd **result) {
    return copy_passwd_record(uid, password, buffer, buffer_length, result);
}
