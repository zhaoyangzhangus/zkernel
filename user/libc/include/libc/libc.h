#pragma once

/* REFACTOR_P5_LIBC_OWNER：freestanding C ABI 和 libc 公共接口。 */

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <uapi/abi.h>
#include <uapi/file.h>
#include <uapi/pipe.h>
#include <uapi/mm.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#include <uapi/time.h>
#include <uapi/wait.h>
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/uio.h>

typedef struct libc_file {
    int descriptor;
    uint32_t flags;
    int orientation;
    unsigned char *buffer;
    size_t buffer_size;
    size_t buffer_position;
    size_t buffer_length;
    int ungot;
    unsigned char wide_ungot[4];
    unsigned int wide_ungot_length;
    struct libc_file *next;
    char *temporary_path;
} FILE;

#ifndef NULL
#define NULL ((void *)0)
#endif
#define EOF (-1)
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* 将 stdio/POSIX 公共常量定义为整数宏。内核 ABI 使用 enum 表示相同取值；
 * 若在此导出该 enum，一些本来合法的比较（例如 Chromium 的
 * File::Whence static_assert）会触发 Clang 的 deprecated-enum-compare 诊断。 */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EWOULDBLOCK EAGAIN

#define LIBC_FILE_READ          (1U << 0)
#define LIBC_FILE_WRITE         (1U << 1)
#define LIBC_FILE_APPEND        (1U << 2)
#define LIBC_FILE_EOF           (1U << 3)
#define LIBC_FILE_ERROR         (1U << 4)
#define LIBC_FILE_OWN_BUFFER    (1U << 5)
#define LIBC_FILE_LINE_BUFFERED (1U << 6)
#define LIBC_FILE_CLOSED        (1U << 7)
#define LIBC_FILE_READ_BUFFER   (1U << 8)
#define LIBC_FILE_WRITE_BUFFER  (1U << 9)

#ifdef __cplusplus
extern "C" {
#endif

/* 完整定义位于 sys/stat.h 和 time.h。前置声明使此总头文件保持自包含，
 * 同时避免产生循环包含。 */
struct stat;
struct timespec;

int64_t __libc_native_syscall6(uint64_t number, uint64_t argument0,
                        uint64_t argument1, uint64_t argument2,
                        uint64_t argument3, uint64_t argument4,
                        uint64_t argument5);
struct os_memory_info;
int __libc_query_memory_info(struct os_memory_info *info);
/* Linux ABI 入口。与 __libc_native_syscall6() 不同，此入口不修改 RAX，
 * 用于 Linux syscall() 和兼容模式探测。 */
int64_t __libc_linux_syscall6(uint64_t number, uint64_t argument0,
                              uint64_t argument1, uint64_t argument2,
                              uint64_t argument3, uint64_t argument4,
                              uint64_t argument5);
bool __libc_linux_personality(void);

/* socket 和文件前端共用的内部描述符钩子。 */
int __libc_install_handle(os_handle_t handle, uint32_t open_flags,
                          bool socket_handle, uint32_t socket_family);
int __libc_install_pipe_handle(os_handle_t handle, uint32_t open_flags,
                               bool read_end);
/* Linux 兼容模式的描述符与 System 原生描述符共用 POSIX 整数命名空间。
 * 这些钩子将原始 Linux 描述符接管到 libc 表中的空闲槽位，并让 Linux 句柄与槽位相同，
 * 使 libc 封装和直接 Linux 系统调用看到相同的编号。 */
int __libc_adopt_linux_fd(int linux_descriptor, int open_flags);
void __libc_fd_table_fork_prepare(void);
void __libc_fd_table_fork_parent(void);
void __libc_fd_table_fork_child(void);
int __libc_linux_fd_for_descriptor(int descriptor);
bool __libc_descriptor_is_linux_fd(int descriptor);
int __libc_mark_linux_socket_descriptor(int descriptor, uint32_t family);
os_handle_t __libc_handle_for_descriptor(int descriptor);
bool __libc_descriptor_is_socket(int descriptor);
bool __libc_descriptor_is_pipe(int descriptor);
bool __libc_pipe_descriptor_is_read(int descriptor);
uint32_t __libc_socket_family(int descriptor);
uint32_t __libc_descriptor_open_flags(int descriptor);
int __libc_descriptor_path(int descriptor, char *output, size_t capacity);
int __libc_status_result(int64_t status);
int __libc_make_absolute_path(const char *path, char *output);
void __libc_build_exec_fd_map(os_exec_fd_map_t *map);
void __libc_init_descriptors(char **envp);
int *__libc_thread_errno_slot(void);
int __libc_thread_init(void);
void __libc_reap_detached_threads(void);
void __libc_close_temporary_files(void);

/* ISO C 内存和字符串函数。 */
void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *left, const void *right, size_t length);
void *memccpy(void *destination, const void *source, int value, size_t length);
void *memchr(const void *buffer, int value, size_t length);
void *memrchr(const void *buffer, int value, size_t length);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t length);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t length);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t length);
char *strchrnul(const char *text, int value);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strstr(const char *text, const char *needle);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strpbrk(const char *text, const char *accept);
char *strtok(char *text, const char *delimiters);
char *strtok_r(char *text, const char *delimiters, char **state);
size_t strxfrm(char *destination, const char *source, size_t length);
int strcoll(const char *left, const char *right);
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t length);
char *stpcpy(char *destination, const char *source);
char *stpncpy(char *destination, const char *source, size_t length);
size_t strlcpy(char *destination, const char *source, size_t capacity);
size_t strlcat(char *destination, const char *source, size_t capacity);
char *strcasestr(const char *text, const char *needle);
void *memmem(const void *buffer, size_t length,
             const void *needle, size_t needle_length);
