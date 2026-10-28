#include "libc/libc.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <wchar.h>

#define LIBC_BUFSIZ 4096U
#define LIBC_TMPNAM_SIZE 64U
#define LIBC_IOFBF 0
#define LIBC_IOLBF 1
#define LIBC_IONBF 2

typedef struct format_output {
    char *buffer;
    size_t capacity;
    size_t length;
} format_output_t;

typedef struct format_spec {
    bool left;
    bool plus;
    bool space;
    bool alternate;
    bool zero;
    size_t width;
    int precision;
    char length;
} format_spec_t;

static FILE g_stderr = {
    .descriptor = STDERR_FILENO,
    .flags = LIBC_FILE_WRITE,
    .ungot = EOF,
};
static FILE g_stdout = {
    .descriptor = STDOUT_FILENO,
    .flags = LIBC_FILE_WRITE | LIBC_FILE_LINE_BUFFERED,
    .ungot = EOF,
    .next = &g_stderr,
};
static FILE g_stdin = {
    .descriptor = STDIN_FILENO,
    .flags = LIBC_FILE_READ,
    .ungot = EOF,
    .next = &g_stdout,
};

FILE *stdin = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;
static FILE *g_streams = &g_stdin;

static bool stream_is_open(const FILE *stream) {
    return stream != 0 && (stream->flags & LIBC_FILE_CLOSED) == 0U;
}

static bool stream_can_read(const FILE *stream) {
    return stream_is_open(stream) &&
           (stream->flags & LIBC_FILE_READ) != 0U;
}

static bool stream_can_write(const FILE *stream) {
    return stream_is_open(stream) &&
           (stream->flags & LIBC_FILE_WRITE) != 0U;
}

static void register_stream(FILE *stream) {
    stream->next = g_streams;
    g_streams = stream;
}

