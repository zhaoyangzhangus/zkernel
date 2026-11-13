/* 阶段 9：加载真实的 595.84 GLVND 和 NVIDIA EGL 供应商库。 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define P9_RTLD_NOW 2

extern void *dlopen(const char *filename, int flags);
extern int dlclose(void *handle);

static void p9_emit(const char *text)
{
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static int p9_fail(const char *text)
{
    p9_emit(text);
    return 1;
}

int main(void)
{
    void *egl = 0;
    void *nvidia = 0;

    p9_emit("OS_P9_LOADER_PROBE_BEGIN\n");
    egl = dlopen("libEGL.so.1", P9_RTLD_NOW);
    if (egl == 0)
        return p9_fail("OS_P9_LIBEGL_FAIL\n");
    p9_emit("OS_P9_LIBEGL_PASS\n");

    nvidia = dlopen("libEGL_nvidia.so.0", P9_RTLD_NOW);
    if (nvidia == 0) {
        (void)dlclose(egl);
        return p9_fail("OS_P9_LIBEGL_NVIDIA_FAIL\n");
    }
    p9_emit("OS_P9_LIBEGL_NVIDIA_PASS\n");

    if (dlclose(nvidia) != 0 || dlclose(egl) != 0)
        return p9_fail("OS_P9_DLCLOSE_FAIL\n");
    p9_emit("OS_P9_LOADER_PASS\n");
    return 0;
}
