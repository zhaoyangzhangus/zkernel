/*
 * System NVIDIA OpenGL 演示程序。
 *
 * 演示程序通过 Linux 兼容扩展自行创建 System 窗口，并将窗口桥接保持精简：OpenGL 渲染到离屏 FBO，
 * 再由 glReadPixels 直接写入预分配且与合成器共享的 System 窗口 surface。窗口 PRESENT 调用会发布帧，
 * 并等待合成器消费。鼠标输入从所属窗口的 Linux fd 读取；仅在按住左键时改变 yaw/pitch。
 */
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uapi/linux_abi.h>
#include <uapi/display.h>
#include <uapi/window.h>
#include <uapi/chrome_caption.h>
#include "cube_timing.h"

#ifndef CUBE_WIDTH
#define CUBE_WIDTH              800U
#endif
#ifndef CUBE_HEIGHT
#define CUBE_HEIGHT             600U
#endif
#ifndef CUBE_WINDOW_X
#define CUBE_WINDOW_X           64
#endif
#ifndef CUBE_WINDOW_Y
#define CUBE_WINDOW_Y           48
#endif
#ifndef CUBE_INTERACTIVE
#define CUBE_INTERACTIVE        0
#endif
#ifndef CUBE_AUTOMATED
#define CUBE_AUTOMATED          0
#endif
#if CUBE_INTERACTIVE || !CUBE_AUTOMATED
#define CUBE_FRAME_CONDITION    (!quit)
#define CUBE_FRAME_PACING       0
#else
/* 有限帧运行仅用于非交互式 CI 门禁。正常立方体应用会持续渲染，直到按下 Esc 或关闭窗口设置 quit。 */
#define CUBE_FRAME_LIMIT        600U
#define CUBE_FRAME_CONDITION    (frame < CUBE_FRAME_LIMIT && !quit)
#define CUBE_FRAME_PACING       1
#define CUBE_FRAME_NS           33333333ULL
#endif
#define CUBE_HBUFFER_ADDRESS    0x08000000ULL
#define CUBE_CLOCK_MONOTONIC    1
#define CUBE_ERRNO_AGAIN        11
#define CUBE_ESC_KEY            0x01U
#define CUBE_PI                 3.14159265358979323846F
#define CUBE_HALF_PI            1.57079632679489661923F
#define CUBE_TWO_PI             6.28318530717958647692F

extern int open(const char *, int, ...);
extern int ioctl(int, unsigned long, ...);
extern int sched_yield(void);
extern int *__errno_location(void);
extern long syscall(long, ...);

/* OS_WINDOW_RESIZABLE 窗口使用服务器侧边框几何。此处常量需与公共窗口几何约定同步，
 * 以便较小的固件帧缓冲区也能容纳诊断窗口。 */
#define CUBE_WINDOW_FRAME_EXTRA 2U
#define CUBE_WINDOW_TITLEBAR    CHROME_CAPTION_HEIGHT
#define CUBE_WINDOW_MIN_WIDTH   160U
#define CUBE_WINDOW_MIN_HEIGHT  96U

typedef struct timespec cube_timespec_t;

typedef unsigned int cube_egl_boolean_t;
typedef unsigned int cube_egl_enum_t;
typedef int cube_egl_int_t;
typedef void *cube_egl_display_t;
typedef void *cube_egl_device_t;
typedef void *cube_egl_config_t;
typedef void *cube_egl_context_t;
typedef void *cube_egl_surface_t;

#define CUBE_EGL_TRUE                         1U
#define CUBE_EGL_NONE                         0x3038
#define CUBE_EGL_PLATFORM_DEVICE_EXT         0x313F
#define CUBE_EGL_OPENGL_API                  0x30A2
#define CUBE_EGL_SURFACE_TYPE                0x3033
#define CUBE_EGL_PBUFFER_BIT                 0x0001
#define CUBE_EGL_RENDERABLE_TYPE             0x3040
#define CUBE_EGL_OPENGL_BIT                  0x0008
#define CUBE_EGL_RED_SIZE                    0x3024
#define CUBE_EGL_GREEN_SIZE                  0x3023
#define CUBE_EGL_BLUE_SIZE                   0x3022
#define CUBE_EGL_ALPHA_SIZE                  0x3021
#define CUBE_EGL_WIDTH                       0x3057
#define CUBE_EGL_HEIGHT                      0x3056
#define CUBE_EGL_CONTEXT_MAJOR_VERSION_KHR   0x3098
#define CUBE_EGL_CONTEXT_MINOR_VERSION_KHR   0x30FB
#define CUBE_EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#define CUBE_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001

#define CUBE_GL_NO_ERROR                     0U
#define CUBE_GL_VENDOR                       0x1F00
#define CUBE_GL_RENDERER                     0x1F01
#define CUBE_GL_VERSION                      0x1F02
#define CUBE_GL_COLOR_BUFFER_BIT             0x00004000U
#define CUBE_GL_DEPTH_BUFFER_BIT             0x00000100U
#define CUBE_GL_DEPTH_TEST                   0x0B71U
#define CUBE_GL_LESS                         0x0201U
#define CUBE_GL_TRIANGLES                    0x0004U
#define CUBE_GL_FLOAT                        0x1406U
#define CUBE_GL_UNSIGNED_BYTE                0x1401U
#define CUBE_GL_UNSIGNED_INT                 0x1405U
#define CUBE_GL_RGBA                         0x1908U
#define CUBE_GL_BGRA                        0x80E1U
#define CUBE_GL_DEPTH_COMPONENT              0x1902U
#define CUBE_GL_ARRAY_BUFFER                 0x8892U
#define CUBE_GL_STATIC_DRAW                 0x88E4U
#define CUBE_GL_FRAMEBUFFER                  0x8D40U
#define CUBE_GL_COLOR_ATTACHMENT0            0x8CE0U
#define CUBE_GL_DEPTH_ATTACHMENT             0x8D00U
#define CUBE_GL_TEXTURE_2D                   0x0DE1U
#define CUBE_GL_TEXTURE_MIN_FILTER           0x2801U
#define CUBE_GL_TEXTURE_MAG_FILTER           0x2800U
#define CUBE_GL_NEAREST                      0x2600U
#define CUBE_GL_RGBA8                        0x8058U
#define CUBE_GL_DEPTH_COMPONENT24            0x81A6U
#define CUBE_GL_FRAMEBUFFER_COMPLETE         0x8CD5U
#define CUBE_GL_TEXTURE_WIDTH                0x1000U
#define CUBE_GL_TEXTURE_HEIGHT               0x1001U
#define CUBE_GL_TEXTURE_INTERNAL_FORMAT      0x1003U
#define CUBE_GL_VERTEX_SHADER                0x8B31U
#define CUBE_GL_FRAGMENT_SHADER              0x8B30U
#define CUBE_GL_COMPILE_STATUS               0x8B81U
#define CUBE_GL_LINK_STATUS                  0x8B82U

#define CUBE_RTLD_NOW                         2
#define CUBE_RTLD_GLOBAL                      0x100

typedef cube_egl_boolean_t (*cube_egl_query_devices_fn)(
    cube_egl_int_t, cube_egl_device_t *, cube_egl_int_t *);
typedef cube_egl_display_t (*cube_egl_get_platform_display_fn)(
    cube_egl_enum_t, void *, const cube_egl_int_t *);
typedef cube_egl_display_t (*cube_egl_get_display_fn)(void *);
typedef cube_egl_boolean_t (*cube_egl_initialize_fn)(
    cube_egl_display_t, cube_egl_int_t *, cube_egl_int_t *);
typedef cube_egl_boolean_t (*cube_egl_bind_api_fn)(cube_egl_enum_t);
typedef cube_egl_boolean_t (*cube_egl_choose_config_fn)(
    cube_egl_display_t, const cube_egl_int_t *, cube_egl_config_t *,
    cube_egl_int_t, cube_egl_int_t *);
typedef cube_egl_context_t (*cube_egl_create_context_fn)(
    cube_egl_display_t, cube_egl_config_t, cube_egl_context_t,
    const cube_egl_int_t *);
typedef cube_egl_surface_t (*cube_egl_create_pbuffer_fn)(
    cube_egl_display_t, cube_egl_config_t, const cube_egl_int_t *);
typedef cube_egl_boolean_t (*cube_egl_make_current_fn)(
    cube_egl_display_t, cube_egl_surface_t, cube_egl_surface_t,
    cube_egl_context_t);
typedef cube_egl_boolean_t (*cube_egl_destroy_context_fn)(
    cube_egl_display_t, cube_egl_context_t);
typedef cube_egl_boolean_t (*cube_egl_destroy_surface_fn)(
    cube_egl_display_t, cube_egl_surface_t);
typedef cube_egl_boolean_t (*cube_egl_terminate_fn)(cube_egl_display_t);
typedef cube_egl_int_t (*cube_egl_get_error_fn)(void);
typedef void *(*cube_egl_get_proc_address_fn)(const char *);

#define CUBE_EGL_NO_DISPLAY ((cube_egl_display_t)0)
#define CUBE_EGL_NO_CONTEXT ((cube_egl_context_t)0)
#define CUBE_EGL_NO_SURFACE ((cube_egl_surface_t)0)

typedef unsigned int cube_gl_enum_t;
typedef unsigned int cube_gl_uint_t;
typedef unsigned int cube_gl_bitfield_t;
typedef unsigned char cube_gl_boolean_t;
typedef int cube_gl_int_t;
typedef int cube_gl_sizei_t;
typedef int64_t cube_gl_sizeiptr_t;
typedef float cube_gl_float_t;

typedef const unsigned char *(*cube_gl_get_string_fn)(cube_gl_enum_t);
typedef cube_gl_enum_t (*cube_gl_get_error_fn)(void);
typedef void (*cube_gl_viewport_fn)(cube_gl_int_t, cube_gl_int_t,
                                    cube_gl_sizei_t, cube_gl_sizei_t);