void *mempcpy(void *destination, const void *source, size_t length);
char *strdup(const char *text);
char *strndup(const char *text, size_t length);
char *strerror(int error_number);
int strerror_r(int error_number, char *buffer, size_t length);

/* C 字符分类函数。 */
int isalnum(int value);
int isalpha(int value);
int isblank(int value);
int iscntrl(int value);
int isdigit(int value);
int isgraph(int value);
int isascii(int value);
int islower(int value);
int isprint(int value);
int ispunct(int value);
int isspace(int value);
int isupper(int value);
int isxdigit(int value);
int tolower(int value);
int toupper(int value);

/* 内存分配和通用工具函数。 */
void *malloc(size_t size) __THROW;
void *calloc(size_t count, size_t size) __THROW;
void *realloc(void *pointer, size_t size) __THROW;
void *reallocarray(void *pointer, size_t count, size_t size) __THROW;
void *aligned_alloc(size_t alignment, size_t size) __THROW;
int posix_memalign(void **pointer, size_t alignment, size_t size) __THROW;
void *memalign(size_t alignment, size_t size) __THROW;
void *valloc(size_t size) __THROW;
size_t malloc_usable_size(void *pointer) __THROW;
void free(void *pointer) __THROW;
__attribute__((target("sse"))) double atof(const char *text);
int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
intmax_t strtoimax(const char *text, char **end, int base);
uintmax_t strtoumax(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);
long double strtold(const char *text, char **end);
int abs(int value);
long labs(long value);
long long llabs(long long value);

#ifndef LIBC_DIV_TYPES_DEFINED
#define LIBC_DIV_TYPES_DEFINED
typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;
#endif
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);
void srand(unsigned int seed);
int rand(void);
void qsort(void *base, size_t count, size_t size,
          int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
char *getenv(const char *name);
char *secure_getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *assignment);
int atexit(void (*function)(void));
int at_quick_exit(void (*function)(void));
void quick_exit(int status) __attribute__((noreturn));
int system(const char *command);
int mkstemp(char *template_name);
char *mkdtemp(char *template_name);
char *mktemp(char *template_name);
char *realpath(const char *path, char *resolved);
int getsubopt(char **optionp, char *const *tokens, char **valuep);

/* 文件描述符与标准 I/O。 */
int open(const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat(int directory_descriptor, const char *path, int flags, ...);
int openat64(int directory_descriptor, const char *path, int flags, ...);
int creat(const char *path, unsigned int mode);
int creat64(const char *path, unsigned int mode);
ssize_t read(int descriptor, void *buffer, size_t length);
ssize_t write(int descriptor, const void *buffer, size_t length);
int close(int descriptor);
off_t lseek(int descriptor, off_t offset, int whence);
off_t lseek64(int descriptor, off_t offset, int whence);
ssize_t pread(int descriptor, void *buffer, size_t length, off_t offset);
ssize_t pread64(int descriptor, void *buffer, size_t length, off_t offset);
ssize_t pwrite(int descriptor, const void *buffer, size_t length, off_t offset);
ssize_t pwrite64(int descriptor, const void *buffer, size_t length, off_t offset);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
ssize_t readv(int descriptor, const struct iovec *vectors, int count);
ssize_t writev(int descriptor, const struct iovec *vectors, int count);
ssize_t preadv(int descriptor, const struct iovec *vectors, int count,
               off_t offset);
ssize_t pwritev(int descriptor, const struct iovec *vectors, int count,
                off_t offset);
int fsync(int descriptor);
int fdatasync(int descriptor);
int ftruncate(int descriptor, off_t length);
int ftruncate64(int descriptor, off_t length);
int truncate(const char *path, off_t length);
int truncate64(const char *path, off_t length);
#ifndef __NATIVE_OS__
void sync(void);
#endif
int stat(const char *path, struct stat *status);
int fstat(int descriptor, struct stat *status);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int mkdir(const char *path, unsigned int mode);
int rename(const char *old_path, const char *new_path);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
ssize_t readlink(const char *path, char *buffer, size_t size);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t process_group);
int isatty(int descriptor);
int dup(int descriptor);
int dup2(int old_descriptor, int new_descriptor);
int dup3(int old_descriptor, int new_descriptor, int flags);
int getdtablesize(void);
int fcntl(int descriptor, int command, ...);
int ioctl(int descriptor, unsigned long request, ...) __THROW;
int getpid(void);
int getppid(void);
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
pid_t fork(void);
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int microseconds);
long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int descriptor, int name);
size_t confstr(int name, char *buffer, size_t size);
int getpagesize(void);