static void unregister_stream(FILE *stream) {
    FILE **cursor = &g_streams;
    while (*cursor != 0) {
        if (*cursor == stream) {
            *cursor = stream->next;
            stream->next = 0;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static int remove_temporary_file(FILE *stream) {
    char *path = stream->temporary_path;
    int result;
    if (path == 0) return 0;
    stream->temporary_path = 0;
    result = unlink(path);
    free(path);
    return result;
}

static int write_all(int descriptor, const void *buffer, size_t length) {
    const unsigned char *cursor = (const unsigned char *)buffer;
    while (length != 0U) {
        ssize_t result = write(descriptor, cursor, length);
        if (result < 0) return -1;
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
    return 0;
}

static int flush_stream(FILE *stream) {
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return -1;
    }
    if ((stream->flags & LIBC_FILE_WRITE_BUFFER) == 0U) return 0;
    if (write_all(stream->descriptor, stream->buffer,
                  stream->buffer_position) < 0) {
        stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    stream->buffer_position = 0U;
    stream->buffer_length = 0U;
    stream->flags &= ~LIBC_FILE_WRITE_BUFFER;
    return 0;
}

/* 将内核文件偏移回退到仍留在输入缓冲区中的字节之前。
 * 更新流从读取切换为写入或执行定位前必须完成此操作。 */
static int discard_read_buffer(FILE *stream) {
    size_t unread = 0U;
    if ((stream->flags & LIBC_FILE_READ_BUFFER) != 0U) {
        unread = stream->buffer_length - stream->buffer_position;
    }
    if (stream->ungot != EOF) ++unread;
    unread += stream->wide_ungot_length;
    if (unread != 0U && lseek(stream->descriptor, -(off_t)unread,
                              SEEK_CUR) < 0) {
        stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    stream->buffer_position = 0U;
    stream->buffer_length = 0U;
    stream->ungot = EOF;
    stream->wide_ungot_length = 0U;
    stream->flags &= ~LIBC_FILE_READ_BUFFER;
    return 0;
}

static int stream_seek(FILE *stream, off_t offset, int whence) {
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return -1;
    }
    if (flush_stream(stream) < 0 || discard_read_buffer(stream) < 0) {
        return -1;
    }
    if (lseek(stream->descriptor, offset, whence) < 0) return -1;
    stream->flags &= ~(LIBC_FILE_EOF | LIBC_FILE_ERROR);
    stream->ungot = EOF;
    return 0;
}

static int stream_position(FILE *stream, off_t *result) {
    off_t position;
    size_t unread = 0U;
    if (result == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return -1;
    }
    if (flush_stream(stream) < 0) return -1;
    position = lseek(stream->descriptor, 0, SEEK_CUR);
    if (position < 0) return -1;
    if ((stream->flags & LIBC_FILE_READ_BUFFER) != 0U) {
        if (stream->buffer_position > stream->buffer_length) {
            errno = EIO;
            stream->flags |= LIBC_FILE_ERROR;
            return -1;
        }
        unread = stream->buffer_length - stream->buffer_position;
    }
    if (stream->ungot != EOF) {
        if (unread == SIZE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        ++unread;
    }
    if (unread > (size_t)INT64_MAX || position < (off_t)unread) {
        errno = EIO;
        stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    *result = position - (off_t)unread;
    return 0;
}

static int prepare_read(FILE *stream) {
    if (!stream_can_read(stream)) {
        errno = EBADF;
        if (stream != 0) stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    if ((stream->flags & LIBC_FILE_WRITE_BUFFER) != 0U &&
        flush_stream(stream) < 0) return -1;
    return 0;
}

static int prepare_write(FILE *stream) {
    if (!stream_can_write(stream)) {
        errno = EBADF;
        if (stream != 0) stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    if ((stream->flags & LIBC_FILE_READ_BUFFER) != 0U ||
        stream->ungot != EOF) {
        if (discard_read_buffer(stream) < 0) return -1;
    }
    return 0;
}

static int fill_input_buffer(FILE *stream) {
    ssize_t result;
    if (prepare_read(stream) < 0) return -1;
    if (stream->buffer == 0 || stream->buffer_size == 0U) {
        unsigned char value;
        result = read(stream->descriptor, &value, 1U);
        if (result < 0) {
            stream->flags |= LIBC_FILE_ERROR;
            return -1;
        }
        if (result == 0) {
            stream->flags |= LIBC_FILE_EOF;
            return 0;
        }
        /* 无缓冲区的流没有安全位置保存字节。标准流通常不带缓冲，因此通过单字节回退槽
         * 返回该字节。 */
        stream->ungot = value;
        stream->flags &= ~LIBC_FILE_EOF;
        return 1;
    }
    result = read(stream->descriptor, stream->buffer, stream->buffer_size);
    if (result < 0) {
        stream->flags |= LIBC_FILE_ERROR;
        return -1;
    }
    if (result == 0) {
        stream->flags |= LIBC_FILE_EOF;
        return 0;
    }
    stream->buffer_position = 0U;
    stream->buffer_length = (size_t)result;
    stream->flags |= LIBC_FILE_READ_BUFFER;
    stream->flags &= ~LIBC_FILE_EOF;
    return 1;
}

static int stream_get_character(FILE *stream) {
    if (prepare_read(stream) < 0) return EOF;
    if (stream->wide_ungot_length != 0U) {
        int value = stream->wide_ungot[0];
        if (stream->wide_ungot_length > 1U) {
            memmove(stream->wide_ungot, stream->wide_ungot + 1,
                    stream->wide_ungot_length - 1U);
        }
        --stream->wide_ungot_length;
        stream->flags &= ~LIBC_FILE_EOF;
        return value;
    }
    if (stream->ungot != EOF) {
        int value = stream->ungot;
        stream->ungot = EOF;
        stream->flags &= ~LIBC_FILE_EOF;
        return value;
    }
    if ((stream->flags & LIBC_FILE_READ_BUFFER) == 0U ||
        stream->buffer_position >= stream->buffer_length) {
        if (fill_input_buffer(stream) <= 0) return EOF;
    }
    if (stream->ungot != EOF) return stream_get_character(stream);
    stream->flags &= ~LIBC_FILE_EOF;
    return stream->buffer[stream->buffer_position++];
}

static size_t stream_read_bytes(FILE *stream, void *buffer, size_t length) {
    unsigned char *cursor = (unsigned char *)buffer;
    size_t completed = 0U;
    if (prepare_read(stream) < 0) return 0U;
    while (completed < length) {
        if (stream->wide_ungot_length != 0U) {
            cursor[completed++] = stream->wide_ungot[0];
            if (stream->wide_ungot_length > 1U) {
                memmove(stream->wide_ungot, stream->wide_ungot + 1,
                        stream->wide_ungot_length - 1U);
            }
            --stream->wide_ungot_length;
            continue;
        }
        if (stream->ungot != EOF) {
            cursor[completed++] = (unsigned char)stream->ungot;
            stream->ungot = EOF;
            continue;
        }
        if ((stream->flags & LIBC_FILE_READ_BUFFER) == 0U ||
            stream->buffer_position >= stream->buffer_length) {
            if (fill_input_buffer(stream) <= 0) break;
        }
        if (stream->ungot != EOF) continue;
        if ((stream->flags & LIBC_FILE_READ_BUFFER) != 0U) {
            size_t available = stream->buffer_length - stream->buffer_position;
            size_t amount = length - completed;
            if (amount > available) amount = available;
            memcpy(cursor + completed,
                   stream->buffer + stream->buffer_position, amount);
            stream->buffer_position += amount;
            completed += amount;
        }
    }
    return completed;
}

static size_t stream_write_bytes(FILE *stream, const void *buffer,
                                 size_t length) {
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t completed = 0U;
    if (prepare_write(stream) < 0) return 0U;
    if (stream->buffer == 0 || stream->buffer_size == 0U) {
        while (completed < length) {
            ssize_t result = write(stream->descriptor, cursor + completed,
                                    length - completed);
            if (result < 0) {
                stream->flags |= LIBC_FILE_ERROR;
                break;
            }
            if (result == 0) {
                errno = EIO;
                stream->flags |= LIBC_FILE_ERROR;
                break;
            }
            completed += (size_t)result;
        }
        return completed;
    }
    while (completed < length) {
        size_t available = stream->buffer_size - stream->buffer_position;
        size_t amount = length - completed;
        if (amount > available) amount = available;
        memcpy(stream->buffer + stream->buffer_position,
               cursor + completed, amount);
        stream->buffer_position += amount;
        stream->flags |= LIBC_FILE_WRITE_BUFFER;
        completed += amount;
        if (stream->buffer_position == stream->buffer_size ||
            ((stream->flags & LIBC_FILE_LINE_BUFFERED) != 0U &&
             memchr(stream->buffer + stream->buffer_position - amount,
                    '\n', amount) != 0)) {
            if (flush_stream(stream) < 0) break;
        }
    }
    return completed;
}

static FILE *create_stream(int descriptor, uint32_t flags) {
    FILE *stream = (FILE *)malloc(sizeof(*stream));
    if (stream == 0) return 0;
    memset(stream, 0, sizeof(*stream));
    stream->descriptor = descriptor;
    stream->flags = flags;
    stream->ungot = EOF;
    stream->buffer = (unsigned char *)malloc(LIBC_BUFSIZ);
    if (stream->buffer != 0) {
        stream->buffer_size = LIBC_BUFSIZ;
        stream->flags |= LIBC_FILE_OWN_BUFFER;
    }
    register_stream(stream);
    return stream;
}

static bool parse_file_mode(const char *mode, int *open_flags,
                            uint32_t *stream_flags) {
    bool plus = false;
    bool exclusive = false;
    bool close_on_exec = false;
    const char *cursor;
    if (mode == 0 || mode[0] == '\0' || open_flags == 0 ||
        stream_flags == 0) return false;
    cursor = mode + 1U;
    while (*cursor != '\0') {
        switch (*cursor++) {
        case '+': plus = true; break;
        case 'x': exclusive = true; break;
        case 'b': break;
        case 't': break;
        case 'e': close_on_exec = true; break;
        default: return false;
        }
    }
    *stream_flags = 0U;
    switch (mode[0]) {
    case 'r':
        *open_flags = plus ? O_RDWR : O_RDONLY;
        *stream_flags = LIBC_FILE_READ | (plus ? LIBC_FILE_WRITE : 0U);
        break;
    case 'w':
        *open_flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        *stream_flags = LIBC_FILE_WRITE | (plus ? LIBC_FILE_READ : 0U);
        break;
    case 'a':
        *open_flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        *stream_flags = LIBC_FILE_WRITE | LIBC_FILE_APPEND |
                        (plus ? LIBC_FILE_READ : 0U);
        break;
    default: return false;
    }
    if (exclusive) *open_flags |= O_EXCL;
    if (close_on_exec) *open_flags |= O_CLOEXEC;
    return true;
}

FILE *fopen(const char *path, const char *mode) {
    int open_flags;
    uint32_t stream_flags;
    int descriptor;
    FILE *stream;
    if (!parse_file_mode(mode, &open_flags, &stream_flags)) {
        errno = EINVAL;
        return 0;
    }
    descriptor = open(path, open_flags, 0666U);
    if (descriptor < 0) return 0;
    if ((stream_flags & LIBC_FILE_APPEND) != 0U) {
        (void)lseek(descriptor, 0, SEEK_END);
    }
    stream = create_stream(descriptor, stream_flags);
    if (stream == 0) {
        (void)close(descriptor);
        return 0;
    }
    return stream;
}

FILE *fopen64(const char *path, const char *mode) {
    return fopen(path, mode);
}

FILE *fdopen(int descriptor, const char *mode) {
    int open_flags;
    uint32_t stream_flags;
    int descriptor_flags;
    if (descriptor < 0 || !parse_file_mode(mode, &open_flags, &stream_flags)) {
        errno = EINVAL;
        return 0;
    }
    if ((open_flags & O_CLOEXEC) != 0) {
        descriptor_flags = fcntl(descriptor, F_GETFD);
        if (descriptor_flags < 0 ||
            fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
            return 0;
        }
    }
    return create_stream(descriptor, stream_flags);
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    int open_flags;
    uint32_t stream_flags;
    int descriptor;
    if (!stream_is_open(stream) || !parse_file_mode(mode, &open_flags,
                                                    &stream_flags)) {
        errno = EINVAL;
        return 0;
    }
    (void)fflush(stream);
    (void)close(stream->descriptor);
    (void)remove_temporary_file(stream);
    if ((stream->flags & LIBC_FILE_OWN_BUFFER) != 0U) free(stream->buffer);
    descriptor = open(path, open_flags, 0666U);
    if (descriptor < 0) {
        stream->flags = LIBC_FILE_CLOSED;
        stream->buffer = 0;
        stream->buffer_size = 0U;
        return 0;
    }
    stream->descriptor = descriptor;
    stream->flags = stream_flags;
    stream->buffer = (unsigned char *)malloc(LIBC_BUFSIZ);
    stream->buffer_size = stream->buffer != 0 ? LIBC_BUFSIZ : 0U;
    if (stream->buffer != 0) stream->flags |= LIBC_FILE_OWN_BUFFER;
    stream->buffer_position = 0U;
    stream->buffer_length = 0U;
    stream->ungot = EOF;
    if ((stream_flags & LIBC_FILE_APPEND) != 0U) {
        (void)lseek(descriptor, 0, SEEK_END);
    }
    return stream;
}

FILE *freopen64(const char *path, const char *mode, FILE *stream) {
    return freopen(path, mode, stream);
}

int fclose(FILE *stream) {
    int result = 0;
    bool standard;
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return EOF;
    }
    standard = stream == stdin || stream == stdout || stream == stderr;
    if (flush_stream(stream) < 0) result = EOF;
    if (close(stream->descriptor) < 0) result = EOF;
    if (remove_temporary_file(stream) < 0) result = EOF;
    unregister_stream(stream);
    if ((stream->flags & LIBC_FILE_OWN_BUFFER) != 0U) free(stream->buffer);
    stream->descriptor = -1;
    stream->flags = LIBC_FILE_CLOSED;
    stream->buffer = 0;
    stream->buffer_size = 0U;
    stream->buffer_position = 0U;
    stream->buffer_length = 0U;
    stream->ungot = EOF;
    if (standard) return result;
    free(stream);
    return result;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
    size_t total;
    size_t bytes;
    if (size == 0U || count == 0U) return 0U;
    if (buffer == 0 || stream == 0 || count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0U;
    }
    total = size * count;
    bytes = stream_read_bytes(stream, buffer, total);
    return bytes / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    size_t total;
    size_t bytes;
    if (size == 0U || count == 0U) return 0U;
    if (buffer == 0 || stream == 0 || count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0U;
    }
    total = size * count;
    bytes = stream_write_bytes(stream, buffer, total);
    return bytes / size;
}

int fflush(FILE *stream) {
    int result = 0;
    if (stream == 0) {
        for (FILE *cursor = g_streams; cursor != 0; cursor = cursor->next) {
            if (flush_stream(cursor) < 0) result = EOF;
        }
        return result;
    }
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return EOF;
    }
    if ((stream->flags & LIBC_FILE_READ_BUFFER) != 0U &&
        discard_read_buffer(stream) < 0) return EOF;
    return flush_stream(stream) < 0 ? EOF : 0;
}

int fgetc(FILE *stream) {
    return stream_get_character(stream);
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int getchar(void) {
    return fgetc(stdin);
}

int fputc(int value, FILE *stream) {
    unsigned char character = (unsigned char)value;
    return stream_write_bytes(stream, &character, 1U) == 1U ?
           (int)character : EOF;
}

int putc(int value, FILE *stream) {
    return fputc(value, stream);
}

int putchar(int value) {
    return fputc(value, stdout);
}

char *fgets(char *buffer, int capacity, FILE *stream) {
    size_t length = 0U;
    int value;
    if (buffer == 0 || capacity <= 0 || stream == 0) {
        errno = EINVAL;
        return 0;
    }
    while (length + 1U < (size_t)capacity) {
        value = fgetc(stream);
        if (value == EOF) break;
        buffer[length++] = (char)value;
        if (value == '\n') break;
    }
    if (length == 0U) return 0;
    buffer[length] = '\0';
    return buffer;
}

int fputs(const char *text, FILE *stream) {
    size_t length;
    if (text == 0) {
        errno = EINVAL;
        return EOF;
    }
    length = strlen(text);
    return stream_write_bytes(stream, text, length) == length ? 0 : EOF;
}

int fwide(FILE *stream, int mode) {
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return 0;
    }
    if (stream->orientation == 0 && mode != 0) {
        stream->orientation = mode > 0 ? 1 : -1;
    }
    return stream->orientation;
}

wint_t fgetwc(FILE *stream) {
    mbstate_t state = {0};
    wchar_t value = L'\0';
    if (!stream_can_read(stream)) {
        errno = EBADF;
        return WEOF;
    }
    if (stream->orientation < 0) {
        errno = EINVAL;
        return WEOF;
    }
    stream->orientation = 1;
    for (;;) {
        unsigned char byte;
        int character = stream_get_character(stream);
        size_t result;
        if (character == EOF) {
            if (!mbsinit(&state)) {
                errno = EILSEQ;
                stream->flags |= LIBC_FILE_ERROR;
            }
            return WEOF;
        }
        byte = (unsigned char)character;
        result = mbrtowc(&value, (const char *)&byte, 1U, &state);
        if (result == (size_t)-2) continue;
        if (result == (size_t)-1) {
            stream->flags |= LIBC_FILE_ERROR;
            return WEOF;
        }
        return (wint_t)value;
    }
}

wint_t ungetwc(wint_t value, FILE *stream) {
    char encoded[4];
    mbstate_t state = {0};
    size_t length;
    if (value == WEOF || !stream_can_read(stream) ||
        stream->ungot != EOF || stream->wide_ungot_length != 0U) {
        errno = EINVAL;
        return WEOF;
    }
    length = wcrtomb(encoded, (wchar_t)value, &state);
    if (length == (size_t)-1 || length > sizeof(stream->wide_ungot)) {
        return WEOF;
    }
    memcpy(stream->wide_ungot, encoded, length);
    stream->wide_ungot_length = (unsigned int)length;
    stream->flags &= ~LIBC_FILE_EOF;
    return value;
}

wint_t fputwc(wchar_t value, FILE *stream) {
    char encoded[MB_LEN_MAX];
    mbstate_t state = {0};
    size_t length;
    if (!stream_can_write(stream)) {
        errno = EBADF;
        return WEOF;
    }
    if (stream->orientation < 0) {
        errno = EINVAL;
        return WEOF;
    }
    stream->orientation = 1;
    length = wcrtomb(encoded, value, &state);
    if (length == (size_t)-1 ||
        stream_write_bytes(stream, encoded, length) != length) return WEOF;
    return (wint_t)value;
}

wchar_t *fgetws(wchar_t *buffer, int capacity, FILE *stream) {
    int length = 0;
    if (buffer == 0 || capacity <= 0 || stream == 0) {
        errno = EINVAL;
        return 0;
    }
    while (length + 1 < capacity) {
        wint_t value = fgetwc(stream);
        if (value == WEOF) break;
        buffer[length++] = (wchar_t)value;
        if (value == (wint_t)L'\n') break;
    }
    if (length == 0) return 0;
    buffer[length] = L'\0';
    return buffer;
}

int fputws(const wchar_t *text, FILE *stream) {
    if (text == 0) {
        errno = EINVAL;
        return WEOF;
    }
    while (*text != L'\0') {
        if (fputwc(*text++, stream) == WEOF) return WEOF;
    }
    return 0;
}

wint_t getwc(FILE *stream) {
    return fgetwc(stream);
}

wint_t putwc(wchar_t value, FILE *stream) {
    return fputwc(value, stream);
}

wint_t getwchar(void) {
    return fgetwc(stdin);
}

wint_t putwchar(wchar_t value) {
    return fputwc(value, stdout);
}

int puts(const char *text) {
    if (fputs(text, stdout) == EOF || fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    return stream_seek(stream, (off_t)offset, whence);
}

long ftell(FILE *stream) {
    off_t position;
    if (stream_position(stream, &position) < 0) return -1L;
    if ((int64_t)position > (int64_t)LONG_MAX ||
        (int64_t)position < (int64_t)LONG_MIN) {
        errno = EOVERFLOW;
        return -1L;
    }
    return (long)position;
}

void rewind(FILE *stream) {
    if (fseek(stream, 0L, SEEK_SET) == 0) clearerr(stream);
}

int fgetpos(FILE *stream, off_t *position) {
    if (position == 0) {
        errno = EINVAL;
        return -1;
    }
    return stream_position(stream, position);
}

int fsetpos(FILE *stream, const off_t *position) {
    if (position == 0) {
        errno = EINVAL;
        return -1;
    }
    return stream_seek(stream, *position, SEEK_SET);
}

int feof(FILE *stream) {
    return stream != 0 && (stream->flags & LIBC_FILE_EOF) != 0U;
}

int ferror(FILE *stream) {
    return stream != 0 && (stream->flags & LIBC_FILE_ERROR) != 0U;
}

void clearerr(FILE *stream) {
    if (stream != 0) stream->flags &= ~(LIBC_FILE_EOF | LIBC_FILE_ERROR);
}

int ungetc(int value, FILE *stream) {
    if (value == EOF || !stream_can_read(stream) || stream->ungot != EOF) {
        errno = EINVAL;
        return EOF;
    }
    stream->ungot = (unsigned char)value;
    stream->flags &= ~LIBC_FILE_EOF;
    return (unsigned char)value;
}

int fileno(FILE *stream) {
    if (!stream_is_open(stream)) {
        errno = EBADF;
        return -1;
    }
    return stream->descriptor;
}

int setvbuf(FILE *stream, char *buffer, int mode, size_t size) {
    unsigned char *replacement = (unsigned char *)buffer;
    bool own_replacement = false;
    if (!stream_is_open(stream) ||
        (mode != LIBC_IOFBF && mode != LIBC_IOLBF && mode != LIBC_IONBF)) {
        errno = EINVAL;
        return -1;
    }
    if (fflush(stream) != 0) return -1;
    if ((stream->flags & LIBC_FILE_OWN_BUFFER) != 0U) free(stream->buffer);
    stream->buffer = 0;
    stream->buffer_size = 0U;
    stream->buffer_position = 0U;
    stream->buffer_length = 0U;
    stream->flags &= ~(LIBC_FILE_OWN_BUFFER | LIBC_FILE_LINE_BUFFERED |
                       LIBC_FILE_READ_BUFFER | LIBC_FILE_WRITE_BUFFER);
    if (mode == LIBC_IONBF) return 0;
    if (replacement == 0) {
        if (size == 0U) size = LIBC_BUFSIZ;
        replacement = (unsigned char *)malloc(size);
        own_replacement = true;
        if (replacement == 0) return -1;
    }
    if (size == 0U) {
        if (own_replacement) free(replacement);
        errno = EINVAL;
        return -1;
    }
    stream->buffer = replacement;
    stream->buffer_size = size;
    if (own_replacement) stream->flags |= LIBC_FILE_OWN_BUFFER;
    if (mode == LIBC_IOLBF) stream->flags |= LIBC_FILE_LINE_BUFFERED;
    return 0;
}

void setbuf(FILE *stream, char *buffer) {
    if (buffer == 0) (void)setvbuf(stream, 0, LIBC_IONBF, 0U);
    else (void)setvbuf(stream, buffer, LIBC_IOFBF, LIBC_BUFSIZ);
}

static void format_output_character(format_output_t *output, char value) {
    if (output->buffer != 0 && output->capacity != 0U &&
        output->length < output->capacity - 1U) {
        output->buffer[output->length] = value;
    }
    if (output->length != SIZE_MAX) ++output->length;
}

static void format_output_text(format_output_t *output, const char *text,
                               size_t length) {
    if (text == 0) text = "(null)";
    if (length == SIZE_MAX) length = strlen(text);
    for (size_t index = 0U; index < length; ++index) {
        format_output_character(output, text[index]);
    }
}

static void format_output_repeat(format_output_t *output, char value,
                                 size_t count) {
    while (count-- != 0U) format_output_character(output, value);
}

static size_t parse_format_number(const char **format) {
    size_t result = 0U;
    while (**format >= '0' && **format <= '9') {
        unsigned int digit = (unsigned int)(*(*format)++ - '0');
        if (result > (SIZE_MAX - digit) / 10U) return SIZE_MAX;
        result = result * 10U + digit;
    }
    return result;
}

static void format_output_integer(format_output_t *output, uint64_t value,
                                  bool negative, unsigned int base,
                                  bool uppercase, const format_spec_t *spec) {
    char digits[64];
    char sign[1];
    char prefix[2];
    size_t digit_count = 0U;
    size_t precision_zeroes = 0U;
    size_t prefix_length = 0U;
    size_t sign_length = 0U;
    size_t body_length;
    size_t padding;
    const char *alphabet = uppercase ? "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" :
                                       "0123456789abcdefghijklmnopqrstuvwxyz";
    bool nonzero = value != 0U;
    if (value == 0U && spec->precision == 0) {
        digit_count = 0U;
    } else {
        do {
            digits[digit_count++] = alphabet[value % base];
            value /= base;
        } while (value != 0U);
    }
    if (spec->precision >= 0 &&
        (size_t)spec->precision > digit_count) {
        precision_zeroes = (size_t)spec->precision - digit_count;
    }
    if (spec->alternate && base == 8U &&
        (digit_count == 0U || digits[digit_count - 1U] != '0')) {
        prefix[0] = '0';
        prefix_length = 1U;
    } else if (spec->alternate && base == 16U && nonzero) {
        prefix[0] = '0';
        prefix[1] = uppercase ? 'X' : 'x';
        prefix_length = 2U;
    }
    if (negative) sign[sign_length++] = '-';
    else if (spec->plus) sign[sign_length++] = '+';
    else if (spec->space) sign[sign_length++] = ' ';
    body_length = sign_length + prefix_length + precision_zeroes + digit_count;
    padding = spec->width > body_length ? spec->width - body_length : 0U;
    if (!spec->left && (!spec->zero || spec->precision >= 0)) {
        format_output_repeat(output, ' ', padding);
    }
    for (size_t index = 0U; index < sign_length; ++index) {
        format_output_character(output, sign[index]);
    }
    for (size_t index = 0U; index < prefix_length; ++index) {
        format_output_character(output, prefix[index]);
    }
    if (!spec->left && spec->zero && spec->precision < 0) {
        format_output_repeat(output, '0', padding);
    }
    format_output_repeat(output, '0', precision_zeroes);
    while (digit_count != 0U) format_output_character(output, digits[--digit_count]);
    if (spec->left) format_output_repeat(output, ' ', padding);
}

static uint64_t format_unsigned_argument(va_list arguments,
                                          const format_spec_t *spec) {
    if (spec->length == 'z') return va_arg(arguments, size_t);
    if (spec->length == 'j') return va_arg(arguments, uintmax_t);
    if (spec->length == 't') return (uint64_t)va_arg(arguments, ptrdiff_t);
    if (spec->length == 'l') return va_arg(arguments, unsigned long);
    if (spec->length == 'L') return va_arg(arguments, unsigned long long);
    return va_arg(arguments, unsigned int);
}

static int64_t format_signed_argument(va_list arguments,
                                      const format_spec_t *spec) {
    if (spec->length == 'z') return va_arg(arguments, ssize_t);
    if (spec->length == 'j') return va_arg(arguments, intmax_t);
    if (spec->length == 't') return va_arg(arguments, ptrdiff_t);
    if (spec->length == 'l') return va_arg(arguments, long);
    if (spec->length == 'L') return va_arg(arguments, long long);
    return va_arg(arguments, int);
}

static void format_string(format_output_t *output, const char *text,
                          const format_spec_t *spec) {
    size_t length = text == 0 ? 6U : strlen(text);
    if (text == 0) text = "(null)";
    if (spec->precision >= 0 && length > (size_t)spec->precision) {
        length = (size_t)spec->precision;
    }
    if (spec->width > length && !spec->left) {
        format_output_repeat(output, ' ', spec->width - length);
    }
    format_output_text(output, text, length);
    if (spec->width > length && spec->left) {
        format_output_repeat(output, ' ', spec->width - length);
    }
}

#define LIBC_FLOAT_PRECISION_LIMIT 3000U
#define LIBC_FLOAT_TEXT_LIMIT 8192U

static void float_text_character(char *text, size_t *length, char value) {
    if (*length < LIBC_FLOAT_TEXT_LIMIT - 1U) text[*length] = value;
    if (*length != SIZE_MAX) ++*length;
}

static void float_text_repeat(char *text, size_t *length, char value,
                              size_t count) {
    while (count-- != 0U) float_text_character(text, length, value);
}

static int float_decimal_exponent(long double value) {
    int exponent = 0;
    while (value >= 10.0L && exponent < 10000) {
        value /= 10.0L;
        ++exponent;
    }
    while (value < 1.0L && exponent > -10000) {
        value *= 10.0L;
        --exponent;
    }
    return exponent;
}

static long double float_normalize(long double value, int exponent) {
    while (exponent > 0) {
        value /= 10.0L;
        --exponent;
    }
    while (exponent < 0) {
        value *= 10.0L;
        ++exponent;
    }
    return value;
}

static unsigned int float_take_digit(long double *value) {
    unsigned int digit = (unsigned int)*value;
    if (digit > 9U) digit = 9U;
    *value = (*value - (long double)digit) * 10.0L;
    return digit;
}

static bool float_round_digits(char *digits, size_t count,
                               unsigned int next_digit) {
    size_t index;
    if (next_digit < 5U) return false;
    index = count;
    while (index != 0U) {
        --index;
        if (digits[index] != '9') {
            ++digits[index];
            return false;
        }
        digits[index] = '0';
    }
    return true;
}

static size_t float_fixed_body(char *text, long double value, int precision,
                               bool alternate) {
    char digits[LIBC_FLOAT_TEXT_LIMIT] = {0};
    size_t length = 0U;
    int exponent;
    long double normalized;
    size_t integer_digits;
    size_t count;
    size_t index;
    unsigned int next_digit;
    bool carry;

    if (value == 0.0L) {
        float_text_character(text, &length, '0');
        if (precision > 0 || alternate) {
            float_text_character(text, &length, '.');
            float_text_repeat(text, &length, '0', (size_t)precision);
        }
        return length;
    }
    exponent = float_decimal_exponent(value);
    normalized = float_normalize(value, exponent);
    if (exponent >= 0) {
        integer_digits = (size_t)exponent + 1U;
        count = integer_digits + (size_t)precision;
        if (count >= LIBC_FLOAT_TEXT_LIMIT) count = LIBC_FLOAT_TEXT_LIMIT - 1U;
        for (index = 0U; index < count; ++index) {
            digits[index] = (char)('0' + float_take_digit(&normalized));
        }
        next_digit = float_take_digit(&normalized);
        carry = float_round_digits(digits, count, next_digit);
        if (carry) {
            float_text_character(text, &length, '1');
            float_text_repeat(text, &length, '0', integer_digits);
            if (precision > 0 || alternate) float_text_character(text, &length, '.');
            float_text_repeat(text, &length, '0', (size_t)precision);
            return length;
        }
        for (index = 0U; index < integer_digits && index < count; ++index) {
            float_text_character(text, &length, digits[index]);
        }
        if (precision > 0 || alternate) float_text_character(text, &length, '.');
        for (; index < count; ++index) float_text_character(text, &length, digits[index]);
        if ((size_t)precision > count - (integer_digits < count ? integer_digits : count)) {
            float_text_repeat(text, &length, '0',
                              (size_t)precision - (count - integer_digits));
        }
        return length;
    }

    integer_digits = (size_t)(-exponent - 1);
    count = (size_t)precision + 1U;
    if (count >= LIBC_FLOAT_TEXT_LIMIT) count = LIBC_FLOAT_TEXT_LIMIT - 1U;
    digits[0] = '0';
    for (index = 1U; index < count; ++index) digits[index] = '0';
    if ((size_t)precision > integer_digits) {
        for (index = integer_digits + 1U; index < count; ++index) {
            digits[index] = (char)('0' + float_take_digit(&normalized));
        }
        next_digit = float_take_digit(&normalized);
    } else if ((size_t)precision == integer_digits) {
        next_digit = float_take_digit(&normalized);
    } else {
        next_digit = 0U;
    }
    carry = float_round_digits(digits, count, next_digit);
    if (carry) {
        digits[0] = '1';
        for (index = 1U; index < count; ++index) digits[index] = '0';
    }
    for (index = 0U; index < count; ++index) float_text_character(text, &length, digits[index]);
    if (precision > 0 || alternate) {
        /* 在整数部分与小数部分的零之间插入小数点。 */
        size_t fractional_start = length > 0U ? 1U : 0U;
        size_t tail = length > fractional_start ? length - fractional_start : 0U;
        if (tail != 0U) {
            for (index = length; index > fractional_start; --index) {
                text[index] = text[index - 1U];
            }
        }
        text[fractional_start] = '.';
        ++length;
    }
    if ((size_t)precision > count - 1U) {
        float_text_repeat(text, &length, '0', (size_t)precision - (count - 1U));
    }
    return length;
}

static size_t float_scientific_body(char *text, long double value, int precision,
                                    bool alternate, bool uppercase) {
    char digits[LIBC_FLOAT_TEXT_LIMIT] = {0};
    size_t length = 0U;
    size_t count = (size_t)precision + 1U;
    size_t index;
    int exponent = 0;
    long double normalized;
    unsigned int next_digit;
    bool carry;
    char exponent_digits[8];
    size_t exponent_length = 0U;
    unsigned int exponent_magnitude;

    if (value == 0.0L) {
        float_text_character(text, &length, '0');
        if (precision > 0 || alternate) float_text_character(text, &length, '.');
        float_text_repeat(text, &length, '0', (size_t)precision);
        exponent = 0;
    } else {
        exponent = float_decimal_exponent(value);
        normalized = float_normalize(value, exponent);
        if (count >= LIBC_FLOAT_TEXT_LIMIT) count = LIBC_FLOAT_TEXT_LIMIT - 1U;
        for (index = 0U; index < count; ++index) {
            digits[index] = (char)('0' + float_take_digit(&normalized));
        }
        next_digit = float_take_digit(&normalized);
        carry = float_round_digits(digits, count, next_digit);
        if (carry) {
            ++exponent;
            digits[0] = '1';
            for (index = 1U; index < count; ++index) digits[index] = '0';
        }
        float_text_character(text, &length, digits[0]);
        if (precision > 0 || alternate) float_text_character(text, &length, '.');
        for (index = 1U; index < count; ++index) {
            float_text_character(text, &length, digits[index]);
        }
    }
    float_text_character(text, &length, uppercase ? 'E' : 'e');
    float_text_character(text, &length, exponent < 0 ? '-' : '+');
    exponent_magnitude = (unsigned int)(exponent < 0 ? -exponent : exponent);
    do {
        exponent_digits[exponent_length++] = (char)('0' + exponent_magnitude % 10U);
        exponent_magnitude /= 10U;
    } while (exponent_magnitude != 0U);
    while (exponent_length < 2U) exponent_digits[exponent_length++] = '0';
    while (exponent_length != 0U) {
        float_text_character(text, &length, exponent_digits[--exponent_length]);
    }
    return length;
}

static size_t float_trim_g(char *text, size_t length, bool scientific,
                           char marker) {
    size_t decimal_position;
    size_t marker_position;
    size_t mantissa_end;
    size_t write;
    size_t read;
    if (!scientific) {
        decimal_position = 0U;
        while (decimal_position < length && text[decimal_position] != '.') {
            ++decimal_position;
        }
        if (decimal_position == length) return length;
        while (length > 0U && text[length - 1U] == '0') --length;
        if (length > 0U && text[length - 1U] == '.') --length;
        return length;
    }
    marker_position = 0U;
    while (marker_position < length && text[marker_position] != marker) {
        ++marker_position;
    }
    if (marker_position == length) return length;
    mantissa_end = marker_position;
    while (mantissa_end > 0U && text[mantissa_end - 1U] == '0') --mantissa_end;
    if (mantissa_end > 0U && text[mantissa_end - 1U] == '.') --mantissa_end;
    if (mantissa_end == marker_position) return length;
    write = mantissa_end;
    for (read = marker_position; read < length; ++read) text[write++] = text[read];
    return write;
}

static size_t float_body(char *text, long double value, char conversion,
                         const format_spec_t *spec) {
    bool uppercase = conversion == 'F' || conversion == 'E' || conversion == 'G';
    bool scientific;
    int precision = spec->precision;
    int exponent;
    size_t length;
    if (precision < 0) precision = 6;
    if ((conversion == 'g' || conversion == 'G') && precision == 0) precision = 1;
    /* ponytail：限制异常精度；若兼容性要求支持任意精度格式化，再扩大此缓冲区。 */
    if ((unsigned int)precision > LIBC_FLOAT_PRECISION_LIMIT) {
        precision = (int)LIBC_FLOAT_PRECISION_LIMIT;
    }
    if (__builtin_isnanl(value)) {
        const char *word = uppercase ? "NAN" : "nan";
        text[0] = word[0];
        text[1] = word[1];
        text[2] = word[2];
        text[3] = '\0';
        return 3U;
    }
    if (__builtin_isinfl(value)) {
        const char *word = uppercase ? "INF" : "inf";
        text[0] = word[0]; text[1] = word[1]; text[2] = word[2]; text[3] = '\0';
        return 3U;
    }
    if (conversion == 'f' || conversion == 'F') {
        return float_fixed_body(text, value, precision, spec->alternate);
    }
    if (conversion == 'e' || conversion == 'E') {
        return float_scientific_body(text, value, precision, spec->alternate,
                                     uppercase);
    }
    exponent = value == 0.0L ? 0 : float_decimal_exponent(value);
    scientific = exponent < -4 || exponent >= precision;
    if (scientific) {
        length = float_scientific_body(text, value,
                                       precision > 0 ? precision - 1 : 0,
                                       spec->alternate, uppercase);
    } else {
        int fixed_precision = precision - (exponent + 1);
        length = float_fixed_body(text, value, fixed_precision < 0 ? 0 : fixed_precision,
                                  spec->alternate);
        if (exponent == precision - 1) {
            size_t integer_length = 0U;
            while (integer_length < length && text[integer_length] != '.') {
                ++integer_length;
            }
            if (integer_length > (size_t)precision) {
                scientific = true;
                length = float_scientific_body(text, value,
                                               precision - 1,
                                               spec->alternate, uppercase);
            }
        }
    }
    if (!spec->alternate) {
        length = float_trim_g(text, length, scientific, uppercase ? 'E' : 'e');
    }
    return length;
}

static void format_output_float(format_output_t *output, long double value,
                                char conversion, const format_spec_t *spec) {
    char text[LIBC_FLOAT_TEXT_LIMIT];
    size_t length;
    bool negative = __builtin_signbitl(value);
    format_spec_t body_spec = *spec;
    if (negative) value = -value;
    length = float_body(text, value, conversion, &body_spec);
    if (negative || spec->plus || spec->space) {
        char sign = negative ? '-' : (spec->plus ? '+' : ' ');
        size_t body_length = length + 1U;
        size_t padding = spec->width > body_length ? spec->width - body_length : 0U;
        if (!spec->left && !spec->zero) format_output_repeat(output, ' ', padding);
        format_output_character(output, sign);
        if (!spec->left && spec->zero) format_output_repeat(output, '0', padding);
        format_output_text(output, text, length);
        if (spec->left) format_output_repeat(output, ' ', padding);
    } else {
        size_t padding = spec->width > length ? spec->width - length : 0U;
        if (!spec->left) format_output_repeat(output, spec->zero ? '0' : ' ', padding);
        format_output_text(output, text, length);
        if (spec->left) format_output_repeat(output, ' ', padding);
    }
}

__attribute__((target("sse"))) static void format_output_float_argument(
    format_output_t *output, va_list arguments, char conversion,
    const format_spec_t *spec) {
    long double value = spec->length == 'L' ? va_arg(arguments, long double) :
                                             (long double)va_arg(arguments, double);
    format_output_float(output, value, conversion, spec);
}

static void format_set_length(const char **format, format_spec_t *spec) {
    if (**format == 'h') {
        ++*format;
        if (**format == 'h') {
            ++*format;
            spec->length = 'h';
        } else spec->length = 'H';
    } else if (**format == 'l') {
        ++*format;
        if (**format == 'l') {
            ++*format;
            spec->length = 'L';
        } else spec->length = 'l';
    } else if (**format == 'j' || **format == 'z' || **format == 't' ||
               **format == 'L') {
        spec->length = *(*format)++;
    }
}

int vsnprintf(char *buffer, size_t capacity, const char *format,
              va_list arguments) {
    format_output_t output = {buffer, capacity, 0U};
    if (format == 0 || (buffer == 0 && capacity != 0U)) {
        errno = EINVAL;
        if (buffer != 0 && capacity != 0U) buffer[0] = '\0';
        return -1;
    }
    while (*format != '\0') {
        format_spec_t spec = {0};
        char conversion;
        if (*format++ != '%') {
            format_output_character(&output, format[-1]);
            continue;
        }
        if (*format == '%') {
            format_output_character(&output, *format++);
            continue;
        }
        for (;;) {
            if (*format == '-') spec.left = true;
            else if (*format == '+') spec.plus = true;
            else if (*format == ' ') spec.space = true;
            else if (*format == '#') spec.alternate = true;
            else if (*format == '0') spec.zero = true;
            else break;
            ++format;
        }
        if (*format == '*') {
            int width = va_arg(arguments, int);
            ++format;
            if (width < 0) {
                spec.left = true;
                spec.width = (size_t)(-(width + 1)) + 1U;
            } else spec.width = (size_t)width;
        } else {
            spec.width = parse_format_number(&format);
        }
        spec.precision = -1;
        if (*format == '.') {
            ++format;
            if (*format == '*') {
                spec.precision = va_arg(arguments, int);
                ++format;
                if (spec.precision < 0) spec.precision = -1;
            } else {
                size_t precision = parse_format_number(&format);
                spec.precision = precision > (size_t)INT_MAX ? INT_MAX :
                                 (int)precision;
            }
        }
        format_set_length(&format, &spec);
        conversion = *format == '\0' ? '\0' : *format++;
        switch (conversion) {
        case 'c': {
            char value = (char)va_arg(arguments, int);
            size_t padding = spec.width > 1U ? spec.width - 1U : 0U;
            if (!spec.left) format_output_repeat(&output, ' ', padding);
            format_output_character(&output, value);
            if (spec.left) format_output_repeat(&output, ' ', padding);
            break;
        }
        case 's':
            format_string(&output, va_arg(arguments, const char *), &spec);
            break;
        case 'd':
        case 'i': {
            int64_t value = format_signed_argument(arguments, &spec);
            uint64_t magnitude = value < 0 ?
                (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
            format_output_integer(&output, magnitude, value < 0, 10U, false,
                                  &spec);
            break;
        }
        case 'u':
            format_output_integer(&output,
                                  format_unsigned_argument(arguments, &spec),
                                  false, 10U, false, &spec);
            break;
        case 'o':
            format_output_integer(&output,
                                  format_unsigned_argument(arguments, &spec),
                                  false, 8U, false, &spec);
            break;
        case 'x':
        case 'X':
            format_output_integer(&output,
                                  format_unsigned_argument(arguments, &spec),
                                  false, 16U, conversion == 'X', &spec);
            break;
        case 'p': {
            format_spec_t pointer_spec = spec;
            pointer_spec.alternate = true;
            format_output_integer(&output,
                                  (uint64_t)(uintptr_t)va_arg(arguments, void *),
                                  false, 16U, false, &pointer_spec);
            break;
        }
        case 'n': {
            size_t count = output.length;
            if (spec.length == 'h') *va_arg(arguments, signed char *) = (signed char)count;
            else if (spec.length == 'H') *va_arg(arguments, short *) = (short)count;
            else if (spec.length == 'l') *va_arg(arguments, long *) = (long)count;
            else if (spec.length == 'L') *va_arg(arguments, long long *) = (long long)count;
            else if (spec.length == 'z') *va_arg(arguments, ssize_t *) = (ssize_t)count;
            else *va_arg(arguments, int *) = (int)count;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            format_output_float_argument(&output, arguments, conversion, &spec);
            break;
        case 'a':
        case 'A':
            /* 十六进制浮点格式化暂缓实现，直到有使用方提出需求；目前十进制格式已覆盖
             * libc/运行时 ABI。 */
            format_string(&output, "?", &spec);
            break;
        case '\0':
            format_output_character(&output, '%');
            --format;
            break;
        default:
            format_output_character(&output, '?');
            break;
        }
    }
    if (buffer != 0 && capacity != 0U) {
        size_t end = output.length < capacity ? output.length : capacity - 1U;
        buffer[end] = '\0';
    }
    return output.length > (size_t)INT_MAX ? INT_MAX : (int)output.length;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}

int vsprintf(char *buffer, const char *format, va_list arguments) {
    return vsnprintf(buffer, SIZE_MAX, format, arguments);
}

int sprintf(char *buffer, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vsprintf(buffer, format, arguments);
    va_end(arguments);
    return result;
}

int vasprintf(char **buffer, const char *format, va_list arguments) {
    va_list sizing_arguments;
    va_list output_arguments;
    int length;
    char *result;
    if (buffer == 0) {
        errno = EINVAL;
        return -1;
    }
    va_copy(sizing_arguments, arguments);
    length = vsnprintf(0, 0U, format, sizing_arguments);
    va_end(sizing_arguments);
    if (length < 0 || (size_t)length == SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    result = (char *)malloc((size_t)length + 1U);
    if (result == 0) return -1;
    va_copy(output_arguments, arguments);
    if (vsnprintf(result, (size_t)length + 1U, format, output_arguments) < 0) {
        va_end(output_arguments);
        free(result);
        return -1;
    }
    va_end(output_arguments);
    *buffer = result;
    return length;
}

int asprintf(char **buffer, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vasprintf(buffer, format, arguments);
    va_end(arguments);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list arguments) {
    char *text;
    int length;
    if (!stream_can_write(stream)) {
        errno = EBADF;
        return -1;
    }
    length = vasprintf(&text, format, arguments);
    if (length < 0) return -1;
    if (stream_write_bytes(stream, text, (size_t)length) != (size_t)length) {
        free(text);
        return -1;
    }
    free(text);
    return length;
}

int vdprintf(int descriptor, const char *format, va_list arguments) {
    char *text;
    int length = vasprintf(&text, format, arguments);
    if (length < 0) return -1;
    if (write_all(descriptor, text, (size_t)length) < 0) {
        free(text);
        return -1;
    }
    free(text);
    return length;
}

int fprintf(FILE *stream, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int dprintf(int descriptor, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vdprintf(descriptor, format, arguments);
    va_end(arguments);
    return result;
}

int vprintf(const char *format, va_list arguments) {
    return vfprintf(stdout, format, arguments);
}

int printf(const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

typedef struct scan_source scan_source_t;
struct scan_source {
    int (*get)(scan_source_t *source);
    int (*unget)(scan_source_t *source, int value);
    const char *text;
    size_t position;
    FILE *stream;
    size_t consumed;
};

static int scan_string_get(scan_source_t *source) {
    int value = source->text[source->position];
    if (value == '\0') return EOF;
    ++source->position;
    ++source->consumed;
    return value;
}

static int scan_string_unget(scan_source_t *source, int value) {
    if (value == EOF || source->position == 0U) return -1;
    --source->position;
    --source->consumed;
    return 0;
}

static int scan_file_get(scan_source_t *source) {
    int value = fgetc(source->stream);
    if (value != EOF) ++source->consumed;
    return value;
}

static int scan_file_unget(scan_source_t *source, int value) {
    if (value == EOF || ungetc(value, source->stream) == EOF) return -1;
    --source->consumed;
    return 0;
}

static int scan_skip_space(scan_source_t *source) {
    int value;
    do {
        value = source->get(source);
    } while (value != EOF && isspace(value));
    if (value != EOF) (void)source->unget(source, value);
    return value;
}

static bool scan_set_match(const char *set, size_t length, int value) {
    bool invert = false;
    bool matched = false;
    size_t index = 0U;
    if (index < length && set[index] == '^') {
        invert = true;
        ++index;
    }
    if (index < length && set[index] == ']') {
        matched = value == ']';
        ++index;
    }
    while (index < length) {
        if (index + 2U < length && set[index + 1U] == '-') {
            int first = (unsigned char)set[index];
            int last = (unsigned char)set[index + 2U];
            if (first <= value && value <= last) matched = true;
            index += 3U;
        } else {
            if ((unsigned char)set[index] == value) matched = true;
            ++index;
        }
    }
    return invert ? !matched : matched;
}

static size_t scan_set_length(const char **format) {
    const char *start = *format;
    const char *cursor = start;
    if (*cursor == '^') ++cursor;
    if (*cursor == ']') ++cursor;
    while (*cursor != '\0' && *cursor != ']') ++cursor;
    *format = *cursor == ']' ? cursor + 1U : cursor;
    return (size_t)(cursor - start);
}

static void scan_store_signed(va_list arguments, char length, int64_t value) {
    if (length == 'h') *va_arg(arguments, signed char *) = (signed char)value;
    else if (length == 'H') *va_arg(arguments, short *) = (short)value;
    else if (length == 'l') *va_arg(arguments, long *) = (long)value;
    else if (length == 'L') *va_arg(arguments, long long *) = (long long)value;
    else if (length == 'z') *va_arg(arguments, ssize_t *) = (ssize_t)value;
    else *va_arg(arguments, int *) = (int)value;
}

static void scan_store_unsigned(va_list arguments, char length, uint64_t value) {
    if (length == 'h') *va_arg(arguments, unsigned char *) = (unsigned char)value;
    else if (length == 'H') *va_arg(arguments, unsigned short *) = (unsigned short)value;
    else if (length == 'l') *va_arg(arguments, unsigned long *) = (unsigned long)value;
    else if (length == 'L') *va_arg(arguments, unsigned long long *) = (unsigned long long)value;
    else if (length == 'z') *va_arg(arguments, size_t *) = (size_t)value;
    else *va_arg(arguments, unsigned int *) = (unsigned int)value;
}

static int scan_digit_value(int value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/* 判断 value 是否可扩展已收集的数值词元。尤其是，'-' 仅在开头作为符号；若将其视为
 * 通用候选字符，像 "%lx-%lx" 这样的格式就会把两个地址一并读入。 */
static bool scan_numeric_candidate(int base, const char *token, size_t length,
                                   int value) {
    size_t prefix = 0U;
    int effective_base = base;
    int digit;

    if (value == EOF) return false;
    if (length == 0U && (value == '+' || value == '-')) return true;
    if (length != 0U && (token[0] == '+' || token[0] == '-')) prefix = 1U;

    if (length == prefix && value != '+' && value != '-') {
        if (value == '0' && (base == 0 || base == 8 || base == 10 ||
                             base == 16)) return true;
        digit = scan_digit_value(value);
        return digit >= 0 && (base == 0 || digit < base);
    }

    if ((base == 0 || base == 16) && length == prefix + 1U &&
        token[prefix] == '0' && (value == 'x' || value == 'X')) {
        return true;
    }

    if (base == 0) {
        if (length >= prefix + 2U && token[prefix] == '0' &&
            (token[prefix + 1U] == 'x' || token[prefix + 1U] == 'X')) {
            effective_base = 16;
        } else if (length > prefix && token[prefix] == '0') {
            effective_base = 8;
        } else {
            effective_base = 10;
        }
    }
    digit = scan_digit_value(value);
    return digit >= 0 && digit < effective_base;
}

static int scan_formatted(scan_source_t *source, const char *format,
                          va_list arguments) {
    int assignments = 0;
    bool input_failure = false;
    while (*format != '\0') {
        if (isspace((unsigned char)*format)) {
            while (isspace((unsigned char)*format)) ++format;
            (void)scan_skip_space(source);
            continue;
        }
        if (*format != '%') {
            int value = source->get(source);
            if (value == EOF) {
                input_failure = true;
                break;
            }
            if (value != (unsigned char)*format) {
                (void)source->unget(source, value);
                break;
            }
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            int value = source->get(source);
            if (value != '%') {
                if (value == EOF) input_failure = true;
                else (void)source->unget(source, value);
                break;
            }
            ++format;
            continue;
        }
        bool suppress = false;
        if (*format == '*') {
            suppress = true;
            ++format;
        }
        size_t width = 0U;
        while (*format >= '0' && *format <= '9') {
            unsigned int digit = (unsigned int)(*format++ - '0');
            if (width <= (SIZE_MAX - digit) / 10U) width = width * 10U + digit;
            else width = SIZE_MAX;
        }
        char length = 0;
        if (*format == 'h') {
            length = 'H';
            ++format;
            if (*format == 'h') {
                length = 'h';
                ++format;
            }
        } else if (*format == 'l') {
            length = 'l';
            ++format;
            if (*format == 'l') {
                length = 'L';
                ++format;
            }
        } else if (*format == 'z' || *format == 'j' || *format == 't') {
            length = *format++;
        }
        char conversion = *format == '\0' ? '\0' : *format++;
        if (conversion == 'n') {
            if (!suppress) scan_store_signed(arguments, length,
                                             (int64_t)source->consumed);
            continue;
        }
        if (conversion == 'c') {
            size_t amount = width == 0U ? 1U : width;
            char *destination = suppress ? 0 : va_arg(arguments, char *);
            size_t count = 0U;
            while (count < amount) {
                int value = source->get(source);
                if (value == EOF) {
                    input_failure = true;
                    break;
                }
                if (destination != 0) destination[count] = (char)value;
                ++count;
            }
            if (count != amount) break;
            if (!suppress) ++assignments;
            continue;
        }
        if (conversion == 's' || conversion == '[') {
            const char *set_start = format;
            size_t set_length = conversion == '[' ? scan_set_length(&format) : 0U;
            char *destination = suppress ? 0 : va_arg(arguments, char *);
            size_t count = 0U;
            if (conversion == 's') (void)scan_skip_space(source);
            for (;;) {
                int value;
                if (width != 0U && count >= width) break;
                value = source->get(source);
                if (value == EOF) break;
                if ((conversion == 's' && isspace(value)) ||
                    (conversion == '[' && !scan_set_match(set_start,
                                                            set_length, value))) {
                    (void)source->unget(source, value);
                    break;
                }
                if (destination != 0) destination[count] = (char)value;
                ++count;
            }
            if (count == 0U) {
                if (source->consumed == 0U) input_failure = true;
                break;
            }
            if (destination != 0) destination[count] = '\0';
            if (!suppress) ++assignments;
            continue;
        }
        if (conversion == 'd' || conversion == 'i' || conversion == 'u' ||
            conversion == 'o' || conversion == 'x' || conversion == 'X' ||
            conversion == 'p') {
            char token[128];
            size_t count = 0U;
            char *end;
            int base = conversion == 'o' ? 8 :
                       (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 :
                       (conversion == 'i' ? 0 : 10);
            (void)scan_skip_space(source);
            while (count + 1U < sizeof(token) &&
                   (width == 0U || count < width)) {
                int value = source->get(source);
                bool candidate = scan_numeric_candidate(base, token, count,
                                                         value);
                if (value == EOF || !candidate) {
                    if (value != EOF) (void)source->unget(source, value);
                    break;
                }
                token[count++] = (char)value;
            }
            token[count] = '\0';
            if (count == 0U) {
                input_failure = source->consumed == 0U;
                break;
            }
            if (conversion == 'd' || conversion == 'i') {
                int64_t value = strtoll(token, &end, base);
                if (end == token) break;
                if (!suppress) {
                    scan_store_signed(arguments, length, value);
                    ++assignments;
                }
            } else {
                uint64_t value = strtoull(token, &end, base);
                if (end == token) break;
                if (conversion == 'p') {
                    if (!suppress) {
                        *va_arg(arguments, void **) = (void *)(uintptr_t)value;
                        ++assignments;
                    }
                } else if (!suppress) {
                    scan_store_unsigned(arguments, length, value);
                    ++assignments;
                }
            }
            continue;
        }
        errno = EINVAL;
        break;
    }
    return assignments == 0 && input_failure ? EOF : assignments;
}

int vsscanf(const char *text, const char *format, va_list arguments) {
    scan_source_t source = {
        .get = scan_string_get,
        .unget = scan_string_unget,
        .text = text,
    };
    if (text == 0 || format == 0) {
        errno = EINVAL;
        return EOF;
    }
    return scan_formatted(&source, format, arguments);
}

int sscanf(const char *text, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vsscanf(text, format, arguments);
    va_end(arguments);
    return result;
}

int vfscanf(FILE *stream, const char *format, va_list arguments) {
    scan_source_t source = {
        .get = scan_file_get,
        .unget = scan_file_unget,
        .stream = stream,
    };
    if (!stream_can_read(stream) || format == 0) {
        errno = EINVAL;
        return EOF;
    }
    return scan_formatted(&source, format, arguments);
}

int fscanf(FILE *stream, const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vfscanf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int vscanf(const char *format, va_list arguments) {
    return vfscanf(stdin, format, arguments);
}

int scanf(const char *format, ...) {
    int result;
    va_list arguments;
    va_start(arguments, format);
    result = vscanf(format, arguments);
    va_end(arguments);
    return result;
}

void perror(const char *prefix) {
    int saved_errno = errno;
    if (prefix != 0 && prefix[0] != '\0') {
        (void)fputs(prefix, stderr);
        (void)fputs(": ", stderr);
    }
    (void)fputs(strerror(saved_errno), stderr);
    (void)fputc('\n', stderr);
    errno = saved_errno;
}

int remove(const char *path) {
    struct stat status;
    if (stat(path, &status) < 0) return -1;
    return S_ISDIR(status.st_mode) ? rmdir(path) : unlink(path);
}

FILE *tmpfile(void) {
    char path[LIBC_TMPNAM_SIZE] = "/tmp/tmp-XXXXXX";
    int descriptor = mkstemp(path);
    FILE *stream;
    if (descriptor < 0) return 0;
    stream = fdopen(descriptor, "w+");
    if (stream == 0) {
        (void)close(descriptor);
        (void)unlink(path);
        return 0;
    }
    stream->temporary_path = strdup(path);
    if (stream->temporary_path == 0) {
        int saved_errno = errno;
        (void)fclose(stream);
        (void)unlink(path);
        errno = saved_errno;
        return 0;
    }
    return stream;
}

void __libc_close_temporary_files(void) {
    FILE *stream = g_streams;
    while (stream != 0) {
        FILE *next = stream->next;
        if (stream->temporary_path != 0) (void)fclose(stream);
        stream = next;
    }
}

char *tmpnam(char *buffer) {
    static unsigned int sequence;
    static char storage[LIBC_TMPNAM_SIZE];
    char *result = buffer != 0 ? buffer : storage;
    if (snprintf(result, LIBC_TMPNAM_SIZE, "/tmp/tmp-%u", sequence++) < 0) {
        errno = EOVERFLOW;
        return 0;
    }
    return result;
}

ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream) {
    size_t length = 0U;
    if (line == 0 || capacity == 0 || !stream_is_open(stream)) {
        errno = EINVAL;
        return -1;
    }
    if (*line == 0 || *capacity == 0U) {
        size_t initial = *capacity < 128U ? 128U : *capacity;
        *line = (char *)realloc(*line, initial);
        if (*line == 0) {
            *capacity = 0U;
            errno = ENOMEM;
            return -1;
        }
        *capacity = initial;
    }
    for (;;) {
        int value = fgetc(stream);
        if (value == EOF) {
            if (length == 0U) return -1;
            break;
        }
        if (length + 1U >= *capacity) {
            size_t replacement;
            char *resized;
            if (*capacity > SIZE_MAX / 2U) {
                errno = EOVERFLOW;
                return -1;
            }
            replacement = *capacity * 2U;
            resized = (char *)realloc(*line, replacement);
            if (resized == 0) return -1;
            *line = resized;
            *capacity = replacement;
        }
        (*line)[length++] = (char)value;
        if ((unsigned char)value == (unsigned char)delimiter) break;
    }
    if (length + 1U >= *capacity) {
        char *resized = (char *)realloc(*line, length + 1U);
        if (resized == 0) return -1;
        *line = resized;
        *capacity = length + 1U;
    }
    (*line)[length] = '\0';
    if (length > (size_t)INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (ssize_t)length;
}

ssize_t getline(char **line, size_t *capacity, FILE *stream) {
    return getdelim(line, capacity, '\n', stream);
}

int fseeko(FILE *stream, off_t offset, int whence) {
    return stream_seek(stream, offset, whence);
}

off_t ftello(FILE *stream) {
    off_t position;
    if (stream_position(stream, &position) < 0) return -1;
    return position;
}

int getc_unlocked(FILE *stream) {
    return fgetc(stream);
}

int getchar_unlocked(void) {
    return fgetc(stdin);
}

int putc_unlocked(int value, FILE *stream) {
    return fputc(value, stream);
}

int putchar_unlocked(int value) {
    return fputc(value, stdout);
}

char *fgets_unlocked(char *buffer, int capacity, FILE *stream) {
    return fgets(buffer, capacity, stream);
}

int fputs_unlocked(const char *text, FILE *stream) {
    return fputs(text, stream);
}

int fputc_unlocked(int value, FILE *stream) {
    return fputc(value, stream);
}

size_t fread_unlocked(void *buffer, size_t size, size_t count, FILE *stream) {
    return fread(buffer, size, count, stream);
}

size_t fwrite_unlocked(const void *buffer, size_t size, size_t count,
                       FILE *stream) {
    return fwrite(buffer, size, count, stream);
}

int fflush_unlocked(FILE *stream) {
    return fflush(stream);
}

/* 导入的 ISO C99 名称共用现有扫描器实现。导出这些符号并不代表兼容 glibc FILE 布局
 * 或符号版本。 */
extern __typeof__(sscanf) __isoc99_sscanf __attribute__((alias("sscanf")));
extern __typeof__(vsscanf) __isoc99_vsscanf __attribute__((alias("vsscanf")));
extern __typeof__(fscanf) __isoc99_fscanf __attribute__((alias("fscanf")));
extern __typeof__(vfscanf) __isoc99_vfscanf __attribute__((alias("vfscanf")));
extern __typeof__(scanf) __isoc99_scanf __attribute__((alias("scanf")));
extern __typeof__(vscanf) __isoc99_vscanf __attribute__((alias("vscanf")));