typedef void (*cube_gl_clear_color_fn)(cube_gl_float_t, cube_gl_float_t,
                                       cube_gl_float_t, cube_gl_float_t);
typedef void (*cube_gl_clear_fn)(cube_gl_bitfield_t);
typedef void (*cube_gl_enable_fn)(cube_gl_enum_t);
typedef void (*cube_gl_depth_func_fn)(cube_gl_enum_t);
typedef cube_gl_uint_t (*cube_gl_create_shader_fn)(cube_gl_enum_t);
typedef void (*cube_gl_shader_source_fn)(cube_gl_uint_t, cube_gl_sizei_t,
                                         const char *const *,
                                         const cube_gl_int_t *);
typedef void (*cube_gl_compile_shader_fn)(cube_gl_uint_t);
typedef void (*cube_gl_get_shader_iv_fn)(cube_gl_uint_t, cube_gl_enum_t,
                                         cube_gl_int_t *);
typedef void (*cube_gl_delete_shader_fn)(cube_gl_uint_t);
typedef cube_gl_uint_t (*cube_gl_create_program_fn)(void);
typedef void (*cube_gl_attach_shader_fn)(cube_gl_uint_t, cube_gl_uint_t);
typedef void (*cube_gl_link_program_fn)(cube_gl_uint_t);
typedef void (*cube_gl_get_program_iv_fn)(cube_gl_uint_t, cube_gl_enum_t,
                                          cube_gl_int_t *);
typedef void (*cube_gl_delete_program_fn)(cube_gl_uint_t);
typedef void (*cube_gl_use_program_fn)(cube_gl_uint_t);
typedef cube_gl_int_t (*cube_gl_get_uniform_location_fn)(cube_gl_uint_t,
                                                         const char *);
typedef void (*cube_gl_uniform_matrix_4fv_fn)(cube_gl_int_t, cube_gl_sizei_t,
                                              cube_gl_boolean_t,
                                              const cube_gl_float_t *);
typedef void (*cube_gl_gen_vertex_arrays_fn)(cube_gl_sizei_t, cube_gl_uint_t *);
typedef void (*cube_gl_bind_vertex_array_fn)(cube_gl_uint_t);
typedef void (*cube_gl_delete_vertex_arrays_fn)(cube_gl_sizei_t,
                                                const cube_gl_uint_t *);
typedef void (*cube_gl_gen_buffers_fn)(cube_gl_sizei_t, cube_gl_uint_t *);
typedef void (*cube_gl_bind_buffer_fn)(cube_gl_enum_t, cube_gl_uint_t);
typedef void (*cube_gl_buffer_data_fn)(cube_gl_enum_t, cube_gl_sizeiptr_t,
                                       const void *, cube_gl_enum_t);
typedef void (*cube_gl_delete_buffers_fn)(cube_gl_sizei_t,
                                          const cube_gl_uint_t *);
typedef void (*cube_gl_enable_vertex_attrib_array_fn)(cube_gl_uint_t);
typedef void (*cube_gl_vertex_attrib_pointer_fn)(cube_gl_uint_t, cube_gl_int_t,
                                                 cube_gl_enum_t,
                                                 cube_gl_boolean_t,
                                                 cube_gl_sizei_t,
                                                 const void *);
typedef void (*cube_gl_draw_arrays_fn)(cube_gl_enum_t, cube_gl_int_t,
                                       cube_gl_sizei_t);
typedef void (*cube_gl_read_pixels_fn)(cube_gl_int_t, cube_gl_int_t,
                                       cube_gl_sizei_t, cube_gl_sizei_t,
                                       cube_gl_enum_t, cube_gl_enum_t, void *);
typedef void (*cube_gl_gen_framebuffers_fn)(cube_gl_sizei_t, cube_gl_uint_t *);
typedef void (*cube_gl_bind_framebuffer_fn)(cube_gl_enum_t, cube_gl_uint_t);
typedef void (*cube_gl_delete_framebuffers_fn)(cube_gl_sizei_t,
                                               const cube_gl_uint_t *);
typedef void (*cube_gl_gen_textures_fn)(cube_gl_sizei_t, cube_gl_uint_t *);
typedef void (*cube_gl_bind_texture_fn)(cube_gl_enum_t, cube_gl_uint_t);
typedef void (*cube_gl_delete_textures_fn)(cube_gl_sizei_t,
                                           const cube_gl_uint_t *);
typedef void (*cube_gl_tex_parameteri_fn)(cube_gl_enum_t, cube_gl_enum_t,
                                          cube_gl_int_t);
typedef void (*cube_gl_tex_image_2d_fn)(cube_gl_enum_t, cube_gl_int_t,
                                        cube_gl_int_t, cube_gl_sizei_t,
                                        cube_gl_sizei_t, cube_gl_int_t,
                                        cube_gl_enum_t, cube_gl_enum_t,
                                        const void *);
typedef void (*cube_gl_framebuffer_texture_2d_fn)(cube_gl_enum_t,
                                                  cube_gl_enum_t,
                                                  cube_gl_enum_t,
                                                  cube_gl_uint_t,
                                                  cube_gl_int_t);
typedef cube_gl_enum_t (*cube_gl_check_framebuffer_status_fn)(cube_gl_enum_t);
typedef void (*cube_gl_get_tex_level_parameteriv_fn)(cube_gl_enum_t,
    cube_gl_int_t, cube_gl_enum_t, cube_gl_int_t *);

typedef struct cube_gl_api {
    cube_gl_get_string_fn get_string;
    cube_gl_get_error_fn get_error;
    cube_gl_viewport_fn viewport;
    cube_gl_clear_color_fn clear_color;
    cube_gl_clear_fn clear;
    cube_gl_enable_fn enable;
    cube_gl_depth_func_fn depth_func;
    cube_gl_create_shader_fn create_shader;
    cube_gl_shader_source_fn shader_source;
    cube_gl_compile_shader_fn compile_shader;
    cube_gl_get_shader_iv_fn get_shader_iv;
    cube_gl_delete_shader_fn delete_shader;
    cube_gl_create_program_fn create_program;
    cube_gl_attach_shader_fn attach_shader;
    cube_gl_link_program_fn link_program;
    cube_gl_get_program_iv_fn get_program_iv;
    cube_gl_delete_program_fn delete_program;
    cube_gl_use_program_fn use_program;
    cube_gl_get_uniform_location_fn get_uniform_location;
    cube_gl_uniform_matrix_4fv_fn uniform_matrix_4fv;
    cube_gl_gen_vertex_arrays_fn gen_vertex_arrays;
    cube_gl_bind_vertex_array_fn bind_vertex_array;
    cube_gl_delete_vertex_arrays_fn delete_vertex_arrays;
    cube_gl_gen_buffers_fn gen_buffers;
    cube_gl_bind_buffer_fn bind_buffer;
    cube_gl_buffer_data_fn buffer_data;
    cube_gl_delete_buffers_fn delete_buffers;
    cube_gl_enable_vertex_attrib_array_fn enable_vertex_attrib_array;
    cube_gl_vertex_attrib_pointer_fn vertex_attrib_pointer;
    cube_gl_draw_arrays_fn draw_arrays;
    cube_gl_read_pixels_fn read_pixels;
    cube_gl_gen_framebuffers_fn gen_framebuffers;
    cube_gl_bind_framebuffer_fn bind_framebuffer;
    cube_gl_delete_framebuffers_fn delete_framebuffers;
    cube_gl_gen_textures_fn gen_textures;
    cube_gl_bind_texture_fn bind_texture;
    cube_gl_delete_textures_fn delete_textures;
    cube_gl_tex_parameteri_fn tex_parameteri;
    cube_gl_tex_image_2d_fn tex_image_2d;
    cube_gl_framebuffer_texture_2d_fn framebuffer_texture_2d;
    cube_gl_check_framebuffer_status_fn check_framebuffer_status;
    cube_gl_get_tex_level_parameteriv_fn get_tex_level_parameteriv;
} cube_gl_api_t;

/* glReadPixels 直接写入此地址。window_server_present() 会等待合成器消费前一序列，
 * 然后才能用下一帧覆盖共享 surface。 */
static uint32_t g_cube_shared_buffer_reported;
static uint64_t g_cube_window_buffer_address = CUBE_HBUFFER_ADDRESS;

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int setenv(const char *, const char *, int);

/* 在启动文件系统中保留一条有界结果，供没有可见 stdout 的桌面启动使用。
 * 写入诊断失败不得改变渲染行为。 */
static void cube_record_log(const char *path, const char *text) {
    if (path == 0 || text == 0) return;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) return;
    size_t remaining = strlen(text);
    while (remaining != 0U) {
        ssize_t written = write(descriptor, text, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || (size_t)written > remaining) break;
        text += (size_t)written;
        remaining -= (size_t)written;
    }
    (void)fsync(descriptor);
    (void)close(descriptor);
}

static void cube_record_result(const char *text) {
    cube_record_log("/log/cube-result.log", text);
}

static void cube_emit(const char *text) {
    if (text == 0) return;
    size_t remaining = strlen(text);
    while (remaining != 0U) {
        ssize_t written = write(STDOUT_FILENO, text, remaining);
        if (written < 0 && *__errno_location() == EINTR) continue;
        if (written <= 0 || (size_t)written > remaining) break;
        text += (size_t)written;
        remaining -= (size_t)written;
    }
}


