#include "libc/libc.h"

#include <dirent.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>

typedef struct glob_context {
    int flags;
    int (*error_function)(const char *, int);
    glob_t *result;
} glob_context_t;

static bool glob_has_magic(const char *pattern, int flags) {
    while (pattern != 0 && *pattern != '\0') {
        if (*pattern == '\\' && (flags & GLOB_NOESCAPE) == 0 &&
            pattern[1] != '\0') {
            pattern += 2;
            continue;
        }
        if (*pattern == '*' || *pattern == '?' || *pattern == '[') return true;
        ++pattern;
    }
    return false;
}

static int glob_join_path(const char *prefix, const char *component,
                          size_t component_length, char output[PATH_MAX]) {
    size_t prefix_length = strlen(prefix);
    size_t length = prefix_length;
    if (prefix_length == 0U) {
        if (component_length + 1U > PATH_MAX) return -1;
        memcpy(output, component, component_length);
        output[component_length] = '\0';
        return 0;
    }
    if (prefix_length == 1U && prefix[0] == '/') {
        if (component_length + 2U > PATH_MAX) return -1;
        output[0] = '/';
        memcpy(output + 1U, component, component_length);
        output[component_length + 1U] = '\0';
        return 0;
    }
    if (length + 1U + component_length + 1U > PATH_MAX) return -1;
    memcpy(output, prefix, length);
    output[length++] = '/';
    memcpy(output + length, component, component_length);
    output[length + component_length] = '\0';
    return 0;
}

static int glob_append_match(glob_context_t *context, const char *path,
                             bool directory) {
    glob_t *result = context->result;
    size_t path_length = strlen(path);
    bool add_slash = directory && (context->flags & GLOB_MARK) != 0 &&
                     (path_length == 0U || path[path_length - 1U] != '/');
    size_t slot;
    size_t vector_count;
    char **resized;
    char *copy;

    if (path_length > SIZE_MAX - (add_slash ? 2U : 1U)) {
        errno = EOVERFLOW;
        return GLOB_NOSPACE;
    }
    copy = (char *)malloc(path_length + (add_slash ? 2U : 1U));
    if (copy == 0) return GLOB_NOSPACE;
    memcpy(copy, path, path_length);
    if (add_slash) copy[path_length++] = '/';
    copy[path_length] = '\0';
    if (result->gl_pathc > SIZE_MAX - result->gl_offs - 2U) {
        free(copy);
        errno = EOVERFLOW;
        return GLOB_NOSPACE;
    }
    slot = result->gl_offs + result->gl_pathc;
    vector_count = slot + 2U;
    if (vector_count > SIZE_MAX / sizeof(*result->gl_pathv)) {
        free(copy);
        errno = EOVERFLOW;
        return GLOB_NOSPACE;
    }
    resized = (char **)realloc(result->gl_pathv,
                                vector_count * sizeof(*result->gl_pathv));
    if (resized == 0) {
        free(copy);
        return GLOB_NOSPACE;
    }
    result->gl_pathv = resized;
    if (result->gl_pathc == 0U) {
        size_t index;
        for (index = 0U; index < result->gl_offs; ++index) {
            result->gl_pathv[index] = 0;
        }
    }
    result->gl_pathv[slot] = copy;
    result->gl_pathv[slot + 1U] = 0;
    ++result->gl_pathc;
    return 0;
}

static int glob_report_error(glob_context_t *context, const char *path) {
    int error_number = errno;
    if (context->error_function != 0 &&
        context->error_function(path, error_number) != 0) {
        return GLOB_ABORTED;
    }
    return (context->flags & GLOB_ERR) != 0 ? GLOB_ABORTED : 0;
}

