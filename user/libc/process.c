#include "libc/libc.h"

#include <limits.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define WAITID_CACHE_LIMIT 16U

typedef struct waitid_cached_status {
    pid_t pid;
    int status;
    bool used;
} waitid_cached_status_t;

static waitid_cached_status_t g_waitid_cache[WAITID_CACHE_LIMIT];

static int waitid_cache_find(pid_t pid, bool any) {
    for (unsigned int index = 0U; index < WAITID_CACHE_LIMIT; ++index) {
        if (g_waitid_cache[index].used &&
            (any || g_waitid_cache[index].pid == pid)) {
            return (int)index;
        }
    }
    return -1;
}

static void waitid_fill_siginfo(siginfo_t *info, pid_t pid, int status) {
    memset(info, 0, sizeof(*info));
    info->si_signo = SIGCHLD;
    info->si_pid = pid;
    info->si_uid = 0U;
    if (WIFEXITED(status)) {
        info->si_code = CLD_EXITED;
        info->si_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        info->si_code = CLD_KILLED;
        info->si_status = WTERMSIG(status);
    } else if (WIFSTOPPED(status)) {
        info->si_code = CLD_STOPPED;
        info->si_status = WSTOPSIG(status);
    }
}

#define LIBC_PATH_LIMIT 256U
#define EXEC_ARGUMENT_LIMIT 32U

static os_exec_fd_map_t g_exec_descriptor_map;