static void cube_emit_u64(uint64_t value) {
    char digits[21];
    size_t start = sizeof(digits) - 1U;
    digits[start] = '\0';
    do {
        digits[--start] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    cube_emit(&digits[start]);
}

static int cube_query_display(uint32_t *width, uint32_t *height) {
    os_display_info_t info = {
        .hdr = {
            .size = sizeof(info),
            .version = 1U,
            .flags = 0U,
        },
        .output = 0U,
        .flags = 0U,
    };
    long result;

    if (width == 0 || height == 0) return 0;
    result = syscall((long)LINUX_SYS_DISPLAY_GET_INFO, &info);
    if (result < 0L || info.width == 0U || info.height == 0U) return 0;
    *width = info.width;
    *height = info.height;
    return 1;
}

static int cube_geometry_fits(uint32_t display_width,
                              uint32_t display_height,
                              int32_t x, int32_t y,
                              uint32_t width, uint32_t height) {
    if (x < 0 || y < 0 || width == 0U || height == 0U) return 0;
    return (uint64_t)(uint32_t)x + width + CUBE_WINDOW_FRAME_EXTRA <=
               display_width &&
           (uint64_t)(uint32_t)y + height + CUBE_WINDOW_FRAME_EXTRA +
               CUBE_WINDOW_TITLEBAR <= display_height;
}

static int cube_fit_geometry(uint32_t display_width,
                             uint32_t display_height,
                             uint32_t *width, uint32_t *height,
                             int32_t *x, int32_t *y) {
    uint32_t fitted_width;
    uint32_t fitted_height;

    if (width == 0 || height == 0 || x == 0 || y == 0 ||
        display_width == 0U || display_height == 0U) return 0;
    if (cube_geometry_fits(display_width, display_height, *x, *y,
                           *width, *height)) return 1;
    if (display_width <= CUBE_WINDOW_FRAME_EXTRA ||
        display_height <= CUBE_WINDOW_FRAME_EXTRA + CUBE_WINDOW_TITLEBAR) {
        return 0;
    }
    fitted_width = display_width - CUBE_WINDOW_FRAME_EXTRA;
    fitted_height = display_height - CUBE_WINDOW_FRAME_EXTRA -
                    CUBE_WINDOW_TITLEBAR;
    if (fitted_width > CUBE_WIDTH) fitted_width = CUBE_WIDTH;
    if (fitted_height > CUBE_HEIGHT) fitted_height = CUBE_HEIGHT;
    if (fitted_width < CUBE_WINDOW_MIN_WIDTH ||
        fitted_height < CUBE_WINDOW_MIN_HEIGHT) return 0;
    *width = fitted_width;
    *height = fitted_height;
    *x = 0;
    *y = 0;
    return 1;
}

static void cube_emit_value(const char *prefix, const char *value) {
    cube_emit(prefix);
    if (value != 0) cube_emit(value);
    cube_emit("\n");
}

static int cube_contains_nvidia(const char *text) {
    static const char needle[] = "NVIDIA";
    size_t text_length;
    if (text == 0) return 0;
    text_length = strlen(text);
    if (text_length < sizeof(needle) - 1U) return 0;
    for (size_t index = 0U; index <= text_length - (sizeof(needle) - 1U);
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

static int cube_window_command(int descriptor, uint32_t operation,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height) {
    linux_window_command_t command = {0};
    uint64_t stride;
    command.operation = operation;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    if (operation == LINUX_WINDOW_OPERATION_PRESENT) {
        if (width == 0U || height == 0U || width > UINT32_MAX / 4U)
            return 0;
        stride = (uint64_t)width * 4U;
        if (stride > UINT32_MAX || stride > UINT64_MAX / height) return 0;
        command.stride = (uint32_t)stride;
        command.address = g_cube_window_buffer_address;
        command.length = stride * height;
    }
    return ioctl(descriptor, (unsigned long)LINUX_WINDOW_IOCTL_COMMIT,
                 &command) == 0;
}

static int cube_now_ns(uint64_t *value) {
    cube_timespec_t now;
    if (value == 0 || clock_gettime(CUBE_CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000LL)
        return 0;
    if ((uint64_t)now.tv_sec >
        (UINT64_MAX - (uint64_t)now.tv_nsec) / 1000000000ULL)
        return 0;
    *value = (uint64_t)now.tv_sec * 1000000000ULL +
             (uint64_t)now.tv_nsec;
    return 1;
}

#if CUBE_FRAME_PACING
static int cube_wait_until(uint64_t deadline) {
    for (;;) {
        uint64_t now;
        if (!cube_now_ns(&now)) return 0;
        if (now >= deadline) return 1;
        if (sched_yield() != 0) return 0;
    }
}
#endif

#define CUBE_GL_LOAD(field, type, name)                                      \
    do {                                                                      \
        void *cube_symbol = handle != 0 ? dlsym(handle, name) : 0;            \
        if (cube_symbol == 0 && get_proc_address != 0)                        \
            cube_symbol = get_proc_address(name);                            \
        api->field = (type)cube_symbol;                                      \
        if (api->field == 0) return 0;                                        \
    } while (0)

static int cube_load_gl_api(void *handle,
                            cube_egl_get_proc_address_fn get_proc_address,
                            cube_gl_api_t *api) {
    CUBE_GL_LOAD(get_string, cube_gl_get_string_fn, "glGetString");
    CUBE_GL_LOAD(get_error, cube_gl_get_error_fn, "glGetError");
    CUBE_GL_LOAD(viewport, cube_gl_viewport_fn, "glViewport");
    CUBE_GL_LOAD(clear_color, cube_gl_clear_color_fn, "glClearColor");
    CUBE_GL_LOAD(clear, cube_gl_clear_fn, "glClear");
    CUBE_GL_LOAD(enable, cube_gl_enable_fn, "glEnable");
    CUBE_GL_LOAD(depth_func, cube_gl_depth_func_fn, "glDepthFunc");
    CUBE_GL_LOAD(create_shader, cube_gl_create_shader_fn, "glCreateShader");
    CUBE_GL_LOAD(shader_source, cube_gl_shader_source_fn, "glShaderSource");
    CUBE_GL_LOAD(compile_shader, cube_gl_compile_shader_fn, "glCompileShader");
    CUBE_GL_LOAD(get_shader_iv, cube_gl_get_shader_iv_fn, "glGetShaderiv");
    CUBE_GL_LOAD(delete_shader, cube_gl_delete_shader_fn, "glDeleteShader");
    CUBE_GL_LOAD(create_program, cube_gl_create_program_fn, "glCreateProgram");
    CUBE_GL_LOAD(attach_shader, cube_gl_attach_shader_fn, "glAttachShader");
    CUBE_GL_LOAD(link_program, cube_gl_link_program_fn, "glLinkProgram");
    CUBE_GL_LOAD(get_program_iv, cube_gl_get_program_iv_fn, "glGetProgramiv");
    CUBE_GL_LOAD(delete_program, cube_gl_delete_program_fn, "glDeleteProgram");
    CUBE_GL_LOAD(use_program, cube_gl_use_program_fn, "glUseProgram");
    CUBE_GL_LOAD(get_uniform_location, cube_gl_get_uniform_location_fn,
                 "glGetUniformLocation");
    CUBE_GL_LOAD(uniform_matrix_4fv, cube_gl_uniform_matrix_4fv_fn,
                 "glUniformMatrix4fv");
    CUBE_GL_LOAD(gen_vertex_arrays, cube_gl_gen_vertex_arrays_fn,
                 "glGenVertexArrays");
    CUBE_GL_LOAD(bind_vertex_array, cube_gl_bind_vertex_array_fn,
                 "glBindVertexArray");
    CUBE_GL_LOAD(delete_vertex_arrays, cube_gl_delete_vertex_arrays_fn,
                 "glDeleteVertexArrays");
    CUBE_GL_LOAD(gen_buffers, cube_gl_gen_buffers_fn, "glGenBuffers");
    CUBE_GL_LOAD(bind_buffer, cube_gl_bind_buffer_fn, "glBindBuffer");
    CUBE_GL_LOAD(buffer_data, cube_gl_buffer_data_fn, "glBufferData");
    CUBE_GL_LOAD(delete_buffers, cube_gl_delete_buffers_fn, "glDeleteBuffers");
    CUBE_GL_LOAD(enable_vertex_attrib_array,
                 cube_gl_enable_vertex_attrib_array_fn,
                 "glEnableVertexAttribArray");
    CUBE_GL_LOAD(vertex_attrib_pointer, cube_gl_vertex_attrib_pointer_fn,
                 "glVertexAttribPointer");
    CUBE_GL_LOAD(draw_arrays, cube_gl_draw_arrays_fn, "glDrawArrays");
    CUBE_GL_LOAD(read_pixels, cube_gl_read_pixels_fn, "glReadPixels");
    CUBE_GL_LOAD(gen_framebuffers, cube_gl_gen_framebuffers_fn,
                 "glGenFramebuffers");
    CUBE_GL_LOAD(bind_framebuffer, cube_gl_bind_framebuffer_fn,
                 "glBindFramebuffer");
    CUBE_GL_LOAD(delete_framebuffers, cube_gl_delete_framebuffers_fn,
                 "glDeleteFramebuffers");
    CUBE_GL_LOAD(gen_textures, cube_gl_gen_textures_fn, "glGenTextures");
    CUBE_GL_LOAD(bind_texture, cube_gl_bind_texture_fn, "glBindTexture");
    CUBE_GL_LOAD(delete_textures, cube_gl_delete_textures_fn,
                 "glDeleteTextures");
    CUBE_GL_LOAD(tex_parameteri, cube_gl_tex_parameteri_fn, "glTexParameteri");
    CUBE_GL_LOAD(tex_image_2d, cube_gl_tex_image_2d_fn, "glTexImage2D");
    CUBE_GL_LOAD(framebuffer_texture_2d,
                 cube_gl_framebuffer_texture_2d_fn,
                 "glFramebufferTexture2D");
    CUBE_GL_LOAD(check_framebuffer_status,
                 cube_gl_check_framebuffer_status_fn,
                 "glCheckFramebufferStatus");
    CUBE_GL_LOAD(get_tex_level_parameteriv,
                 cube_gl_get_tex_level_parameteriv_fn,
                 "glGetTexLevelParameteriv");
    return 1;
}

#undef CUBE_GL_LOAD

static cube_gl_uint_t cube_compile_shader(const cube_gl_api_t *api,
                                          cube_gl_enum_t type,
                                          const char *source) {
    cube_gl_uint_t shader = api->create_shader(type);
    cube_gl_int_t compiled = 0;
    const char *sources[] = {source};
    if (shader == 0U) return 0U;
    api->shader_source(shader, 1, sources, 0);
    api->compile_shader(shader);
    api->get_shader_iv(shader, CUBE_GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        api->delete_shader(shader);
        return 0U;
    }
    return shader;
}

static cube_gl_uint_t cube_link_program(const cube_gl_api_t *api,
                                        cube_gl_uint_t vertex_shader,
                                        cube_gl_uint_t fragment_shader) {
    cube_gl_uint_t program = api->create_program();
    cube_gl_int_t linked = 0;
    if (program == 0U) return 0U;
    api->attach_shader(program, vertex_shader);
    api->attach_shader(program, fragment_shader);
    api->link_program(program);
    api->get_program_iv(program, CUBE_GL_LINK_STATUS, &linked);
    if (linked == 0) {
        api->delete_program(program);
        return 0U;
    }
    return program;
}

typedef struct cube_vertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
} cube_vertex_t;

static cube_vertex_t cube_corner(uint32_t index) {
    switch (index) {
    case 0U: return (cube_vertex_t){-1.0F, -1.0F, -1.0F, 1.0F, 0.0F, 0.0F};
    case 1U: return (cube_vertex_t){ 1.0F, -1.0F, -1.0F, 0.0F, 1.0F, 0.0F};
    case 2U: return (cube_vertex_t){ 1.0F,  1.0F, -1.0F, 0.0F, 0.0F, 1.0F};
    case 3U: return (cube_vertex_t){-1.0F,  1.0F, -1.0F, 1.0F, 1.0F, 0.0F};
    case 4U: return (cube_vertex_t){-1.0F, -1.0F,  1.0F, 1.0F, 0.0F, 1.0F};
    case 5U: return (cube_vertex_t){ 1.0F, -1.0F,  1.0F, 0.0F, 1.0F, 1.0F};
    case 6U: return (cube_vertex_t){ 1.0F,  1.0F,  1.0F, 1.0F, 1.0F, 1.0F};
    default: return (cube_vertex_t){-1.0F,  1.0F,  1.0F, 0.05F, 0.05F, 0.05F};
    }
}

static void cube_add_triangle(cube_vertex_t *vertices, uint32_t *count,
                              cube_vertex_t a, cube_vertex_t b,
                              cube_vertex_t c) {
    vertices[(*count)++] = a;
    vertices[(*count)++] = b;
    vertices[(*count)++] = c;
}

static uint32_t cube_build_vertices(cube_vertex_t *vertices) {
    uint32_t count = 0U;
#define CUBE_FACE(a, b, c, d)                                                \
    do {                                                                      \
        cube_add_triangle(vertices, &count, cube_corner(a), cube_corner(b),  \
                          cube_corner(c));                                   \
        cube_add_triangle(vertices, &count, cube_corner(a), cube_corner(c),  \
                          cube_corner(d));                                   \
    } while (0)
    /* 后、前、左、右、底、顶 */
    CUBE_FACE(1U, 0U, 3U, 2U);
    CUBE_FACE(4U, 5U, 6U, 7U);
    CUBE_FACE(0U, 4U, 7U, 3U);
    CUBE_FACE(5U, 1U, 2U, 6U);
    CUBE_FACE(0U, 1U, 5U, 4U);
    CUBE_FACE(3U, 7U, 6U, 2U);
#undef CUBE_FACE
    return count;
}

static void cube_sincos(float angle, float *sine, float *cosine) {
    int cosine_sign = 1;
    while (angle > CUBE_PI) angle -= CUBE_TWO_PI;
    while (angle < -CUBE_PI) angle += CUBE_TWO_PI;
    if (angle > CUBE_HALF_PI) {
        angle = CUBE_PI - angle;
        cosine_sign = -1;
    } else if (angle < -CUBE_HALF_PI) {
        angle = -CUBE_PI - angle;
        cosine_sign = -1;
    }
    float square = angle * angle;
    float sine_value = angle * (1.0F - square *
        (1.0F / 6.0F - square *
        (1.0F / 120.0F - square * (1.0F / 5040.0F))));
    float cosine_value = 1.0F - square *
        (0.5F - square * (1.0F / 24.0F - square * (1.0F / 720.0F)));
    *sine = sine_value;
    *cosine = cosine_sign < 0 ? -cosine_value : cosine_value;
}

static void cube_matrix_identity(float matrix[16]) {
    for (uint32_t index = 0U; index < 16U; ++index) matrix[index] = 0.0F;
    matrix[0] = 1.0F;
    matrix[5] = 1.0F;
    matrix[10] = 1.0F;
    matrix[15] = 1.0F;
}

static void cube_matrix_multiply(float output[16], const float left[16],
                                 const float right[16]) {
    float result[16];
    for (uint32_t column = 0U; column < 4U; ++column) {
        for (uint32_t row = 0U; row < 4U; ++row) {
            float value = 0.0F;
            for (uint32_t index = 0U; index < 4U; ++index)
                value += left[index * 4U + row] *
                         right[column * 4U + index];
            result[column * 4U + row] = value;
        }
    }
    for (uint32_t index = 0U; index < 16U; ++index) output[index] = result[index];
}

static void cube_build_mvp(float mvp[16], float yaw, float pitch,
                           uint32_t width, uint32_t height) {
    float sin_yaw;
    float cos_yaw;
    float sin_pitch;
    float cos_pitch;
    float model[16];
    float rotate_y[16];
    float rotate_x[16];
    float view[16];
    float projection[16];
    float view_model[16];
    float aspect = (float)width / (float)height;
    float focal = 1.0F / 0.57735026919F;
    float near_plane = 0.1F;
    float far_plane = 100.0F;

    cube_sincos(yaw, &sin_yaw, &cos_yaw);
    cube_sincos(pitch, &sin_pitch, &cos_pitch);
    cube_matrix_identity(rotate_y);
    rotate_y[0] = cos_yaw;
    rotate_y[2] = -sin_yaw;
    rotate_y[8] = sin_yaw;
    rotate_y[10] = cos_yaw;
    cube_matrix_identity(rotate_x);
    rotate_x[5] = cos_pitch;
    rotate_x[6] = sin_pitch;
    rotate_x[9] = -sin_pitch;
    rotate_x[10] = cos_pitch;
    cube_matrix_multiply(model, rotate_y, rotate_x);
    cube_matrix_identity(view);
    view[14] = -5.0F;
    for (uint32_t index = 0U; index < 16U; ++index) projection[index] = 0.0F;
    projection[0] = focal / aspect;
    /* glReadPixels 从最底行开始。垂直翻转渲染，使共享 surface 已按从上到下排列，无需逐行复制。 */
    projection[5] = -focal;
    projection[10] = (far_plane + near_plane) / (near_plane - far_plane);
    projection[11] = -1.0F;
    projection[14] = (2.0F * far_plane * near_plane) /
                     (near_plane - far_plane);
    cube_matrix_multiply(view_model, view, model);
    cube_matrix_multiply(mvp, projection, view_model);
}

static uint8_t cube_glyph_row(char character, uint32_t row) {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C},
    };
    static const uint8_t blank[7] = {0};
    if (row >= 7U) return 0U;
    if (character >= '0' && character <= '9')
        return digits[(uint32_t)(character - '0')][row];
    switch (character) {
    case 'F': { static const uint8_t glyph[7] =
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}; return glyph[row]; }
    case 'P': { static const uint8_t glyph[7] =
        {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}; return glyph[row]; }
    case 'S': { static const uint8_t glyph[7] =
        {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}; return glyph[row]; }
    case 'T': { static const uint8_t glyph[7] =
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return glyph[row]; }
    case 'I': { static const uint8_t glyph[7] =
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}; return glyph[row]; }
    case 'M': { static const uint8_t glyph[7] =
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}; return glyph[row]; }
    case 'E': { static const uint8_t glyph[7] =
        {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}; return glyph[row]; }
    case ':': { static const uint8_t glyph[7] =
        {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return glyph[row]; }
    case '.': { static const uint8_t glyph[7] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06}; return glyph[row]; }
    default: return blank[row];
    }
}

