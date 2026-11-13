#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <features.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <gnu/libc-version.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__GLIBC__) || __GLIBC__ != 2 || __GLIBC_MINOR__ != 36
#error "Chrome Vulkan compatibility probe requires the pinned guest glibc headers"
#endif

static __thread int tls_marker = 42;
extern void *__libc_stack_end;
extern double _Complex __muldc3(double, double, double, double);
extern double _Complex __divdc3(double, double, double, double);
extern float _Complex __mulsc3(float, float, float, float);

static void *worker(void *unused) {
    (void)unused;
    assert(tls_marker == 42);
    tls_marker = 100;
    return (void *)1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--exec-child") == 0) {
        int channel = atoi(argv[2]), old_channel = atoi(argv[3]);
        if (channel != 6 || old_channel < 0 || old_channel == channel ||
            strcmp(gnu_get_libc_version(), "2.36") != 0 || tls_marker != 42)
            return 8;
        if (fcntl(channel, F_GETFD) != 0) return 9;
        errno = 0;
        if (fcntl(old_channel, F_GETFD) != -1 || errno != EBADF) return 10;
        struct pollfd readable = {channel, POLLIN, 0};
        if (poll(&readable, 1, 5000) != 1) return 11;
        int value = 0;
        if (recv(channel, &value, sizeof(value), 0) != sizeof(value) || value != 42)
            return 12;
        value = 200;
        if (send(channel, &value, sizeof(value), 0) != sizeof(value)) return 13;
        close(channel);
        return 7;
    }
    if (argc == 3 && strcmp(argv[1], "--log") == 0) {
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
            dup2(fd, STDERR_FILENO) < 0) return 2;
        if (fd > STDERR_FILENO) close(fd);
    } else if (argc != 1) {
        return 2;
    }
    setvbuf(stdout, 0, _IONBF, 0);
    puts("CHROME_GLIBC_START");
    assert(strcmp(gnu_get_libc_version(), "2.36") == 0);
    struct sysinfo system_info;
    memset(&system_info, 0xa5, sizeof(system_info));
    assert(sizeof(system_info) == 112 && sysinfo(&system_info) == 0);
    long physical_pages = sysconf(_SC_PHYS_PAGES);
    long available_pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    assert(physical_pages > 0 && available_pages >= 0 &&
           available_pages <= physical_pages && page_size > 0);
    assert((uint64_t)physical_pages <= UINT64_MAX / (uint64_t)page_size);
    assert((uint64_t)physical_pages * (uint64_t)page_size ==
           (uint64_t)system_info.totalram * system_info.mem_unit);
    assert(system_info.freeram <= system_info.totalram && system_info.mem_unit != 0);
    puts("CHROME_GLIBC_SYSINFO_PASS");
    struct timespec thread_before, thread_after, process_time;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_before) == 0);
    volatile uint64_t busy = 1;
    for (unsigned i = 0; i < 5000000; ++i) busy = busy * 3 + i;
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_after) == 0);
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process_time) == 0);
    assert(thread_after.tv_sec > thread_before.tv_sec ||
           (thread_after.tv_sec == thread_before.tv_sec &&
            thread_after.tv_nsec >= thread_before.tv_nsec));
    assert(thread_after.tv_nsec >= 0 && thread_after.tv_nsec < 1000000000L &&
           process_time.tv_nsec >= 0 && process_time.tv_nsec < 1000000000L);
    puts("CHROME_GLIBC_CPU_CLOCK_PASS");
    pthread_t thread;
    void *result = 0;
    assert(pthread_create(&thread, 0, worker, 0) == 0);
    assert(pthread_join(thread, &result) == 0);
    assert(result == (void *)1 && tls_marker == 42);
    puts("CHROME_GLIBC_THREAD_TLS_PASS");
    void *library = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    assert(library);
    double (*cosine)(double) = (double (*)(double))dlsym(library, "cos");
    assert(cosine && cosine(0.0) == 1.0);
    assert(dlclose(library) == 0);
    puts("CHROME_GLIBC_DLOPEN_PASS");
    /* 浏览器的图像/音频计算内核会使用 compiler-rt 复数辅助函数。
     * 使用 volatile 输入，避免此检查被常量折叠取代。 */
    volatile double real = 1.0, imaginary = 2.0, other_real = 3.0, other_imaginary = -4.0;
    double _Complex left = real + imaginary * I;
    double _Complex right = other_real + other_imaginary * I;
    double _Complex product = left * right, quotient = product / right;
    assert(fabs(creal(product) - 11.0) < 1e-8 && fabs(cimag(product) - 2.0) < 1e-8);
    assert(fabs(creal(quotient) - real) < 1e-8 && fabs(cimag(quotient) - imaginary) < 1e-8);
    product = __muldc3(real, imaginary, other_real, other_imaginary);
    quotient = __divdc3(creal(product), cimag(product), other_real, other_imaginary);
    assert(fabs(creal(product) - 11.0) < 1e-8 && fabs(cimag(product) - 2.0) < 1e-8);
    assert(fabs(creal(quotient) - real) < 1e-8 && fabs(cimag(quotient) - imaginary) < 1e-8);
    volatile float single_real = 1.0f, single_imaginary = 2.0f;
    float _Complex single = single_real + single_imaginary * I;
    float _Complex squared = single * single;
    assert(crealf(squared) == -3.0f && cimagf(squared) == 4.0f);
    squared = __mulsc3(single_real, single_imaginary, single_real, single_imaginary);
    assert(crealf(squared) == -3.0f && cimagf(squared) == 4.0f);
    assert(__libc_stack_end && (uintptr_t)__libc_stack_end > (uintptr_t)&thread);
    puts("CHROME_GLIBC_COMPILER_RUNTIME_PASS");
    const char *fs_root = getenv("CHROME_GLIBC_PROBE_FS_ROOT");
    if (!fs_root) fs_root = "/tmp";
    char directory[192], nested[224];
    assert(snprintf(directory, sizeof(directory), "%s/chrome-runtime-%ld", fs_root,
                    (long)getpid()) > 0);
    if (mkdir(directory, 0700) != 0) {
        fprintf(stderr, "CHROME_GLIBC_MKDIR_FAIL errno=%d\n", errno);
        return 1;
    }
    assert(snprintf(nested, sizeof(nested), "%s/nested", directory) > 0);
    assert(mkdirat(AT_FDCWD, nested, 0700) == 0);
    int directory_fd = open(nested, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat directory_info;
    assert(directory_fd >= 0 && fstat(directory_fd, &directory_info) == 0 &&
           S_ISDIR(directory_info.st_mode));
    assert(close(directory_fd) == 0);
    char resolved_directory[4096];
    if (!realpath(directory, resolved_directory)) {
        fprintf(stderr, "CHROME_GLIBC_REALPATH_FAIL errno=%d\n", errno);
        return 1;
    }
    assert(strcmp(directory, resolved_directory) == 0);
    errno = 0;
    assert(mkdir(directory, 0700) == -1 && errno == EEXIST);
    puts("CHROME_GLIBC_DIRECTORY_PASS");
    char link_path[256], dangling[256], loop[256];
    assert(snprintf(link_path, sizeof(link_path), "%s/alias", directory) > 0);
    assert(symlink("nested", link_path) == 0);
    struct stat link_info;
    assert(lstat(link_path, &link_info) == 0 && S_ISLNK(link_info.st_mode) &&
           link_info.st_size == 6);
    char link_text[16]; memset(link_text, 'X', sizeof(link_text));
    assert(readlink(link_path, link_text, 3) == 3 &&
           memcmp(link_text, "nes", 3) == 0 && link_text[3] == 'X');
    errno = 0;
    assert(symlink("replacement", link_path) == -1 && errno == EEXIST);
    assert(stat(link_path, &link_info) == 0 && S_ISDIR(link_info.st_mode));
    assert(realpath(link_path, resolved_directory) == resolved_directory &&
           strcmp(resolved_directory, nested) == 0);
    assert(snprintf(dangling, sizeof(dangling), "%s/dangling", directory) > 0);
    assert(symlink("absent", dangling) == 0);
    assert(lstat(dangling, &link_info) == 0 && S_ISLNK(link_info.st_mode));
    errno = 0; assert(stat(dangling, &link_info) == -1 && errno == ENOENT);
    assert(snprintf(loop, sizeof(loop), "%s/loop", directory) > 0);
    assert(symlink("loop", loop) == 0);
    errno = 0; assert(stat(loop, &link_info) == -1 && errno == ELOOP);
    puts("CHROME_GLIBC_SYMLINK_PASS");
    errno = 0; assert(unlink(nested) == -1 && errno == EISDIR);
    errno = 0; assert(rmdir(link_path) == -1 && errno == ENOTDIR);
    errno = 0; assert(rmdir(directory) == -1 && errno == ENOTEMPTY);
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(directory_fd >= 0 && unlinkat(directory_fd, "alias", 0) == 0);
    assert(close(directory_fd) == 0);
    assert(stat(nested, &link_info) == 0 && S_ISDIR(link_info.st_mode));
    errno = 0; assert(lstat(link_path, &link_info) == -1 && errno == ENOENT);
    assert(unlink(dangling) == 0 && unlink(loop) == 0);
    assert(unlinkat(AT_FDCWD, nested, AT_REMOVEDIR) == 0);
    puts("CHROME_GLIBC_UNLINK_PASS");
    char old_path[256], new_path[256];
    assert(snprintf(old_path, sizeof(old_path), "%s/profile.new", directory) > 0);
    assert(snprintf(new_path, sizeof(new_path), "%s/Preferences", directory) > 0);
    int old_fd = open(old_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    int replaced_fd = open(new_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(old_fd >= 0 && replaced_fd >= 0);
    assert(write(old_fd, "A", 1) == 1 && write(replaced_fd, "B", 1) == 1);
    assert(rename(old_path, new_path) == 0);
    int renamed_fd = open(new_path, O_RDONLY);
    char renamed_byte = 0;
    assert(renamed_fd >= 0 && pread(renamed_fd, &renamed_byte, 1, 0) == 1 && renamed_byte == 'A');
    assert(pread(replaced_fd, &renamed_byte, 1, 0) == 1 && renamed_byte == 'B');
    errno = 0; assert(lstat(old_path, &link_info) == -1 && errno == ENOENT);
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
    assert(directory_fd >= 0 && renameat(directory_fd, "Preferences", directory_fd, "profile.saved") == 0);
    assert(close(directory_fd) == 0 && close(old_fd) == 0 && close(replaced_fd) == 0 && close(renamed_fd) == 0);
    assert(snprintf(new_path, sizeof(new_path), "%s/profile.saved", directory) > 0);
    assert(unlink(new_path) == 0);
    puts("CHROME_GLIBC_RENAME_PASS");
    assert(snprintf(new_path, sizeof(new_path), "%s/database.lock", directory) > 0);
    int lock_fd = open(new_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(lock_fd >= 0);
    struct flock file_lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    assert(fcntl(lock_fd, F_SETLK, &file_lock) == 0);
    int lock_channel[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, lock_channel) == 0);
    pid_t lock_parent = getpid(), lock_child = fork();
    assert(lock_child >= 0);
    if (lock_child == 0) {
        close(lock_channel[0]);
        int independent_fd = open(new_path, O_RDWR);
        assert(independent_fd >= 0);
        struct flock query = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
        assert(fcntl(independent_fd, F_GETLK, &query) == 0 &&
               query.l_type == F_WRLCK && query.l_pid == lock_parent && query.l_len == 0);
        errno = 0;
        assert(fcntl(independent_fd, F_SETLK, &file_lock) == -1 &&
               (errno == EAGAIN || errno == EACCES));
        char ready = 'L';
        assert(send(lock_channel[1], &ready, 1, 0) == 1);
        assert(fcntl(independent_fd, F_SETLKW, &file_lock) == 0);
        /* POSIX 锁属于进程，不属于打开描述：关闭该 inode 的任一别名，都会释放该所有者的全部锁。 */
        int alias = open(new_path, O_RDONLY);
        assert(alias >= 0 && close(alias) == 0);
        ready = 'U';
        assert(send(lock_channel[1], &ready, 1, 0) == 1);
        assert(recv(lock_channel[1], &ready, 1, 0) == 1 && ready == 'A');
        assert(fcntl(independent_fd, F_SETLK, &file_lock) == 0);
        _exit(0); /* exit must release without an explicit unlock/close */
    }
    close(lock_channel[1]);
    char lock_ready;
    assert(recv(lock_channel[0], &lock_ready, 1, 0) == 1 && lock_ready == 'L');
    file_lock.l_type = F_UNLCK;
    assert(fcntl(lock_fd, F_SETLK, &file_lock) == 0);
    assert(recv(lock_channel[0], &lock_ready, 1, 0) == 1 && lock_ready == 'U');
    file_lock.l_type = F_WRLCK;
    assert(fcntl(lock_fd, F_SETLK, &file_lock) == 0);
    file_lock.l_type = F_UNLCK;
    assert(fcntl(lock_fd, F_SETLK, &file_lock) == 0);
    lock_ready = 'A';
    assert(send(lock_channel[0], &lock_ready, 1, 0) == 1);
    int lock_status;
    assert(waitpid(lock_child, &lock_status, 0) == lock_child &&
           WIFEXITED(lock_status) && WEXITSTATUS(lock_status) == 0);
    file_lock.l_type = F_WRLCK;
    assert(fcntl(lock_fd, F_SETLK, &file_lock) == 0);
    assert(close(lock_fd) == 0 && close(lock_channel[0]) == 0 && unlink(new_path) == 0);
    puts("CHROME_GLIBC_RECORD_LOCK_PASS");
    char shared_path[256];
    assert(snprintf(shared_path, sizeof(shared_path), "%s/shared", directory) > 0);
    int shared_fd = open(shared_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(shared_fd >= 0 && ftruncate(shared_fd, 4096) == 0);
    volatile unsigned char *first = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, shared_fd, 0);
    assert(first != MAP_FAILED);
    /* 重现 Chromium 的打包对：fd 寄存器高半部保存另一个描述符，低半部才是真正的 fd。 */
    register unsigned long arg4 __asm__("r10") = MAP_SHARED;
    register unsigned long arg5 __asm__("r8") = (45UL << 32) | (unsigned int)shared_fd;
    register unsigned long arg6 __asm__("r9") = 0;
    long mapped;
    __asm__ volatile("syscall" : "=a"(mapped) : "a"(9UL), "D"(0UL),
        "S"(4096UL), "d"((unsigned long)(PROT_READ | PROT_WRITE)),
        "r"(arg4), "r"(arg5), "r"(arg6) : "rcx", "r11", "memory");
    if (mapped < 0) {
        fprintf(stderr, "CHROME_GLIBC_PACKED_FD_MMAP_FAIL status=%ld\n", mapped);
        return 1;
    }
    volatile unsigned char *second = (void *)(uintptr_t)mapped;
    first[0] = 0x5a; assert(second[0] == 0x5a);
    second[4095] = 0x6d; assert(first[4095] == 0x6d);
    assert(close(shared_fd) == 0);
    first[1] = 0x42; assert(second[1] == 0x42);
    assert(munmap((void *)first, 4096) == 0 && munmap((void *)second, 4096) == 0);
    puts("CHROME_GLIBC_SHARED_MAP_PASS");
    /* Chromium 的 Linux 进程通道使用有序 UNIX 套接字。检查所选 libc 的真实 fork/描述符/等待 ABI，
     * 而不是原生包装函数。 */
    int channel[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == 0);
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "CHROME_GLIBC_FORK_FAIL errno=%d\n", errno);
        close(channel[0]);
        close(channel[1]);
        return 1;
    }
    if (child == 0) {
        close(channel[0]);
        struct pollfd readable = {channel[1], POLLIN, 0};
        assert(poll(&readable, 1, 5000) == 1);
        int value = 0;
        assert(recv(channel[1], &value, sizeof(value), 0) == sizeof(value));
        assert(value == 42 && tls_marker == 42);
        tls_marker = 100;
        assert(send(channel[1], &tls_marker, sizeof(tls_marker), 0) == sizeof(tls_marker));
        close(channel[1]);
        _exit(7);
    }
    close(channel[1]);
    assert(send(channel[0], &tls_marker, sizeof(tls_marker), 0) == sizeof(tls_marker));
    struct pollfd readable = {channel[0], POLLIN, 0};
    assert(poll(&readable, 1, 5000) == 1);
    int value = 0;
    assert(recv(channel[0], &value, sizeof(value), 0) == sizeof(value));
    assert(value == 100 && tls_marker == 42);
    close(channel[0]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    puts("CHROME_GLIBC_FORK_IPC_PASS");
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == 0);
    assert(channel[0] != 6 && channel[1] != 6);
    char old_channel[16];
    assert(snprintf(old_channel, sizeof(old_channel), "%d", channel[1]) > 0);
    char *child_args[] = {argv[0], "--exec-child", "6", old_channel, 0};
    const char *loader = getenv("CHROME_GLIBC_PROBE_LOADER");
    const char *libraries = getenv("CHROME_GLIBC_PROBE_LIBRARIES");
    assert((loader == 0) == (libraries == 0));
    char *loader_args[] = {(char *)loader, "--library-path", (char *)libraries,
                          argv[0], "--exec-child", "6", old_channel, 0};
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(channel[0]);
        if (dup2(channel[1], 6) != 6) _exit(14);
        /* dup2 会为重新映射的 IPC fd 清除 CLOEXEC；真实 Linux execve 替换映像时，其原始别名必须消失。 */
        execve(loader ? loader : argv[0], loader ? loader_args : child_args, environ);
        perror("CHROME_GLIBC_EXEC_FAIL");
        _exit(15);
    }
    close(channel[1]);
    assert(send(channel[0], &tls_marker, sizeof(tls_marker), 0) == sizeof(tls_marker));
    readable = (struct pollfd){channel[0], POLLIN, 0};
    assert(poll(&readable, 1, 5000) == 1);
    assert(recv(channel[0], &value, sizeof(value), 0) == sizeof(value));
    assert(value == 200 && tls_marker == 42);
    close(channel[0]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    puts("CHROME_GLIBC_EXEC_FD_PASS");
    return 0;
}