void *mmap(void *address, size_t length, int protection, int flags,
           int descriptor, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int msync(void *address, size_t length, int flags);
int madvise(void *address, size_t length, int advice);

int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *argument0, ...);
int execlp(const char *file, const char *argument0, ...);
int execle(const char *path, const char *argument0, ...);

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char *const argv[], const char *options);

int clock_gettime(int clock_id, struct timespec *value);
int clock_settime(int clock_id, const struct timespec *value);
int clock_getres(int clock_id, struct timespec *value);
int nanosleep(const struct timespec *request, struct timespec *remaining);
int clock_nanosleep(int clock_id, int flags, const struct timespec *request,
                    struct timespec *remaining);

int vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments);
int snprintf(char *buffer, size_t capacity, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list arguments);
int sprintf(char *buffer, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list arguments);
int vdprintf(int descriptor, const char *format, va_list arguments);
int fprintf(FILE *stream, const char *format, ...);
int dprintf(int descriptor, const char *format, ...);
int vprintf(const char *format, va_list arguments);
int printf(const char *format, ...);
int vasprintf(char **buffer, const char *format, va_list arguments);
int asprintf(char **buffer, const char *format, ...);
int putchar(int value);
int puts(const char *text);
FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode);
FILE *fdopen(int descriptor, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
FILE *freopen64(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int fputc(int value, FILE *stream);
int putc(int value, FILE *stream);
int putchar(int value);
char *fgets(char *buffer, int capacity, FILE *stream);
int fputs(const char *text, FILE *stream);
int fwide(FILE *stream, int mode);
wint_t fgetwc(FILE *stream);
wint_t ungetwc(wint_t value, FILE *stream);
wint_t fputwc(wchar_t value, FILE *stream);
wchar_t *fgetws(wchar_t *buffer, int capacity, FILE *stream);
int fputws(const wchar_t *text, FILE *stream);
wint_t getwc(FILE *stream);
wint_t putwc(wchar_t value, FILE *stream);
wint_t getwchar(void);
wint_t putwchar(wchar_t value);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, off_t *position);
int fsetpos(FILE *stream, const off_t *position);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int ungetc(int value, FILE *stream);
int fileno(FILE *stream);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
void perror(const char *prefix);
int remove(const char *path);
FILE *tmpfile(void);
char *tmpnam(char *buffer);
int vscanf(const char *format, va_list arguments);
int scanf(const char *format, ...);
int vfscanf(FILE *stream, const char *format, va_list arguments);
int fscanf(FILE *stream, const char *format, ...);
int vsscanf(const char *text, const char *format, va_list arguments);
int sscanf(const char *text, const char *format, ...);
ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);
ssize_t getline(char **line, size_t *capacity, FILE *stream);
int getc_unlocked(FILE *stream);
int getchar_unlocked(void);
int putc_unlocked(int value, FILE *stream);
int putchar_unlocked(int value);
char *fgets_unlocked(char *buffer, int capacity, FILE *stream);
int fputs_unlocked(const char *text, FILE *stream);
int fputc_unlocked(int value, FILE *stream);
size_t fread_unlocked(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite_unlocked(const void *buffer, size_t size, size_t count,
                       FILE *stream);
int fflush_unlocked(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#ifdef __cplusplus
extern "C" {
#endif
extern char **environ;
#ifdef __cplusplus
}
#endif

/* 独立运行 CRT 使用的进程启动与终止钩子。 */
int __libc_start_main(int argc, char **argv, char **envp);
void __libc_init_environment(char **envp);
void __libc_run_exit_handlers(void);
void __libc_atfork_prepare(void);
void __libc_atfork_parent(void);
void __libc_atfork_child(void);
void __assert_fail(const char *expression, const char *file,
                   int line, const char *function) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
void _exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