static void cube_fill_rect(uint8_t *pixels, uint32_t width, uint32_t height,
                           uint32_t x, uint32_t y, uint32_t rect_width,
                           uint32_t rect_height, uint8_t blue, uint8_t green,
                           uint8_t red) {
    if (x >= width || y >= height) return;
    if (rect_width > width - x) rect_width = width - x;
    if (rect_height > height - y) rect_height = height - y;
    for (uint32_t row = 0U; row < rect_height; ++row) {
        for (uint32_t column = 0U; column < rect_width; ++column) {
            uint8_t *pixel = pixels +
                ((uint64_t)(y + row) * width + x + column) * 4U;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            /* 窗口 surface 为 XRGB8888；第 3 字节不是 alpha 通道。 */
            pixel[3] = 0U;
        }
    }
}

static char *cube_overlay_u64(char *text, uint64_t value) {
    char digits[20];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) *text++ = digits[--count];
    return text;
}

static void cube_format_overlay(char text[64], uint32_t frames,
                                uint64_t elapsed_ns) {
    uint64_t fps = elapsed_ns == 0U ? 0U :
                   ((uint64_t)frames * 1000000000ULL) / elapsed_ns;
    uint64_t seconds = elapsed_ns / 1000000000ULL;
    uint64_t tenths = (elapsed_ns / 100000000ULL) % 10U;
    char *cursor = text;
    memcpy(cursor, "FPS:", 4U);
    cursor = cube_overlay_u64(cursor + 4U, fps);
    memcpy(cursor, " TIME:", 6U);
    cursor = cube_overlay_u64(cursor + 6U, seconds);
    *cursor++ = '.';
    *cursor++ = (char)('0' + tenths);
    *cursor++ = 'S';
    *cursor = '\0';
}

