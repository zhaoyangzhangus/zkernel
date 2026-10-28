/* Phase 10: real EGL device discovery, initialization, and current context. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define P10_EGL_FALSE 0
#define P10_EGL_TRUE 1
#define P10_EGL_NONE 0x3038
#define P10_EGL_PLATFORM_DEVICE_EXT 0x313F
#define P10_EGL_OPENGL_API 0x30A2
#define P10_EGL_SURFACE_TYPE 0x3033
#define P10_EGL_PBUFFER_BIT 0x0001
#define P10_EGL_WIDTH 0x3057
#define P10_EGL_HEIGHT 0x3056
#define P10_GL_VENDOR 0x1F00
#define P10_GL_RENDERER 0x1F01

typedef unsigned int p10_egl_boolean_t;
typedef unsigned int p10_egl_enum_t;
typedef int p10_egl_int_t;
typedef void *p10_egl_display_t;
typedef void *p10_egl_device_t;
typedef void *p10_egl_config_t;
typedef void *p10_egl_context_t;
typedef void *p10_egl_surface_t;

#define P10_EGL_NO_DISPLAY ((p10_egl_display_t)0)
#define P10_EGL_NO_CONTEXT ((p10_egl_context_t)0)
#define P10_EGL_NO_SURFACE ((p10_egl_surface_t)0)

typedef p10_egl_boolean_t (*p10_egl_query_devices_fn)(
    p10_egl_int_t, p10_egl_device_t *, p10_egl_int_t *);
typedef p10_egl_display_t (*p10_egl_get_platform_display_fn)(
    p10_egl_enum_t, void *, const p10_egl_int_t *);
typedef p10_egl_display_t (*p10_egl_get_display_fn)(void *);
typedef p10_egl_boolean_t (*p10_egl_initialize_fn)(
    p10_egl_display_t, p10_egl_int_t *, p10_egl_int_t *);
typedef p10_egl_boolean_t (*p10_egl_bind_api_fn)(p10_egl_enum_t);
typedef p10_egl_boolean_t (*p10_egl_choose_config_fn)(
    p10_egl_display_t, const p10_egl_int_t *, p10_egl_config_t *,
    p10_egl_int_t, p10_egl_int_t *);
typedef p10_egl_context_t (*p10_egl_create_context_fn)(
    p10_egl_display_t, p10_egl_config_t, p10_egl_context_t,
    const p10_egl_int_t *);
typedef p10_egl_surface_t (*p10_egl_create_pbuffer_fn)(
    p10_egl_display_t, p10_egl_config_t, const p10_egl_int_t *);
typedef p10_egl_boolean_t (*p10_egl_make_current_fn)(
    p10_egl_display_t, p10_egl_surface_t, p10_egl_surface_t,
    p10_egl_context_t);
typedef p10_egl_boolean_t (*p10_egl_destroy_context_fn)(
    p10_egl_display_t, p10_egl_context_t);
typedef p10_egl_boolean_t (*p10_egl_destroy_surface_fn)(
    p10_egl_display_t, p10_egl_surface_t);
typedef p10_egl_boolean_t (*p10_egl_terminate_fn)(p10_egl_display_t);
typedef void *(*p10_egl_get_proc_address_fn)(const char *);
typedef const unsigned char *(*p10_gl_get_string_fn)(unsigned int);

/* The real GLVND entry points are loaded after the process starts.  This is
 * the same loader path exercised by the Phase 9 closure test: libEGL parses
 * its vendor JSON and resolves the NVIDIA ICD through the system dynamic
 * linker. */
#define P10_RTLD_NOW 2
#define P10_RTLD_GLOBAL 0x100
#define P10_RTLD_NOLOAD 4

extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *name);
extern int dlclose(void *handle);
extern const char *dlerror(void);
extern int dlinfo(void *handle, int request, void *argument);
extern int setenv(const char *name, const char *value, int overwrite);

typedef struct p10_link_map {
    uintptr_t address;
    const char *name;
    void *dynamic;
    struct p10_link_map *next;
    struct p10_link_map *previous;
} p10_link_map_t;

