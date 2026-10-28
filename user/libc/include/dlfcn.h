#pragma once

/* Chromium POSIX 代码使用的动态加载器接口。System 当前通过进程加载器解析自身共享对象；
 * 这些取值使标准接口保持源代码兼容，而其余加载器
 * entry points are added to the libc ABI. */
#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100
#define RTLD_NOLOAD 0x0004
#define RTLD_NODELETE 0x1000
#define RTLD_DEEPBIND 0x0008
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1L)

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

#ifdef __cplusplus
extern "C" {
#endif
void *dlopen(const char *file, int mode);
int dlclose(void *handle);
void *dlsym(void *handle, const char *name);
const char *dlerror(void);
int dladdr(const void *address, Dl_info *info);
#ifdef __cplusplus
}
#endif