static void cube_draw_overlay(uint8_t *pixels, uint32_t width, uint32_t height,
                              uint32_t frames, uint64_t elapsed_ns) {
    char text[64];
    cube_format_overlay(text, frames, elapsed_ns);
    cube_fill_rect(pixels, width, height, 0U, 0U,
                   8U + (uint32_t)strlen(text) * 12U, 22U,
                   0x18U, 0x18U, 0x24U);
    for (uint32_t index = 0U; text[index] != '\0'; ++index) {
        uint32_t origin_x = 4U + index * 12U;
        for (uint32_t row = 0U; row < 7U; ++row) {
            uint8_t bits = cube_glyph_row(text[index], row);
            for (uint32_t column = 0U; column < 5U; ++column) {
                if ((bits & (uint8_t)(1U << (4U - column))) == 0U) continue;
                cube_fill_rect(pixels, width, height,
                               origin_x + column * 2U, 3U + row * 2U,
                               2U, 2U, 0xF0U, 0xF0U, 0xF0U);
            }
        }
    }
}

static int cube_present(const cube_gl_api_t *api, int descriptor,
                        uint32_t width, uint32_t height, uint32_t frames,
                        uint64_t elapsed_ns, cube_timing_sample_t *timing) {
    uint64_t read_begin = 0U, read_end = 0U, publish_begin = 0U, publish_end = 0U;
    int clock_valid = 0;
    if (timing != 0) timing->valid = 0;
    uint8_t *pixels = (uint8_t *)(uintptr_t)g_cube_window_buffer_address;
    uint64_t pixel_count = (uint64_t)width * height;
    if (api == 0 || descriptor < 0 || width == 0U || height == 0U ||
        pixel_count > SIZE_MAX / 4U || pixels == 0) return 0;
    if (timing != 0) clock_valid = cube_now_ns(&read_begin);
    api->read_pixels(0, 0, (cube_gl_sizei_t)width, (cube_gl_sizei_t)height,
                     CUBE_GL_BGRA, CUBE_GL_UNSIGNED_BYTE, pixels);
    if (clock_valid) clock_valid = cube_now_ns(&read_end);
    if (api->get_error() != CUBE_GL_NO_ERROR) return 0;
    if (g_cube_shared_buffer_reported == 0U) {
        uint64_t nonclear = 0U;
        uint64_t reserved_nonzero = 0U;
        uint64_t hash = 1469598103934665603ULL;
        uint32_t bounds_left = width;
        uint32_t bounds_top = height;
        uint32_t bounds_right = 0U;
        uint32_t bounds_bottom = 0U;
        uint8_t minimum[3] = {255U, 255U, 255U};
        uint8_t maximum[3] = {0U, 0U, 0U};
        uint64_t center = ((uint64_t)(height / 2U) * width +
                           width / 2U) * 4U;

        for (uint32_t y = 0U; y < height; ++y) {
            for (uint32_t x = 0U; x < width; ++x) {
                uint64_t offset = ((uint64_t)y * width + x) * 4U;
                if (pixels[offset + 3U] != 0U) ++reserved_nonzero;
                /* 诊断以逻辑 RGB 顺序报告，而共享 XRGB surface 按 B、G、R、X 字节存储。 */
                bool differs = pixels[offset + 2U] != 9U ||
                               pixels[offset + 1U] != 11U ||
                               pixels[offset] != 19U;
                if (differs) {
                    ++nonclear;
                    if (x < bounds_left) bounds_left = x;
                    if (y < bounds_top) bounds_top = y;
                    if (x + 1U > bounds_right) bounds_right = x + 1U;
                    if (y + 1U > bounds_bottom) bounds_bottom = y + 1U;
                }
                for (uint32_t component = 0U; component < 3U; ++component) {
                    uint8_t value = pixels[offset + 2U - component];
                    if (value < minimum[component]) minimum[component] = value;
                    if (value > maximum[component]) maximum[component] = value;
                    hash ^= value;
                    hash *= 1099511628211ULL;
                }
                hash ^= pixels[offset + 3U];
                hash *= 1099511628211ULL;
            }
        }
        cube_emit("OS_CUBE_READBACK center=");
        cube_emit_u64(pixels[center + 2U]);
        cube_emit(",");
        cube_emit_u64(pixels[center + 1U]);
        cube_emit(",");
        cube_emit_u64(pixels[center]);
        cube_emit(",");
        cube_emit_u64(pixels[center + 3U]);
        cube_emit(" nonclear=");
        cube_emit_u64(nonclear);
        cube_emit(" bounds=");
        cube_emit_u64(bounds_left);
        cube_emit(",");
        cube_emit_u64(bounds_top);
        cube_emit(",");
        cube_emit_u64(bounds_right);
        cube_emit(",");
        cube_emit_u64(bounds_bottom);
        cube_emit(" min=");
        cube_emit_u64(minimum[0]);
        cube_emit(",");
        cube_emit_u64(minimum[1]);
        cube_emit(",");
        cube_emit_u64(minimum[2]);
        cube_emit(" max=");
        cube_emit_u64(maximum[0]);
        cube_emit(",");
        cube_emit_u64(maximum[1]);
        cube_emit(",");
        cube_emit_u64(maximum[2]);
        cube_emit(" hash=");
        cube_emit_u64(hash);
        cube_emit("\n");
        cube_emit("OS_CUBE_GPU_SHARED_BUFFER address=");
        cube_emit_u64(g_cube_window_buffer_address);
        cube_emit(" bytes=");
        cube_emit_u64(pixel_count * 4U);
        cube_emit("\n");
        cube_emit("OS_CUBE_XRGB_NONZERO=");
        cube_emit_u64(reserved_nonzero);
        cube_emit("\n");
        if (reserved_nonzero != 0U) return 0;
        g_cube_shared_buffer_reported = 1U;
    }
    /* FBO 清屏和片元输出已生成 XRGB 的高位零字节。不要再次读/改/写整个共享 surface。 */
    cube_draw_overlay(pixels, width, height, frames, elapsed_ns);
    if (clock_valid) clock_valid = cube_now_ns(&publish_begin);
    int presented = cube_window_command(descriptor, LINUX_WINDOW_OPERATION_PRESENT,
                                        0, 0, width, height);
    if (clock_valid) clock_valid = cube_now_ns(&publish_end);
    if (timing != 0 && presented && clock_valid && read_end >= read_begin &&
        publish_end >= publish_begin) {
        timing->readback_ns = read_end - read_begin;
        timing->publish_ns = publish_end - publish_begin;
        timing->valid = 1;
    }
    return presented;
}

static void cube_record_profile(uint32_t display_width, uint32_t display_height,
                                uint32_t width, uint32_t height,
                                const char *renderer) {
    char line[384];
    if (renderer == 0) return;
    int length = snprintf(line, sizeof(line),
        "OS_CUBE_PROFILE display=%ux%u surface=%ux%u renderer=%.160s\n"
        "readback=GPU_wait_and_CPU_copy submit=window_present_ACK_wait\n",
        display_width, display_height, width, height, renderer);
    if (length <= 0 || (size_t)length >= sizeof(line)) return;
    cube_record_log("/log/cube-profile.log", line);
}

/* 回读时间包含等待先前 GPU 工作。旧版 PRESENT 系统调用除了提交损坏区域，还会等待合成器消费/ACK；
 * 因此该值既不是单独的 CPU 提交开销，也不是物理显示器刷新时间。 */
static void cube_report_timing(const cube_timing_totals_t *totals) {
    if (totals->frames == 0U) return;
    char line[256];
    int length = snprintf(line, sizeof(line),
        "OS_CUBE_TIMING frames=%llu readback_avg_us=%llu readback_max_us=%llu "
        "submit_avg_us=%llu submit_max_us=%llu\n",
        (unsigned long long)totals->frames,
        (unsigned long long)(totals->readback_ns / totals->frames / 1000U),
        (unsigned long long)(totals->readback_max_ns / 1000U),
        (unsigned long long)(totals->publish_ns / totals->frames / 1000U),
        (unsigned long long)(totals->publish_max_ns / 1000U));
    if (length <= 0 || (size_t)length >= sizeof(line)) return;
    cube_emit(line);
    /* 桌面启动可能丢弃 stdout。在启动卷中只保留最新区间，每 5 秒写入一条小型有界记录。 */
    cube_record_log("/log/cube-timing.log", line);
}

static int cube_process_events(int descriptor, int *dragging, int *quit,
                               float *yaw, float *pitch,
                               uint32_t *drag_events) {
    for (;;) {
        os_window_event_t event;
        ssize_t result = read(descriptor, &event, sizeof(event));
        if (result < 0) {
            int error = *__errno_location();
            if (error == CUBE_ERRNO_AGAIN) return 1;
            return 0;
        }
        if (result == 0) return 0;
        if ((size_t)result != sizeof(event)) return 0;
        if (event.type == OS_WINDOW_EVENT_CLOSE_REQUEST) {
            *quit = 1;
            continue;
        }
        if (event.type != OS_WINDOW_EVENT_INPUT) continue;
        if (event.input.type == OS_INPUT_EVENT_KEY &&
            event.input.code == CUBE_ESC_KEY &&
            event.input.value != OS_INPUT_VALUE_RELEASE) {
            *quit = 1;
        } else if (event.input.type == OS_INPUT_EVENT_BUTTON &&
                   event.input.code == OS_INPUT_BUTTON_LEFT) {
            if (event.input.value == OS_INPUT_VALUE_PRESS && !*dragging) {
                *dragging = 1;
                cube_emit("OS_CUBE_DRAG_BEGIN\n");
            } else if (event.input.value == OS_INPUT_VALUE_RELEASE) {
                *dragging = 0;
            }
        } else if (*dragging && event.input.type == OS_INPUT_EVENT_RELATIVE) {
            if (event.input.code == OS_INPUT_REL_X) {
                *yaw += (float)event.input.value * 0.01F;
                ++*drag_events;
            } else if (event.input.code == OS_INPUT_REL_Y) {
                *pitch += (float)event.input.value * 0.01F;
                if (*pitch > 1.45F) *pitch = 1.45F;
                if (*pitch < -1.45F) *pitch = -1.45F;
                ++*drag_events;
            }
            if (*drag_events == 1U) cube_emit("OS_CUBE_DRAG_PASS\n");
        }
    }
}