static int glob_expand(glob_context_t *context, const char *prefix,
                       const char *pattern) {
    const char *component_end;
    const char *rest;
    size_t component_length;
    bool has_magic;
    bool has_rest;
    char component[PATH_MAX];
    char next_path[PATH_MAX];
    struct stat status;

    while (*pattern == '/') ++pattern;
    if (*pattern == '\0') {
        /* 空模式不等同于当前目录。保留无匹配项路径，供下方的 GLOB_NOCHECK/GLOB_NOMAGIC 使用。 */
        if (prefix[0] == '\0') return 0;
        const char *path = prefix[0] == '\0' ? "." : prefix;
        if (stat(path, &status) < 0) return 0;
        return glob_append_match(context, prefix[0] == '\0' ? "." : prefix,
                                 S_ISDIR(status.st_mode));
    }
    component_end = strchr(pattern, '/');
    component_length = component_end == 0 ? strlen(pattern) :
                       (size_t)(component_end - pattern);
    if (component_length == 0U || component_length >= sizeof(component)) {
        errno = ENAMETOOLONG;
        return GLOB_NOSPACE;
    }
    memcpy(component, pattern, component_length);
    component[component_length] = '\0';
    rest = component_end == 0 ? 0 : component_end + 1;
    while (rest != 0 && *rest == '/') ++rest;
    has_rest = rest != 0 && *rest != '\0';
    has_magic = glob_has_magic(component, context->flags);
    if (!has_magic) {
        if (glob_join_path(prefix, component, component_length, next_path) < 0) {
            errno = ENAMETOOLONG;
            return GLOB_NOSPACE;
        }
        if (stat(next_path, &status) < 0) return 0;
        if (has_rest) {
            if (!S_ISDIR(status.st_mode)) return 0;
            return glob_expand(context, next_path, rest);
        }
        return glob_append_match(context, next_path, S_ISDIR(status.st_mode));
    }

    {
        const char *directory_path = prefix[0] == '\0' ? "." : prefix;
        DIR *directory = opendir(directory_path);
        struct dirent *entry;
        int status_code;
        int match_flags = (context->flags & GLOB_NOESCAPE) != 0 ? FNM_NOESCAPE : 0;
        if ((context->flags & GLOB_PERIOD) == 0) match_flags |= FNM_PERIOD;
        if (directory == 0) return glob_report_error(context, directory_path);
        while ((entry = readdir(directory)) != 0) {
            if (fnmatch(component, entry->d_name, match_flags) != 0) continue;
            if (glob_join_path(prefix, entry->d_name, strlen(entry->d_name),
                               next_path) < 0) {
                (void)closedir(directory);
                errno = ENAMETOOLONG;
                return GLOB_NOSPACE;
            }
            if (stat(next_path, &status) < 0) continue;
            if (has_rest) {
                if (!S_ISDIR(status.st_mode)) continue;
                status_code = glob_expand(context, next_path, rest);
            } else {
                status_code = glob_append_match(context, next_path,
                                                S_ISDIR(status.st_mode));
            }
            if (status_code != 0) {
                (void)closedir(directory);
                return status_code;
            }
        }
        if (errno != 0 && errno != ENOENT) {
            status_code = glob_report_error(context, directory_path);
            if (status_code != 0) {
                (void)closedir(directory);
                return status_code;
            }
        }
        (void)closedir(directory);
    }
    return 0;
}

static int glob_compare(const void *left, const void *right) {
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

int glob(const char *pattern, int flags,
         int (*error_function)(const char *, int), glob_t *result) {
    char prefix[PATH_MAX] = {0};
    const char *components;
    size_t initial_count;
    size_t added_count;
    bool magic;
    int status;
    const int supported = GLOB_ERR | GLOB_MARK | GLOB_NOSORT | GLOB_DOOFFS |
                          GLOB_NOCHECK | GLOB_APPEND | GLOB_NOESCAPE |
                          GLOB_NOMAGIC | GLOB_PERIOD;

    if (pattern == 0 || result == 0 || (flags & ~supported) != 0) {
        errno = EINVAL;
        return GLOB_ABORTED;
    }
    if ((flags & GLOB_APPEND) == 0) {
        result->gl_pathc = 0U;
        result->gl_pathv = 0;
        if ((flags & GLOB_DOOFFS) == 0) result->gl_offs = 0U;
    }
    if ((flags & GLOB_DOOFFS) == 0) result->gl_offs = 0U;
    initial_count = result->gl_pathc;
    components = pattern;
    if (*components == '/') {
        prefix[0] = '/';
        prefix[1] = '\0';
        while (*components == '/') ++components;
    }
    magic = glob_has_magic(pattern, flags);
    {
        glob_context_t context = {
            .flags = flags,
            .error_function = error_function,
            .result = result,
        };
        status = glob_expand(&context, prefix, components);
    }
    if (status != 0) {
        globfree(result);
        return status;
    }
    if (result->gl_pathc == initial_count) {
        if ((flags & GLOB_NOCHECK) != 0 ||
            ((flags & GLOB_NOMAGIC) != 0 && !magic)) {
            glob_context_t context = {
                .flags = flags,
                .error_function = error_function,
                .result = result,
            };
            status = glob_append_match(&context, pattern, false);
            if (status != 0) {
                globfree(result);
                return status;
            }
        } else {
            return GLOB_NOMATCH;
        }
    }
    added_count = result->gl_pathc - initial_count;
    if ((flags & GLOB_NOSORT) == 0 && added_count > 1U) {
        qsort(result->gl_pathv + result->gl_offs + initial_count,
              added_count, sizeof(*result->gl_pathv), glob_compare);
    }
    return 0;
}

void globfree(glob_t *result) {
    size_t index;
    if (result == 0) return;
    if (result->gl_pathv != 0) {
        for (index = 0U; index < result->gl_offs + result->gl_pathc; ++index) {
            free(result->gl_pathv[index]);
        }
        free(result->gl_pathv);
    }
    result->gl_pathc = 0U;
    result->gl_pathv = 0;
    result->gl_offs = 0U;
}
