#include "libc/libc.h"

#include <elf.h>
#include <uapi/linux_abi.h>

#define LIBC_ENVIRONMENT_INITIAL_CAPACITY 16U
#define LIBC_INIT_AT_PHDR 3U
#define LIBC_INIT_AT_PHENT 4U
#define LIBC_INIT_AT_PHNUM 5U
#define LIBC_INIT_AT_NULL 0U
#define LIBC_INIT_PT_PHDR 6U
#define LIBC_INIT_DT_NULL 0LL
#define LIBC_INIT_DT_INIT 12LL
#define LIBC_INIT_DT_INIT_ARRAY 25LL
#define LIBC_INIT_DT_INIT_ARRAYSZ 27LL
#define LIBC_INIT_MAX_AUXV 128U
#define LIBC_INIT_MAX_PHDR 128U
#define LIBC_INIT_MAX_ARGC 4096U
#define LIBC_INIT_MAX_DYNAMIC_SIZE (64U * 1024U)

typedef void (*libc_initializer_t)(int argc, char **argv, char **envp);

static char *g_empty_environment[1] = {0};
static bool g_environment_owned;
static size_t g_environment_capacity;
static bool g_main_initializers_ran;
static int g_main_initializers_argc;
static char **g_main_initializers_argv;
static char **g_main_initializers_environment;

char **environ = g_empty_environment;