int main(void) {
    static const char vertex_source[] =
        "#version 330 core\n"
        "layout(location = 0) in vec3 cube_position;\n"
        "layout(location = 1) in vec3 cube_color;\n"
        "uniform mat4 mvp;\n"
        "out vec3 interpolated_color;\n"
        "void main() { gl_Position = mvp * vec4(cube_position, 1.0); "
        "interpolated_color = cube_color; }\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "in vec3 interpolated_color;\n"
        "out vec4 cube_output;\n"
        "void main() { cube_output = vec4(interpolated_color, 0.0); }\n";
    cube_egl_display_t display = CUBE_EGL_NO_DISPLAY;
    cube_egl_context_t context = CUBE_EGL_NO_CONTEXT;
    cube_egl_surface_t surface = CUBE_EGL_NO_SURFACE;
    cube_egl_device_t devices[8] = {0};
    cube_egl_config_t config = 0;
    cube_egl_int_t device_count = 0;
    cube_egl_int_t config_count = 0;
    cube_egl_int_t major = 0;
    cube_egl_int_t minor = 0;
    cube_egl_query_devices_fn query_devices = 0;
    cube_egl_get_platform_display_fn get_platform_display = 0;
    cube_egl_get_display_fn get_display = 0;
    cube_egl_initialize_fn initialize = 0;
    cube_egl_bind_api_fn bind_api = 0;
    cube_egl_choose_config_fn choose_config = 0;
    cube_egl_create_context_fn create_context = 0;
    cube_egl_create_pbuffer_fn create_pbuffer = 0;
    cube_egl_make_current_fn make_current = 0;
    cube_egl_destroy_context_fn destroy_context = 0;
    cube_egl_destroy_surface_fn destroy_surface = 0;
    cube_egl_terminate_fn terminate_display = 0;
    cube_egl_get_error_fn get_egl_error = 0;
    cube_egl_get_proc_address_fn get_proc_address = 0;
    cube_gl_api_t gl = {0};
    cube_vertex_t vertices[36];
    cube_gl_uint_t vertex_shader = 0U;
    cube_gl_uint_t fragment_shader = 0U;
    cube_gl_uint_t program = 0U;
    cube_gl_uint_t vertex_array = 0U;
    cube_gl_uint_t vertex_buffer = 0U;
    cube_gl_uint_t framebuffer = 0U;
    cube_gl_uint_t textures[2] = {0U, 0U};
    cube_gl_int_t mvp_location = -1;
    void *egl_handle = 0;
    void *gl_handle = 0;
    int window_descriptor = -1;
    int window_closed = 0;
    int gl_loaded = 0;
    int result = 1;
    const char *failure_marker = "OS_CUBE_FAILURE_UNKNOWN\n";
    float yaw = 0.55F;
    float pitch = 0.35F;
    int dragging = 0;
    int quit = 0;
    uint32_t drag_events = 0U;
    uint32_t rendered_frames = 0U;
    uint64_t start_ns = 0U;
    uint64_t timing_report_ns = 0U;
    cube_timing_totals_t timing_totals = {0};
#if CUBE_FRAME_PACING
    uint64_t next_deadline = 0U;
#endif
    uint32_t cube_width = CUBE_WIDTH;
    uint32_t cube_height = CUBE_HEIGHT;
    int32_t cube_x = CUBE_WINDOW_X;
    int32_t cube_y = CUBE_WINDOW_Y;
    int32_t create_x;
    int32_t create_y;
    uint32_t display_width = 0U;
    uint32_t display_height = 0U;
    linux_window_create_t window_create = {
        .x = 0,
        .y = 0,
        .width = 0,
        .height = 0,
        /* 由 Ring0 绘制边框/标题栏，使其成为普通窗口。 */
        .flags = OS_WINDOW_RESIZABLE | OS_WINDOW_STANDARD_CAPTION_CONTROLS,
        .background = 0x00121C2EU,
        .address = CUBE_HBUFFER_ADDRESS,
        .title = "立方体 - OpenGL",
    };

    cube_record_result("OS_CUBE_MAIN_ENTERED\n");
    /* 查询来宾实际输出模式，不要假定固件模式。当前 OVMF 构建中，无 PCI 资源的 RAMFB 路径
     * 刻意限制为 800x600；而真实 1080p GOP 无需后备模式。 */
    if (CUBE_INTERACTIVE && cube_query_display(&display_width,
                                               &display_height)) {
        if (!cube_fit_geometry(display_width, display_height,
                                &cube_width, &cube_height,
                                &cube_x, &cube_y)) {
            failure_marker = "OS_CUBE_DISPLAY_GEOMETRY_FAIL\n";
            goto cleanup;
        }
    }
    create_x = CUBE_INTERACTIVE ? 0 : cube_x;
    create_y = CUBE_INTERACTIVE ? 0 : cube_y;
    window_create.x = create_x;
    window_create.y = create_y;
    window_create.width = cube_width;
    window_create.height = cube_height;

    cube_emit("OS_CUBE_EGL_BEGIN\n");
    {
        long create_result = syscall(
            (long)LINUX_SYS_WINDOW_CREATE, &window_create);
        if (create_result < 0L || create_result > (long)INT32_MAX ||
            window_create.address != CUBE_HBUFFER_ADDRESS ||
            window_create.identifier == 0U ||
            window_create.buffer_size == 0U) {
            failure_marker = "OS_CUBE_WINDOW_CREATE_FAIL\n";
            goto cleanup;
        }
        window_descriptor = (int)create_result;
        g_cube_window_buffer_address = window_create.address;
    }
    cube_emit("OS_CUBE_WINDOW_CREATED\n");
    if (window_descriptor < 0) {
        failure_marker = "OS_CUBE_WINDOW_FD_FAIL\n";
        goto cleanup;
    }
    if (!cube_window_command(window_descriptor,
                             LINUX_WINDOW_OPERATION_RESIZE, 0, 0,
                             cube_width, cube_height)) {
        /* 较旧内核可能尚未提供 DISPLAY_GET_INFO。初始交互窗口在 (0,0) 创建，
         * 因此按已知的 800x600 后备模式，重试可容纳的最大装饰客户端区域。 */
        if (!CUBE_INTERACTIVE ||
            cube_width <= CUBE_WINDOW_FRAME_EXTRA ||
            cube_height <= CUBE_WINDOW_FRAME_EXTRA + CUBE_WINDOW_TITLEBAR) {
            failure_marker = "OS_CUBE_WINDOW_RESIZE_FAIL\n";
            goto cleanup;
        }
        cube_width -= CUBE_WINDOW_FRAME_EXTRA;
        cube_height -= CUBE_WINDOW_FRAME_EXTRA + CUBE_WINDOW_TITLEBAR;
        cube_x = 0;
        cube_y = 0;
        if (!cube_window_command(window_descriptor,
                                 LINUX_WINDOW_OPERATION_RESIZE, 0, 0,
                                 cube_width, cube_height)) {
            failure_marker = "OS_CUBE_WINDOW_RESIZE_FAIL\n";
            goto cleanup;
        }
        cube_emit("OS_CUBE_WINDOW_FALLBACK\n");
    }
    /* 首帧前测试 Linux 窗口缩放/移动适配器。 */
    if (!cube_window_command(window_descriptor,
                             LINUX_WINDOW_OPERATION_MOVE,
                             cube_x, cube_y, 0U, 0U)) {
        failure_marker = "OS_CUBE_WINDOW_MOVE_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_WINDOW_POSITION x=");
    cube_emit_u64((uint64_t)(uint32_t)cube_x);
    cube_emit(" y=");
    cube_emit_u64((uint64_t)(uint32_t)cube_y);
    cube_emit("\n");
    cube_emit("OS_CUBE_WINDOW_READY\n");
    if (setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               "/usr/apps/cube/egl/10_nvidia.json", 1) != 0) {
        failure_marker = "OS_CUBE_GLVND_ENV_FAIL\n";
        goto cleanup;
    }
    egl_handle = dlopen("/usr/apps/cube/lib/libEGL.so.1",
                        CUBE_RTLD_NOW | CUBE_RTLD_GLOBAL);
    if (egl_handle == 0) {
        failure_marker = "OS_CUBE_LIBEGL_FAIL\n";
        goto cleanup;
    }
    get_proc_address = (cube_egl_get_proc_address_fn)
        dlsym(egl_handle, "eglGetProcAddress");
    get_display = (cube_egl_get_display_fn)dlsym(egl_handle, "eglGetDisplay");
    query_devices = (cube_egl_query_devices_fn)
        get_proc_address("eglQueryDevicesEXT");
    get_platform_display = (cube_egl_get_platform_display_fn)
        get_proc_address("eglGetPlatformDisplayEXT");
    initialize = (cube_egl_initialize_fn)dlsym(egl_handle, "eglInitialize");
    bind_api = (cube_egl_bind_api_fn)dlsym(egl_handle, "eglBindAPI");
    choose_config = (cube_egl_choose_config_fn)
        dlsym(egl_handle, "eglChooseConfig");
    create_context = (cube_egl_create_context_fn)
        dlsym(egl_handle, "eglCreateContext");
    create_pbuffer = (cube_egl_create_pbuffer_fn)
        dlsym(egl_handle, "eglCreatePbufferSurface");
    make_current = (cube_egl_make_current_fn)dlsym(egl_handle,
                                                    "eglMakeCurrent");
    destroy_context = (cube_egl_destroy_context_fn)
        dlsym(egl_handle, "eglDestroyContext");
    destroy_surface = (cube_egl_destroy_surface_fn)
        dlsym(egl_handle, "eglDestroySurface");
    terminate_display = (cube_egl_terminate_fn)dlsym(egl_handle, "eglTerminate");
    get_egl_error = (cube_egl_get_error_fn)dlsym(egl_handle, "eglGetError");
    if (get_proc_address == 0 || get_display == 0 || query_devices == 0 ||
        initialize == 0 || bind_api == 0 || choose_config == 0 ||
        create_context == 0 || create_pbuffer == 0 || make_current == 0 ||
        destroy_context == 0 || destroy_surface == 0 ||
        terminate_display == 0 || get_egl_error == 0) {
        failure_marker = "OS_CUBE_EGL_SYMBOL_FAIL\n";
        goto cleanup;
    }
    if (query_devices(8, devices, &device_count) != CUBE_EGL_TRUE ||
        device_count <= 0 || devices[0] == 0) {
        failure_marker = "OS_CUBE_NVIDIA_DEVICE_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_NVIDIA_DEVICE_PASS\n");
    display = get_platform_display != 0 ?
              get_platform_display(CUBE_EGL_PLATFORM_DEVICE_EXT,
                                    devices[0], 0) : CUBE_EGL_NO_DISPLAY;
    if (display == CUBE_EGL_NO_DISPLAY) display = get_display(0);
    if (display == CUBE_EGL_NO_DISPLAY ||
        bind_api(CUBE_EGL_OPENGL_API) != CUBE_EGL_TRUE ||
        initialize(display, &major, &minor) != CUBE_EGL_TRUE) {
        failure_marker = "OS_CUBE_EGL_INITIALIZE_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_EGL_INITIALIZED\n");
    {
        const cube_egl_int_t attributes[] = {
            CUBE_EGL_SURFACE_TYPE, CUBE_EGL_PBUFFER_BIT,
            CUBE_EGL_RENDERABLE_TYPE, CUBE_EGL_OPENGL_BIT,
            CUBE_EGL_RED_SIZE, 8, CUBE_EGL_GREEN_SIZE, 8,
            CUBE_EGL_BLUE_SIZE, 8, CUBE_EGL_ALPHA_SIZE, 8,
            CUBE_EGL_NONE
        };
        if (choose_config(display, attributes, &config, 1, &config_count) !=
                CUBE_EGL_TRUE || config_count <= 0 || config == 0) {
            failure_marker = "OS_CUBE_CONFIG_FAIL\n";
            goto cleanup;
        }
    }
    context = create_context(
        display, config, CUBE_EGL_NO_CONTEXT,
        (const cube_egl_int_t[]){
            CUBE_EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            CUBE_EGL_CONTEXT_MINOR_VERSION_KHR, 3,
            CUBE_EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            CUBE_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            CUBE_EGL_NONE});
    if (context == CUBE_EGL_NO_CONTEXT) {
        failure_marker = "OS_CUBE_CONTEXT_CREATE_FAIL\n";
        goto cleanup;
    }
    surface = create_pbuffer(display, config,
                             (const cube_egl_int_t[]){
                                 CUBE_EGL_WIDTH, (cube_egl_int_t)cube_width,
                                 CUBE_EGL_HEIGHT, (cube_egl_int_t)cube_height,
                                 CUBE_EGL_NONE});
    if (surface == CUBE_EGL_NO_SURFACE) {
        cube_emit("OS_CUBE_PBUFFER_ERROR=");
        cube_emit_u64((uint64_t)get_egl_error());
        cube_emit("\n");
        failure_marker = "OS_CUBE_PBUFFER_CREATE_FAIL\n";
        goto cleanup;
    }
    if (make_current(display, surface, surface, context) != CUBE_EGL_TRUE) {
        failure_marker = "OS_CUBE_CONTEXT_CURRENT_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_CONTEXT_CURRENT\n");
    gl_handle = dlopen("libOpenGL.so.0", CUBE_RTLD_NOW | CUBE_RTLD_GLOBAL);
    if (gl_handle == 0 || !cube_load_gl_api(gl_handle, get_proc_address, &gl)) {
        failure_marker = "OS_CUBE_GL_SYMBOL_FAIL\n";
        goto cleanup;
    }
    gl_loaded = 1;
    {
        const unsigned char *vendor = gl.get_string(CUBE_GL_VENDOR);
        const unsigned char *renderer = gl.get_string(CUBE_GL_RENDERER);
        const unsigned char *version = gl.get_string(CUBE_GL_VERSION);
        if (vendor == 0 || renderer == 0 ||
            !cube_contains_nvidia((const char *)vendor)) {
            failure_marker = "OS_CUBE_GL_VENDOR_FAIL\n";
            goto cleanup;
        }
        cube_emit_value("OS_CUBE_GL_VENDOR=", (const char *)vendor);
        cube_emit_value("OS_CUBE_GL_RENDERER=", (const char *)renderer);
        cube_emit_value("OS_CUBE_GL_VERSION=", (const char *)version);
        cube_record_profile(display_width, display_height, cube_width,
                            cube_height, (const char *)renderer);
    }
    (void)gl.get_error();
    gl.gen_framebuffers(1, &framebuffer);
    gl.gen_textures(2, textures);
    if (gl.get_error() != CUBE_GL_NO_ERROR || framebuffer == 0U ||
        textures[0] == 0U || textures[1] == 0U) {
        failure_marker = "OS_CUBE_FBO_CREATE_FAIL\n";
        goto cleanup;
    }
    gl.bind_framebuffer(CUBE_GL_FRAMEBUFFER, framebuffer);
    gl.bind_texture(CUBE_GL_TEXTURE_2D, textures[0]);
    gl.tex_parameteri(CUBE_GL_TEXTURE_2D, CUBE_GL_TEXTURE_MIN_FILTER,
                      (cube_gl_int_t)CUBE_GL_NEAREST);
    gl.tex_parameteri(CUBE_GL_TEXTURE_2D, CUBE_GL_TEXTURE_MAG_FILTER,
                      (cube_gl_int_t)CUBE_GL_NEAREST);
    gl.tex_image_2d(CUBE_GL_TEXTURE_2D, 0, (cube_gl_int_t)CUBE_GL_RGBA8,
                    (cube_gl_sizei_t)cube_width,
                    (cube_gl_sizei_t)cube_height, 0, CUBE_GL_RGBA,
                    CUBE_GL_UNSIGNED_BYTE, 0);
    {
        cube_gl_int_t actual_width = 0, actual_height = 0, actual_format = 0;
        gl.get_tex_level_parameteriv(CUBE_GL_TEXTURE_2D, 0,
            CUBE_GL_TEXTURE_WIDTH, &actual_width);
        gl.get_tex_level_parameteriv(CUBE_GL_TEXTURE_2D, 0,
            CUBE_GL_TEXTURE_HEIGHT, &actual_height);
        gl.get_tex_level_parameteriv(CUBE_GL_TEXTURE_2D, 0,
            CUBE_GL_TEXTURE_INTERNAL_FORMAT, &actual_format);
        cube_emit("OS_CUBE_COLOR_TEXTURE width=");
        cube_emit_u64((uint32_t)actual_width);
        cube_emit(" height=");
        cube_emit_u64((uint32_t)actual_height);
        cube_emit(" format=");
        cube_emit_u64((uint32_t)actual_format);
        cube_emit("\n");
    }
    gl.framebuffer_texture_2d(CUBE_GL_FRAMEBUFFER, CUBE_GL_COLOR_ATTACHMENT0,
                              CUBE_GL_TEXTURE_2D, textures[0], 0);
    cube_emit("OS_CUBE_COLOR_FBO_STATUS=");
    cube_emit_u64(gl.check_framebuffer_status(CUBE_GL_FRAMEBUFFER));
    cube_emit("\n");
    gl.bind_texture(CUBE_GL_TEXTURE_2D, textures[1]);
    gl.tex_parameteri(CUBE_GL_TEXTURE_2D, CUBE_GL_TEXTURE_MIN_FILTER,
                      (cube_gl_int_t)CUBE_GL_NEAREST);
    gl.tex_parameteri(CUBE_GL_TEXTURE_2D, CUBE_GL_TEXTURE_MAG_FILTER,
                      (cube_gl_int_t)CUBE_GL_NEAREST);
    gl.tex_image_2d(CUBE_GL_TEXTURE_2D, 0,
                    (cube_gl_int_t)CUBE_GL_DEPTH_COMPONENT24,
                    (cube_gl_sizei_t)cube_width,
                    (cube_gl_sizei_t)cube_height, 0, CUBE_GL_DEPTH_COMPONENT,
                    CUBE_GL_UNSIGNED_INT, 0);
    gl.framebuffer_texture_2d(CUBE_GL_FRAMEBUFFER, CUBE_GL_DEPTH_ATTACHMENT,
                              CUBE_GL_TEXTURE_2D, textures[1], 0);
    cube_gl_enum_t attachment_error = gl.get_error();
    cube_gl_enum_t framebuffer_status =
        gl.check_framebuffer_status(CUBE_GL_FRAMEBUFFER);
    cube_gl_enum_t status_error = gl.get_error();
    if (attachment_error != CUBE_GL_NO_ERROR ||
        framebuffer_status != CUBE_GL_FRAMEBUFFER_COMPLETE ||
        status_error != CUBE_GL_NO_ERROR) {
        cube_emit("OS_CUBE_FBO_STATUS=");
        cube_emit_u64(framebuffer_status);
        cube_emit(" attachment_error=");
        cube_emit_u64(attachment_error);
        cube_emit(" status_error=");
        cube_emit_u64(status_error);
        cube_emit("\n");
        failure_marker = "OS_CUBE_FBO_STATUS_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_FBO_COMPLETE\n");
    vertex_shader = cube_compile_shader(&gl, CUBE_GL_VERTEX_SHADER,
                                        vertex_source);
    fragment_shader = cube_compile_shader(&gl, CUBE_GL_FRAGMENT_SHADER,
                                          fragment_source);
    program = (vertex_shader != 0U && fragment_shader != 0U) ?
              cube_link_program(&gl, vertex_shader, fragment_shader) : 0U;
    if (program == 0U) {
        failure_marker = "OS_CUBE_SHADER_FAIL\n";
        goto cleanup;
    }
    mvp_location = gl.get_uniform_location(program, "mvp");
    if (mvp_location < 0) {
        failure_marker = "OS_CUBE_UNIFORM_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_SHADER_READY\n");
    if (cube_build_vertices(vertices) != 36U) {
        failure_marker = "OS_CUBE_GEOMETRY_FAIL\n";
        goto cleanup;
    }
    gl.gen_vertex_arrays(1, &vertex_array);
    gl.gen_buffers(1, &vertex_buffer);
    if (vertex_array == 0U || vertex_buffer == 0U) {
        failure_marker = "OS_CUBE_BUFFER_CREATE_FAIL\n";
        goto cleanup;
    }
    gl.bind_vertex_array(vertex_array);
    gl.bind_buffer(CUBE_GL_ARRAY_BUFFER, vertex_buffer);
    gl.buffer_data(CUBE_GL_ARRAY_BUFFER, (cube_gl_sizeiptr_t)sizeof(vertices),
                   vertices, CUBE_GL_STATIC_DRAW);
    gl.enable_vertex_attrib_array(0U);
    gl.vertex_attrib_pointer(0U, 3, CUBE_GL_FLOAT, 0U,
                             (cube_gl_sizei_t)sizeof(cube_vertex_t), 0);
    gl.enable_vertex_attrib_array(1U);
    gl.vertex_attrib_pointer(1U, 3, CUBE_GL_FLOAT, 0U,
                             (cube_gl_sizei_t)sizeof(cube_vertex_t),
                             (const void *)(uintptr_t)(3U * sizeof(float)));
    gl.use_program(program);
    gl.enable(CUBE_GL_DEPTH_TEST);
    gl.depth_func(CUBE_GL_LESS);
    gl.viewport(0, 0, (cube_gl_sizei_t)cube_width,
                (cube_gl_sizei_t)cube_height);
    if (gl.get_error() != CUBE_GL_NO_ERROR) {
        failure_marker = "OS_CUBE_GL_SETUP_FAIL\n";
        goto cleanup;
    }
    cube_emit("OS_CUBE_MOUSE_DRAG_READY\n");
    if (!cube_now_ns(&start_ns)) {
        failure_marker = "OS_CUBE_CLOCK_FAIL\n";
        goto cleanup;
    }