static void p10_emit(const char *text)
{
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static void p10_emit_value(const char *prefix, const char *value)
{
    p10_emit(prefix);
    if (value != 0) p10_emit(value);
    p10_emit("\n");
}

static int p10_fail(const char *text)
{
    p10_emit(text);
    return 1;
}

static int p10_contains_nvidia(const char *text)
{
    static const char needle[] = "NVIDIA";
    size_t text_length;
    size_t index;

    if (text == 0) return 0;
    text_length = strlen(text);
    if (text_length < sizeof(needle) - 1U) return 0;
    for (index = 0U; index <= text_length - (sizeof(needle) - 1U);
         ++index) {
        size_t needle_index;
        for (needle_index = 0U; needle_index < sizeof(needle) - 1U;
             ++needle_index) {
            if (text[index + needle_index] != needle[needle_index]) break;
        }
        if (needle_index == sizeof(needle) - 1U) return 1;
    }
    return 0;
}

static void p10_emit_link_map(void *handle)
{
    p10_link_map_t *link_map = 0;
    if (handle == 0 || dlinfo(handle, 2, &link_map) != 0 ||
        link_map == 0) {
        p10_emit("OS_P10_LINKMAP_FAIL\n");
        return;
    }
    for (uint32_t index = 0U; index < 32U && link_map != 0; ++index) {
        if (link_map->name != 0)
            p10_emit_value("OS_P10_LINKMAP_NODE=", link_map->name);
        link_map = link_map->next;
    }
}

int main(void)
{
    void *egl_handle = 0;
    void *libc_handle = 0;
    void *libm_handle = 0;
    void *open_gl_handle = 0;
    p10_egl_display_t display = P10_EGL_NO_DISPLAY;
    p10_egl_context_t context = P10_EGL_NO_CONTEXT;
    p10_egl_surface_t surface = P10_EGL_NO_SURFACE;
    p10_egl_device_t devices[8] = {0};
    p10_egl_config_t config = 0;
    p10_egl_int_t device_count = 0;
    p10_egl_int_t config_count = 0;
    p10_egl_int_t major = 0;
    p10_egl_int_t minor = 0;
    int result = 1;

    p10_egl_query_devices_fn query_devices = 0;
    p10_egl_get_platform_display_fn get_platform_display = 0;
    p10_egl_get_display_fn get_display = 0;
    p10_egl_initialize_fn initialize = 0;
    p10_egl_bind_api_fn bind_api = 0;
    p10_egl_choose_config_fn choose_config = 0;
    p10_egl_create_context_fn create_context = 0;
    p10_egl_create_pbuffer_fn create_pbuffer = 0;
    p10_egl_make_current_fn make_current = 0;
    p10_egl_destroy_context_fn destroy_context = 0;
    p10_egl_destroy_surface_fn destroy_surface = 0;
    p10_egl_terminate_fn terminate_display = 0;
    p10_egl_get_proc_address_fn get_proc_address = 0;
    p10_gl_get_string_fn get_string = 0;
    const char *dlopen_error = 0;
    const char *failure_marker =
        "OS_P10_EGL_FAILURE_STAGE_UNKNOWN\n";

    p10_emit("OS_P10_EGL_PROBE_BEGIN\n");
    /* Use the official GLVND override so the headless probe selects only the
     * pinned NVIDIA ICD; the JSON is still parsed and the vendor is still
     * loaded by libEGL itself. */
    if (setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               "/usr/apps/p10-egl-context/egl/10_nvidia.json", 1) != 0) {
        return p10_fail("OS_P10_GLVND_ENV_FAIL\n");
    }
    egl_handle = dlopen(
        "/usr/apps/p10-egl-context/lib/libEGL.so.1",
        P10_RTLD_NOW | P10_RTLD_GLOBAL);
    dlopen_error = dlerror();
    if (dlopen_error != 0) {
        p10_emit_value("OS_P10_DLOPEN_ERROR=", dlopen_error);
        return p10_fail("OS_P10_LIBEGL_FAIL\n");
    }
    if (egl_handle == 0)
        return p10_fail("OS_P10_LIBEGL_FAIL\n");
    {
        p10_link_map_t *link_map = 0;
        if (dlinfo(egl_handle, 2, &link_map) == 0 && link_map != 0 &&
            link_map->name != 0)
            p10_emit_value("OS_P10_EGL_LINKMAP=", link_map->name);
        else
            p10_emit("OS_P10_EGL_LINKMAP_FAIL\n");
    }
    get_proc_address = (p10_egl_get_proc_address_fn)
        dlsym(egl_handle, "eglGetProcAddress");
    if (get_proc_address == 0)
        get_proc_address = (p10_egl_get_proc_address_fn)
            dlsym(0, "eglGetProcAddress");
    if (get_proc_address == 0) {
        const char *dl_error = dlerror();
        if (dlsym(egl_handle, "eglGetDisplay") != 0)
            p10_emit("OS_P10_DLSYM_EGL_DISPLAY_PASS\n");
        else
            p10_emit("OS_P10_DLSYM_EGL_DISPLAY_FAIL\n");
        if (dlsym(egl_handle, "malloc") != 0)
            p10_emit("OS_P10_DLSYM_EGL_DEPENDENCY_PASS\n");
        else
            p10_emit("OS_P10_DLSYM_EGL_DEPENDENCY_FAIL\n");
        libm_handle = dlopen("libm.so.6", P10_RTLD_NOW | P10_RTLD_GLOBAL);
        if (libm_handle != 0 && dlsym(libm_handle, "cos") != 0)
            p10_emit("OS_P10_DLSYM_LIBM_PASS\n");
        else
            p10_emit("OS_P10_DLSYM_LIBM_FAIL\n");
        libc_handle = dlopen("libc.so.6", P10_RTLD_NOW | P10_RTLD_GLOBAL);
        if (libc_handle != 0) {
            if (dlsym(libc_handle, "malloc") != 0)
                p10_emit("OS_P10_DLSYM_LIBC_HANDLE_PASS\n");
            else
                p10_emit("OS_P10_DLSYM_LIBC_HANDLE_FAIL\n");
        } else {
            p10_emit("OS_P10_DLOPEN_LIBC_FAIL\n");
        }
        if (dlsym(0, "malloc") != 0)
            p10_emit("OS_P10_DLSYM_LIBC_GLOBAL_PASS\n");
        else
            p10_emit("OS_P10_DLSYM_LIBC_GLOBAL_FAIL\n");
        if (dl_error != 0)
            p10_emit_value("OS_P10_DLSYM_ERROR=", dl_error);
        failure_marker = "OS_P10_EGL_GETPROC_SYMBOL_FAIL\n";
        goto cleanup;
    }
    get_display = (p10_egl_get_display_fn)dlsym(egl_handle, "eglGetDisplay");
    if (get_display == 0)
        get_display = (p10_egl_get_display_fn)dlsym(0, "eglGetDisplay");
    if (get_display == 0) {
        failure_marker = "OS_P10_EGL_GETDISPLAY_SYMBOL_FAIL\n";
        goto cleanup;
    }
    /* Trigger normal GLVND vendor discovery through the exported EGL display
     * entrypoint before asking for the device extension. */
    display = get_display(0);
    p10_emit("OS_P10_EGL_VENDOR_DISCOVERY_DONE\n");

    /* Core EGL entry points are exported by libEGL.  Resolve them after the
     * vendor has been selected; use eglGetProcAddress only as the standard
     * dispatch fallback for implementations that expose a core symbol there. */
    initialize = (p10_egl_initialize_fn)dlsym(egl_handle, "eglInitialize");
    if (initialize == 0)
        initialize = (p10_egl_initialize_fn)get_proc_address("eglInitialize");
    bind_api = (p10_egl_bind_api_fn)dlsym(egl_handle, "eglBindAPI");
    if (bind_api == 0)
        bind_api = (p10_egl_bind_api_fn)get_proc_address("eglBindAPI");
    choose_config = (p10_egl_choose_config_fn)
        dlsym(egl_handle, "eglChooseConfig");
    if (choose_config == 0)
        choose_config = (p10_egl_choose_config_fn)
            get_proc_address("eglChooseConfig");
    create_context = (p10_egl_create_context_fn)
        dlsym(egl_handle, "eglCreateContext");
    if (create_context == 0)
        create_context = (p10_egl_create_context_fn)
            get_proc_address("eglCreateContext");
    create_pbuffer = (p10_egl_create_pbuffer_fn)
        dlsym(egl_handle, "eglCreatePbufferSurface");
    if (create_pbuffer == 0)
        create_pbuffer = (p10_egl_create_pbuffer_fn)
            get_proc_address("eglCreatePbufferSurface");
    make_current = (p10_egl_make_current_fn)
        dlsym(egl_handle, "eglMakeCurrent");
    if (make_current == 0)
        make_current = (p10_egl_make_current_fn)
            get_proc_address("eglMakeCurrent");
    destroy_context = (p10_egl_destroy_context_fn)
        dlsym(egl_handle, "eglDestroyContext");
    if (destroy_context == 0)
        destroy_context = (p10_egl_destroy_context_fn)
            get_proc_address("eglDestroyContext");
    destroy_surface = (p10_egl_destroy_surface_fn)
        dlsym(egl_handle, "eglDestroySurface");
    if (destroy_surface == 0)
        destroy_surface = (p10_egl_destroy_surface_fn)
            get_proc_address("eglDestroySurface");
    terminate_display = (p10_egl_terminate_fn)dlsym(egl_handle, "eglTerminate");
    if (terminate_display == 0)
        terminate_display = (p10_egl_terminate_fn)
            get_proc_address("eglTerminate");
    if (initialize == 0 || bind_api == 0 || choose_config == 0 ||
        create_context == 0 || create_pbuffer == 0 || make_current == 0 ||
        destroy_context == 0 || destroy_surface == 0 ||
        terminate_display == 0) {
        failure_marker = "OS_P10_EGL_SYMBOL_FAIL\n";
        goto cleanup;
    }

    query_devices = get_proc_address != 0 ?
        (p10_egl_query_devices_fn)get_proc_address("eglQueryDevicesEXT") : 0;
    if (query_devices == 0) {
        failure_marker = "OS_P10_QUERY_DEVICES_SYMBOL_FAIL\n";
        goto cleanup;
    }
    p10_emit("OS_P10_QUERY_DEVICES_CALL\n");
    if (query_devices(8, devices, &device_count) != P10_EGL_TRUE) {
        failure_marker = "OS_P10_QUERY_DEVICES_CALL_FAIL\n";
        goto cleanup;
    }
    if (device_count <= 0) {
        void *vendor_handle = dlopen(
            "libEGL_nvidia.so.0", P10_RTLD_NOW | P10_RTLD_NOLOAD);
        p10_emit_link_map(egl_handle);
        if (vendor_handle != 0) {
            p10_emit("OS_P10_NVIDIA_ICD_NOLOAD_PASS\n");
            if (dlsym(vendor_handle, "__egl_Main") != 0)
                p10_emit("OS_P10_NVIDIA_ICD_ENTRY_PASS\n");
            else
                p10_emit("OS_P10_NVIDIA_ICD_ENTRY_FAIL\n");
            if (dlclose(vendor_handle) != 0)
                p10_emit("OS_P10_NVIDIA_ICD_CLOSE_FAIL\n");
        } else {
            const char *vendor_error = dlerror();
            p10_emit("OS_P10_NVIDIA_ICD_NOLOAD_FAIL\n");
            if (vendor_error != 0)
                p10_emit_value("OS_P10_NVIDIA_ICD_ERROR=", vendor_error);
        }
        failure_marker = "OS_P10_DEVICE_COUNT_FAIL\n";
        goto cleanup;
    }
    if (devices[0] == 0) {
        failure_marker = "OS_P10_DEVICE_HANDLE_FAIL\n";
        goto cleanup;
    }
    p10_emit("OS_P10_NVIDIA_DEVICE_PASS\n");

    get_platform_display = get_proc_address != 0 ?
        (p10_egl_get_platform_display_fn)get_proc_address(
            "eglGetPlatformDisplayEXT") : 0;
    if (get_platform_display != 0) {
        display = get_platform_display(P10_EGL_PLATFORM_DEVICE_EXT,
                                       devices[0], 0);
    }
    if (display == P10_EGL_NO_DISPLAY && get_display != 0)
        display = get_display(0);
    if (display == P10_EGL_NO_DISPLAY) {
        failure_marker = "OS_P10_DISPLAY_FAIL\n";
        goto cleanup;
    }

    if (initialize(display, &major, &minor) != P10_EGL_TRUE) {
        failure_marker = "OS_P10_EGL_INITIALIZE_FAIL\n";
        goto cleanup;
    }
    p10_emit("OS_P10_EGL_INITIALIZED\n");

    if (bind_api(P10_EGL_OPENGL_API) != P10_EGL_TRUE) {
        failure_marker = "OS_P10_BIND_API_FAIL\n";
        goto cleanup;
    }
    {
        const p10_egl_int_t config_attributes[] = {
            P10_EGL_SURFACE_TYPE, P10_EGL_PBUFFER_BIT, P10_EGL_NONE
        };
        if (choose_config(display, config_attributes, &config, 1,
                          &config_count) != P10_EGL_TRUE) {
            failure_marker = "OS_P10_CHOOSE_CONFIG_FAIL\n";
            goto cleanup;
        }
        if (config_count <= 0 || config == 0) {
            failure_marker = "OS_P10_CONFIG_RESULT_FAIL\n";
            goto cleanup;
        }
    }
    context = create_context(display, config, P10_EGL_NO_CONTEXT,
                             (const p10_egl_int_t[]){P10_EGL_NONE});
    if (context == P10_EGL_NO_CONTEXT) {
        failure_marker = "OS_P10_CREATE_CONTEXT_FAIL\n";
        goto cleanup;
    }

    if (make_current(display, P10_EGL_NO_SURFACE, P10_EGL_NO_SURFACE,
                     context) != P10_EGL_TRUE) {
        surface = create_pbuffer(display, config,
                                 (const p10_egl_int_t[]){
                                     P10_EGL_WIDTH, 16,
                                     P10_EGL_HEIGHT, 16,
                                     P10_EGL_NONE});
        if (surface == P10_EGL_NO_SURFACE) {
            failure_marker = "OS_P10_CREATE_PBUFFER_FAIL\n";
            goto cleanup;
        }
        if (make_current(display, surface, surface, context) != P10_EGL_TRUE) {
            failure_marker = "OS_P10_MAKE_CURRENT_PBUFFER_FAIL\n";
            goto cleanup;
        }
    }
    p10_emit("OS_P10_CONTEXT_CURRENT\n");

    open_gl_handle = dlopen("libOpenGL.so.0", P10_RTLD_NOW);
    if (open_gl_handle != 0)
        get_string = (p10_gl_get_string_fn)dlsym(open_gl_handle, "glGetString");
    if (get_string == 0 && get_proc_address != 0)
        get_string = (p10_gl_get_string_fn)get_proc_address("glGetString");
    if (get_string == 0) {
        failure_marker = "OS_P10_GL_SYMBOL_FAIL\n";
        goto cleanup;
    }
    const unsigned char *vendor = get_string(P10_GL_VENDOR);
    const unsigned char *renderer = get_string(P10_GL_RENDERER);
    if (vendor == 0 || renderer == 0 || vendor[0] == '\0' ||
        renderer[0] == '\0') {
        failure_marker = "OS_P10_GL_STRING_FAIL\n";
        goto cleanup;
    }
    if (!p10_contains_nvidia((const char *)vendor)) {
        failure_marker = "OS_P10_GL_VENDOR_FAIL\n";
        goto cleanup;
    }
    p10_emit_value("OS_P10_GL_VENDOR=", (const char *)vendor);
    p10_emit_value("OS_P10_GL_RENDERER=", (const char *)renderer);
    result = 0;

cleanup:
    if (surface != P10_EGL_NO_SURFACE && destroy_surface != 0)
        (void)destroy_surface(display, surface);
    if (context != P10_EGL_NO_CONTEXT && destroy_context != 0)
        (void)destroy_context(display, context);
    if (display != P10_EGL_NO_DISPLAY && terminate_display != 0)
        (void)terminate_display(display);
    if (open_gl_handle != 0 && dlclose(open_gl_handle) != 0)
        failure_marker = "OS_P10_DLCLOSE_FAIL\n";
    if (libm_handle != 0 && dlclose(libm_handle) != 0)
        failure_marker = "OS_P10_DLCLOSE_FAIL\n";
    if (libc_handle != 0 && dlclose(libc_handle) != 0)
        failure_marker = "OS_P10_DLCLOSE_FAIL\n";
    if (egl_handle != 0 && dlclose(egl_handle) != 0)
        failure_marker = "OS_P10_DLCLOSE_FAIL\n";
    if (result != 0) {
        p10_emit(failure_marker);
        return p10_fail("OS_P10_EGL_FAIL\n");
    }
    p10_emit("OS_P10_PASS\n");
    return 0;
}