static bool environment_add_u64(uint64_t left, uint64_t right,
                                uint64_t *result) {
    if (result == 0 || left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool environment_range_in_load_segment(
    const Elf64_Phdr *program_headers, uint64_t program_header_count,
    uint64_t load_bias, uint64_t address, uint64_t size,
    uint32_t required_flags) {
    for (uint64_t index = 0U; index < program_header_count; ++index) {
        const Elf64_Phdr *program = &program_headers[index];
        uint64_t start;
        uint64_t end;
        if (program->p_type != PT_LOAD ||
            (program->p_flags & required_flags) != required_flags ||
            !environment_add_u64(load_bias, program->p_vaddr, &start) ||
            !environment_add_u64(start, program->p_memsz, &end)) continue;
        if (address >= start && address <= end && size <= end - address) {
            return true;
        }
    }
    return false;
}

static bool environment_executable_address(const Elf64_Phdr *program_headers,
                                           uint64_t program_header_count,
                                           uint64_t load_bias,
                                           uint64_t address) {
    return environment_range_in_load_segment(program_headers,
        program_header_count, load_bias, address, 1U, PF_X);
}

/* envp 位于内核构造的初始栈中，布局为 argc、argv[]、NULL、envp[]。
 * 向后遍历可恢复旧版单参数 Chrome CRT 钩子所需的 argc 和 argv。 */
static int environment_recover_startup_arguments(char **environment,
                                                int *argc_out,
                                                char ***argv_out) {
    const uintptr_t *words = (const uintptr_t *)(uintptr_t)environment;
    if (environment == 0 || argc_out == 0 || argv_out == 0 || words[-1] != 0U) {
        return -1;
    }
    for (uint32_t count = 0U; count < LIBC_INIT_MAX_ARGC; ++count) {
        uintptr_t value = words[-2 - (int64_t)count];
        if (value <= LIBC_INIT_MAX_ARGC) {
            if (value != count) return -1;
            *argc_out = (int)count;
            *argv_out = (char **)(uintptr_t)(words - (size_t)count - 1U);
            return 0;
        }
    }
    return -1;
}

static int environment_run_main_initializers(int argc, char **argv,
                                            char **environment) {
    Elf64_auxv_t *auxiliary;
    const Elf64_Phdr *program_headers = 0;
    uint64_t program_header_count = 0U;
    uint64_t dynamic_vaddr = 0U;
    uint64_t dynamic_size = 0U;
    uint64_t load_bias = 0U;
    uint64_t init_vaddr = 0U;
    uint64_t init_array_vaddr = 0U;
    uint64_t init_array_size = 0U;
    uint32_t environment_count = 0U;
    bool have_phdr = false;
    bool have_phent = false;
    bool have_phnum = false;
    bool have_auxiliary_null = false;
    bool have_load_bias = false;
    bool have_dynamic = false;
    bool have_init_array = false;
    bool have_init_array_size = false;

    if (argc < 0 || argv == 0 || environment == 0) return -1;
    if (g_main_initializers_ran) {
        return argc == g_main_initializers_argc &&
                       argv == g_main_initializers_argv &&
                       environment == g_main_initializers_environment
                   ? 0 : -1;
    }
    while (environment_count < 4096U && environment[environment_count] != 0) {
        ++environment_count;
    }
    if (environment_count == 4096U) return -1;
    auxiliary = (Elf64_auxv_t *)(uintptr_t)(environment + environment_count + 1U);
    for (uint32_t index = 0U; index < LIBC_INIT_MAX_AUXV; ++index) {
        uint64_t type = auxiliary[index].a_type;
        uint64_t value = auxiliary[index].a_un.a_val;
        if (type == LIBC_INIT_AT_NULL) {
            have_auxiliary_null = true;
            break;
        }
        if (type == LIBC_INIT_AT_PHDR) {
            program_headers = (const Elf64_Phdr *)(uintptr_t)value;
            have_phdr = true;
        } else if (type == LIBC_INIT_AT_PHENT) {
            have_phent = value == sizeof(Elf64_Phdr);
        } else if (type == LIBC_INIT_AT_PHNUM) {
            program_header_count = value;
            have_phnum = true;
        }
    }
    if (!have_auxiliary_null || !have_phdr || !have_phent || !have_phnum ||
        program_headers == 0 || program_header_count == 0U ||
        program_header_count > LIBC_INIT_MAX_PHDR) return -1;

    for (uint64_t index = 0U; index < program_header_count; ++index) {
        const Elf64_Phdr *program = &program_headers[index];
        if (program->p_type == LIBC_INIT_PT_PHDR) {
            if ((uint64_t)(uintptr_t)program_headers < program->p_vaddr) {
                return -1;
            }
            load_bias = (uint64_t)(uintptr_t)program_headers - program->p_vaddr;
            have_load_bias = true;
        } else if (program->p_type == PT_DYNAMIC) {
            dynamic_vaddr = program->p_vaddr;
            dynamic_size = program->p_memsz;
            have_dynamic = true;
        }
    }
    if (!have_load_bias || !have_dynamic ||
        dynamic_size < sizeof(Elf64_Dyn) ||
        dynamic_size > LIBC_INIT_MAX_DYNAMIC_SIZE ||
        dynamic_size % sizeof(Elf64_Dyn) != 0U) return -1;
    uint64_t dynamic_address;
    if (!environment_add_u64(load_bias, dynamic_vaddr, &dynamic_address) ||
        !environment_range_in_load_segment(program_headers,
            program_header_count, load_bias, dynamic_address, dynamic_size,
            4U)) return -1;
    const Elf64_Dyn *dynamic = (const Elf64_Dyn *)(uintptr_t)dynamic_address;
    uint64_t dynamic_count = dynamic_size / sizeof(Elf64_Dyn);
    bool have_dynamic_null = false;
    for (uint64_t index = 0U; index < dynamic_count; ++index) {
        if (dynamic[index].d_tag == LIBC_INIT_DT_NULL) {
            have_dynamic_null = true;
            break;
        }
        if (dynamic[index].d_tag == LIBC_INIT_DT_INIT) {
            init_vaddr = dynamic[index].d_un.d_ptr;
        } else if (dynamic[index].d_tag == LIBC_INIT_DT_INIT_ARRAY) {
            init_array_vaddr = dynamic[index].d_un.d_ptr;
            have_init_array = true;
        } else if (dynamic[index].d_tag == LIBC_INIT_DT_INIT_ARRAYSZ) {
            init_array_size = dynamic[index].d_un.d_val;
            have_init_array_size = true;
        }
    }
    if (!have_dynamic_null || have_init_array != have_init_array_size ||
        init_array_size % sizeof(uint64_t) != 0U ||
        init_array_size > LIBC_INIT_MAX_DYNAMIC_SIZE) return -1;

    uint64_t init_address = 0U;
    uint64_t init_array_address = 0U;
    const uint64_t *initializers = 0;
    if (init_vaddr != 0U &&
        (!environment_add_u64(load_bias, init_vaddr, &init_address) ||
         !environment_executable_address(program_headers,
             program_header_count, load_bias, init_address))) return -1;
    if (init_array_size != 0U) {
        if (!have_init_array ||
            !environment_add_u64(load_bias, init_array_vaddr,
                                 &init_array_address) ||
            !environment_range_in_load_segment(program_headers,
                program_header_count, load_bias, init_array_address,
                init_array_size, 4U)) return -1;
        initializers = (const uint64_t *)(uintptr_t)init_array_address;
        for (uint64_t index = 0U;
             index < init_array_size / sizeof(uint64_t); ++index) {
            uint64_t function = initializers[index];
            if (function != 0U &&
                !environment_executable_address(program_headers,
                    program_header_count, load_bias, function)) return -1;
        }
    }

    g_main_initializers_argc = argc;
    g_main_initializers_argv = argv;
    g_main_initializers_environment = environment;
    g_main_initializers_ran = true;
    if (init_address != 0U) {
        ((libc_initializer_t)(uintptr_t)init_address)(argc, argv, environment);
    }
    for (uint64_t index = 0U;
         index < init_array_size / sizeof(uint64_t); ++index) {
        uint64_t function = initializers[index];
        if (function != 0U) {
            ((libc_initializer_t)(uintptr_t)function)(argc, argv, environment);
        }
    }
    return 0;
}

static void environment_startup_debug_mark(const char *text, size_t length) {
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
}

static size_t environment_count(void) {
    size_t count = 0U;
    if (environ == 0) return 0U;
    while (environ[count] != 0) ++count;
    return count;
}

static bool valid_name(const char *name) {
    return name != 0 && name[0] != '\0' && strchr(name, '=') == 0;
}

static bool entry_matches(const char *entry, const char *name,
                          size_t name_length) {
    return entry != 0 && strncmp(entry, name, name_length) == 0 &&
           entry[name_length] == '=';
}

static int find_entry(const char *name, size_t name_length) {
    size_t count = environment_count();
    for (size_t index = 0U; index < count; ++index) {
        if (entry_matches(environ[index], name, name_length)) return (int)index;
    }
    return -1;
}

static void free_owned_environment(char **environment, size_t count) {
    if (!environment) return;
    for (size_t index = 0U; index < count; ++index) free(environment[index]);
    free(environment);
}

static int make_environment_owned(size_t extra_entries) {
    size_t count = environment_count();
    size_t required = count + extra_entries + 1U;
    size_t capacity = g_environment_capacity;
    char **replacement;

    if (required < count || required > SIZE_MAX / sizeof(*replacement)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (g_environment_owned && capacity >= required) return 0;
    if (!g_environment_owned) {
        capacity = LIBC_ENVIRONMENT_INITIAL_CAPACITY;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        replacement = (char **)malloc(capacity * sizeof(*replacement));
        if (replacement == 0) return -1;
        for (size_t index = 0U; index < count; ++index) {
            replacement[index] = strdup(environ[index]);
            if (replacement[index] == 0) {
                for (size_t cleanup = 0U; cleanup < index; ++cleanup) {
                    free(replacement[cleanup]);
                }
                free(replacement);
                return -1;
            }
        }
        replacement[count] = 0;
        environ = replacement;
        g_environment_owned = true;
        g_environment_capacity = capacity;
        return 0;
    }
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    replacement = (char **)realloc(environ, capacity * sizeof(*replacement));
    if (replacement == 0) return -1;
    environ = replacement;
    g_environment_capacity = capacity;
    return 0;
}

void __libc_init_environment_main(int argc, char **argv, char **environment) {
    if (g_environment_owned) {
        free_owned_environment(environ, environment_count());
    }
    environ = environment != 0 ? environment : g_empty_environment;
    g_environment_owned = false;
    g_environment_capacity = 0U;
    if (environment_run_main_initializers(argc, argv, environment) != 0) {
        environment_startup_debug_mark("OS_LIBC_MAIN_INIT_FAIL\n",
            sizeof("OS_LIBC_MAIN_INIT_FAIL\n") - 1U);
        _exit(127);
    }
    environment_startup_debug_mark("OS_LIBC_MAIN_INIT_OK\n",
        sizeof("OS_LIBC_MAIN_INIT_OK\n") - 1U);
}

void __libc_init_environment(char **environment) {
    int argc;
    char **argv;
    if (environment_recover_startup_arguments(environment, &argc, &argv) != 0) {
        environment_startup_debug_mark("OS_LIBC_MAIN_INIT_FAIL\n",
            sizeof("OS_LIBC_MAIN_INIT_FAIL\n") - 1U);
        _exit(127);
    }
    __libc_init_environment_main(argc, argv, environment);
}

char *getenv(const char *name) {
    size_t length;
    int index;
    if (!valid_name(name)) return 0;
    length = strlen(name);
    index = find_entry(name, length);
    return index < 0 ? 0 : environ[index] + length + 1U;
}

char *secure_getenv(const char *name) {
    /* System has no set-user-ID transition or privilege-changing exec path. */
    return getenv(name);
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t name_length;
    size_t value_length;
    size_t total;
    int index;
    char *assignment;

    if (!valid_name(name) || value == 0) {
        errno = EINVAL;
        return -1;
    }
    name_length = strlen(name);
    value_length = strlen(value);
    if (name_length > SIZE_MAX - 2U ||
        value_length > SIZE_MAX - name_length - 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    total = name_length + value_length + 2U;
    index = find_entry(name, name_length);
    if (index >= 0 && !overwrite) return 0;
    assignment = (char *)malloc(total);
    if (assignment == 0) return -1;
    memcpy(assignment, name, name_length);
    assignment[name_length] = '=';
    memcpy(assignment + name_length + 1U, value, value_length + 1U);
    if (make_environment_owned(index < 0 ? 1U : 0U) < 0) {
        free(assignment);
        return -1;
    }
    if (index >= 0) {
        free(environ[index]);
        environ[index] = assignment;
    } else {
        size_t count = environment_count();
        environ[count] = assignment;
        environ[count + 1U] = 0;
    }
    return 0;
}

int putenv(char *assignment) {
    char *separator;
    size_t name_length;
    int index;
    char *copy;
    if (assignment == 0 || assignment[0] == '\0' ||
        (separator = strchr(assignment, '=')) == 0 || separator == assignment) {
        errno = EINVAL;
        return -1;
    }
    name_length = (size_t)(separator - assignment);
    index = find_entry(assignment, name_length);
    copy = strdup(assignment);
    if (copy == 0) return -1;
    if (make_environment_owned(index < 0 ? 1U : 0U) < 0) {
        free(copy);
        return -1;
    }
    if (index >= 0) {
        free(environ[index]);
        environ[index] = copy;
    } else {
        size_t count = environment_count();
        environ[count] = copy;
        environ[count + 1U] = 0;
    }
    return 0;
}

int unsetenv(const char *name) {
    size_t name_length;
    size_t count;
    if (!valid_name(name)) {
        errno = EINVAL;
        return -1;
    }
    name_length = strlen(name);
    if (make_environment_owned(0U) < 0) return -1;
    count = environment_count();
    for (size_t index = 0U; index < count;) {
        if (!entry_matches(environ[index], name, name_length)) {
            ++index;
            continue;
        }
        free(environ[index]);
        for (size_t move = index + 1U; move <= count; ++move) {
            environ[move - 1U] = environ[move];
        }
        --count;
    }
    return 0;
}