#if CUBE_FRAME_PACING
    next_deadline = start_ns;
#endif
    for (uint32_t frame = 0U; CUBE_FRAME_CONDITION; ++frame) {
        uint64_t now;
        uint64_t elapsed_ns;
        float mvp[16];
        cube_timing_sample_t timing = {0};
        if (!cube_process_events(window_descriptor, &dragging, &quit, &yaw,
                                 &pitch, &drag_events)) {
            failure_marker = "OS_CUBE_WINDOW_READ_FAIL\n";
            goto cleanup;
        }
        if (quit) break;
        if (!cube_now_ns(&now)) {
            failure_marker = "OS_CUBE_CLOCK_FAIL\n";
            goto cleanup;
        }
        elapsed_ns = now >= start_ns ? now - start_ns : 0U;
        cube_build_mvp(mvp, yaw, pitch, cube_width, cube_height);
        gl.uniform_matrix_4fv(mvp_location, 1, 0U, mvp);
        gl.clear_color(0.035F, 0.045F, 0.075F, 0.0F);
        gl.clear(CUBE_GL_COLOR_BUFFER_BIT | CUBE_GL_DEPTH_BUFFER_BIT);
        gl.draw_arrays(CUBE_GL_TRIANGLES, 0, 36);
        /* 下方写入客户端内存的 ReadPixels 会自行等待此前渲染完成；单独调用 Finish 会额外增加一次
         * 整个上下文的 CPU/GPU 等待。 */
        if (gl.get_error() != CUBE_GL_NO_ERROR ||
            !cube_present(&gl, window_descriptor, cube_width, cube_height,
                          frame + 1U, elapsed_ns, &timing)) {
            failure_marker = "OS_CUBE_PRESENT_FAIL\n";
            goto cleanup;
        }
        rendered_frames = frame + 1U;
        /* 排除首帧像素验证和驱动预热时间。 */
        if (frame != 0U) cube_timing_add(&timing_totals, &timing);
        if (elapsed_ns >= timing_report_ns &&
            elapsed_ns - timing_report_ns >= 5000000000ULL) {
            cube_report_timing(&timing_totals);
            timing_totals = (cube_timing_totals_t){0};
            timing_report_ns = elapsed_ns;
        }
        if (rendered_frames == 1U) cube_emit("OS_CUBE_VISIBLE\n");
#if CUBE_FRAME_PACING
        if (next_deadline > UINT64_MAX - CUBE_FRAME_NS) {
            next_deadline = now;
        } else {
            next_deadline += CUBE_FRAME_NS;
        }
        if (!cube_now_ns(&now)) {
            failure_marker = "OS_CUBE_CLOCK_FAIL\n";
            goto cleanup;
        }
        if (now < next_deadline) {
            if (!cube_wait_until(next_deadline)) {
                failure_marker = "OS_CUBE_FRAME_WAIT_FAIL\n";
                goto cleanup;
            }
        } else {
            next_deadline = now;
        }
#endif
    }
    if (rendered_frames == 0U) {
        failure_marker = "OS_CUBE_NO_FRAME_FAIL\n";
        goto cleanup;
    }
    cube_report_timing(&timing_totals);
    {
        uint64_t finish_ns;
        uint64_t elapsed_ns;
        if (!cube_now_ns(&finish_ns)) {
            failure_marker = "OS_CUBE_CLOCK_FAIL\n";
            goto cleanup;
        }
        elapsed_ns = finish_ns >= start_ns ? finish_ns - start_ns : 0U;
        cube_emit("OS_CUBE_FRAME_PASS frames=");
        cube_emit_u64(rendered_frames);
        cube_emit(" fps=");
        cube_emit_u64(elapsed_ns == 0U ? 0U :
                      ((uint64_t)rendered_frames * 1000000000ULL) /
                      elapsed_ns);
        cube_emit(" time_tenths=");
        cube_emit_u64(elapsed_ns / 100000000ULL);
        cube_emit("\n");
        if (drag_events == 0U)
            cube_emit("OS_CUBE_DRAG_NO_INPUT\n");
    }
    if (!cube_window_command(window_descriptor, LINUX_WINDOW_OPERATION_CLOSE,
                             0, 0, 0U, 0U)) {
        failure_marker = "OS_CUBE_CLOSE_FAIL\n";
        goto cleanup;
    }
    window_closed = 1;
    cube_emit("OS_CUBE_CLOSE_PASS\n");
    if (close(window_descriptor) != 0) {
        window_descriptor = -1;
        failure_marker = "OS_CUBE_FD_CLOSE_FAIL\n";
        goto cleanup;
    }
    window_descriptor = -1;
    result = 0;