pid_t fork(void) {
    int64_t status;
    __libc_atfork_prepare();
    __libc_fd_table_fork_prepare();
    status = __libc_native_syscall6(OS_SYS_PROCESS_FORK,
                             0U, 0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        __libc_fd_table_fork_parent();
        __libc_atfork_parent();
        return (pid_t)__libc_status_result(status);
    }
    if (status == 0) {
        __libc_fd_table_fork_child();
        __libc_atfork_child();
    } else {
        __libc_fd_table_fork_parent();
        __libc_atfork_parent();
    }
    if (status > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (pid_t)status;
}

pid_t waitpid(pid_t pid, int *status_pointer, int options) {
    int64_t status = __libc_native_syscall6(
        OS_SYS_PROCESS_WAIT, (uint64_t)(int64_t)pid, (uint64_t)(uint32_t)options,
        (uint64_t)(uintptr_t)status_pointer, 0U, 0U, 0U);
    if (status < 0) return (pid_t)__libc_status_result(status);
    if (status > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (pid_t)status;
}

pid_t wait(int *status_pointer) {
    return waitpid((pid_t)-1, status_pointer, 0);
}

pid_t wait4(pid_t pid, int *status_pointer, int options,
            struct rusage *usage) {
    pid_t result = waitpid(pid, status_pointer, options);
    if (result >= 0 && usage != 0) {
        memset(usage, 0, sizeof(*usage));
    }
    return result;
}

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
    pid_t wait_pid;
    int status = 0;
    pid_t result;
    int cached_index;
    bool nohang = (options & WNOHANG) != 0;
    bool nowait = (options & WNOWAIT) != 0;

    if (infop == 0 || (options & ~(WEXITED | WSTOPPED | WCONTINUED |
                                   WNOHANG | WNOWAIT)) != 0 ||
        (options & (WEXITED | WSTOPPED | WCONTINUED)) == 0 ||
        (idtype != P_ALL && idtype != P_PID) ||
        (idtype == P_PID && (pid_t)id <= 0)) {
        errno = EINVAL;
        return -1;
    }
    /* System 当前只发布子进程的终止状态。 */
    if ((options & (WSTOPPED | WCONTINUED)) != 0 &&
        (options & WEXITED) == 0) {
        errno = ENOTSUP;
        return -1;
    }

    wait_pid = idtype == P_PID ? (pid_t)id : (pid_t)-1;
    cached_index = waitid_cache_find(wait_pid, idtype == P_ALL);
    if (cached_index >= 0) {
        status = g_waitid_cache[(unsigned int)cached_index].status;
        result = g_waitid_cache[(unsigned int)cached_index].pid;
        if (!nowait) g_waitid_cache[(unsigned int)cached_index].used = false;
    } else {
        result = waitpid(wait_pid, &status, nohang ? WNOHANG : 0);
        if (result < 0) return -1;
        if (result == 0) {
            memset(infop, 0, sizeof(*infop));
            return 0;
        }
        if (nowait) {
            unsigned int slot;
            for (slot = 0U; slot < WAITID_CACHE_LIMIT; ++slot) {
                if (!g_waitid_cache[slot].used) break;
            }
            if (slot == WAITID_CACHE_LIMIT) {
                errno = EAGAIN;
                return -1;
            }
            g_waitid_cache[slot].pid = result;
            g_waitid_cache[slot].status = status;
            g_waitid_cache[slot].used = true;
        }
    }
    waitid_fill_siginfo(infop, result, status);
    return 0;
}

static int exec_with_environment(const char *path, char *const argv[],
                                 char *const envp[]) {
    char absolute_path[LIBC_PATH_LIMIT];
    int64_t status;
    if (__libc_make_absolute_path(path, absolute_path) < 0) return -1;
    __libc_build_exec_fd_map(&g_exec_descriptor_map);
    status = __libc_native_syscall6(OS_SYS_PROCESS_EXEC,
                             (uint64_t)(uintptr_t)absolute_path,
                             (uint64_t)(uintptr_t)argv,
                             (uint64_t)(uintptr_t)(envp != 0 ? envp : environ),
                             (uint64_t)(uintptr_t)&g_exec_descriptor_map,
                             0U, 0U);
    if (status < 0) return __libc_status_result(status);
    errno = EIO;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return exec_with_environment(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return exec_with_environment(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    const char *search_path;
    char candidate[LIBC_PATH_LIMIT];
    const char *component;
    int last_error = ENOENT;

    if (file == 0 || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/') != 0) return execv(file, argv);
    search_path = getenv("PATH");
    if (search_path == 0 || search_path[0] == '\0') search_path = "/sbin:/bin";
    if (strlen(search_path) >= LIBC_PATH_LIMIT) {
        errno = ENAMETOOLONG;
        return -1;
    }
    component = search_path;
    for (;;) {
        const char *separator = strchr(component, ':');
        size_t component_length = separator == 0 ? strlen(component) :
                                   (size_t)(separator - component);
        size_t file_length = strlen(file);
        if (component_length + file_length + 2U < sizeof(candidate)) {
            if (component_length == 0U) {
                strcpy(candidate, file);
            } else {
                memcpy(candidate, component, component_length);
                candidate[component_length] = '\0';
                if (candidate[component_length - 1U] != '/') {
                    candidate[component_length++] = '/';
                    candidate[component_length] = '\0';
                }
                strcat(candidate, file);
            }
            if (execv(candidate, argv) < 0) {
                last_error = errno;
                if (last_error != ENOENT && last_error != ENOTDIR) return -1;
            }
        }
        if (separator == 0) break;
        component = separator + 1;
    }
    errno = last_error;
    return -1;
}

static int exec_list(const char *path, const char *argument0,
                     va_list arguments, char *const *environment,
                     bool search_path) {
    const char *argv[EXEC_ARGUMENT_LIMIT + 1U];
    const char *argument = argument0;
    size_t count = 0U;
    if (argument == 0) {
        errno = EINVAL;
        return -1;
    }
    while (argument != 0 && count < EXEC_ARGUMENT_LIMIT) {
        argv[count++] = argument;
        argument = va_arg(arguments, const char *);
    }
    if (argument != 0) {
        errno = E2BIG;
        return -1;
    }
    argv[count] = 0;
    if (search_path) return execvp(path, (char *const *)argv);
    return exec_with_environment(path, (char *const *)argv, environment);
}

int execl(const char *path, const char *argument0, ...) {
    va_list arguments;
    int result;
    va_start(arguments, argument0);
    result = exec_list(path, argument0, arguments, environ, false);
    va_end(arguments);
    return result;
}

int execlp(const char *file, const char *argument0, ...) {
    va_list arguments;
    int result;
    va_start(arguments, argument0);
    result = exec_list(file, argument0, arguments, environ, true);
    va_end(arguments);
    return result;
}

int execle(const char *path, const char *argument0, ...) {
    const char *argv[EXEC_ARGUMENT_LIMIT + 1U];
    const char *argument = argument0;
    char *const *environment;
    va_list arguments;
    size_t count = 0U;

    if (argument0 == 0) {
        errno = EINVAL;
        return -1;
    }
    va_start(arguments, argument0);
    while (argument != 0 && count < EXEC_ARGUMENT_LIMIT) {
        argv[count++] = argument;
        argument = va_arg(arguments, const char *);
    }
    if (argument != 0) {
        va_end(arguments);
        errno = E2BIG;
        return -1;
    }
    environment = va_arg(arguments, char *const *);
    va_end(arguments);
    argv[count] = 0;
    return exec_with_environment(path, (char *const *)argv, environment);
}