cleanup:
    if (gl_loaded) {
        if (vertex_buffer != 0U) gl.delete_buffers(1, &vertex_buffer);
        if (vertex_array != 0U) gl.delete_vertex_arrays(1, &vertex_array);
        if (program != 0U) gl.delete_program(program);
        if (vertex_shader != 0U) gl.delete_shader(vertex_shader);
        if (fragment_shader != 0U) gl.delete_shader(fragment_shader);
        if (textures[0] != 0U || textures[1] != 0U)
            gl.delete_textures(2, textures);
        if (framebuffer != 0U) gl.delete_framebuffers(1, &framebuffer);
    }
    if (surface != CUBE_EGL_NO_SURFACE && destroy_surface != 0)
        (void)destroy_surface(display, surface);
    if (context != CUBE_EGL_NO_CONTEXT && destroy_context != 0)
        (void)destroy_context(display, context);
    if (display != CUBE_EGL_NO_DISPLAY && terminate_display != 0)
        (void)terminate_display(display);
    if (gl_handle != 0 && dlclose(gl_handle) != 0) result = 1;
    if (egl_handle != 0 && dlclose(egl_handle) != 0) result = 1;
    if (window_descriptor >= 0) {
        if (!window_closed) {
            if (cube_window_command(window_descriptor,
                                    LINUX_WINDOW_OPERATION_CLOSE,
                                    0, 0, 0U, 0U)) {
                window_closed = 1;
            } else if (result == 0) {
                failure_marker = "OS_CUBE_CLOSE_FAIL\n";
                result = 1;
            }
        }
        if (close(window_descriptor) != 0 && result == 0) {
            failure_marker = "OS_CUBE_FD_CLOSE_FAIL\n";
            result = 1;
        }
        window_descriptor = -1;
    }
    if (result != 0) {
        cube_record_result(failure_marker);
        cube_emit(failure_marker);
        cube_emit("OS_CUBE_FAIL\n");
        return 1;
    }
    cube_record_result("OS_CUBE_PASS\n");
    cube_emit("OS_CUBE_PASS\n");
    return 0;
}

#undef CUBE_FRAME_CONDITION
