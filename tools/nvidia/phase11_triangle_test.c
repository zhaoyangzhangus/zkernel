#ifndef P11_APP_ROOT
#define P11_APP_ROOT "/usr/apps/p11-triangle"
#endif
/* Phase 11: render a real OpenGL triangle and validate CPU readback. */
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifndef P11_WINDOW_BRIDGE
#define P11_WINDOW_BRIDGE 0
#endif

#if P11_WINDOW_BRIDGE
#include <uapi/linux_abi.h>

#define P12_O_RDWR 2
#define P12_O_CLOEXEC 02000000
extern int open(const char *, int, ...);
extern int ioctl(int, unsigned long, ...);
#endif

#define P11_EGL_TRUE 1U
#define P11_EGL_NONE 0x3038
#define P11_EGL_PLATFORM_DEVICE_EXT 0x313F
#define P11_EGL_OPENGL_API 0x30A2
#define P11_EGL_SURFACE_TYPE 0x3033
#define P11_EGL_PBUFFER_BIT 0x0001
#define P11_EGL_RENDERABLE_TYPE 0x3040
#define P11_EGL_OPENGL_BIT 0x0008
#define P11_EGL_RED_SIZE 0x3024
#define P11_EGL_GREEN_SIZE 0x3023
#define P11_EGL_BLUE_SIZE 0x3022
#define P11_EGL_ALPHA_SIZE 0x3021
#define P11_EGL_WIDTH 0x3057
#define P11_EGL_HEIGHT 0x3056
#define P11_EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#define P11_EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define P11_EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#define P11_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#define P11_GL_VENDOR 0x1F00
#define P11_GL_RENDERER 0x1F01
#define P11_GL_VERSION 0x1F02

#define P11_GL_NO_ERROR 0U
#define P11_GL_COLOR_BUFFER_BIT 0x00004000U
#define P11_GL_TRIANGLES 0x0004U
#define P11_GL_FLOAT 0x1406U
#define P11_GL_RGBA 0x1908U
#define P11_GL_UNSIGNED_BYTE 0x1401U
#define P11_GL_ARRAY_BUFFER 0x8892U
#define P11_GL_STATIC_DRAW 0x88E4U
#define P11_GL_FRAMEBUFFER 0x8D40U
#define P11_GL_COLOR_ATTACHMENT0 0x8CE0U
#define P11_GL_TEXTURE_2D 0x0DE1U
#define P11_GL_TEXTURE_MIN_FILTER 0x2801U
#define P11_GL_TEXTURE_MAG_FILTER 0x2800U
#define P11_GL_NEAREST 0x2600U
#define P11_GL_RGBA8 0x8058U
#define P11_GL_FRAMEBUFFER_COMPLETE 0x8CD5U
#define P11_GL_VERTEX_SHADER 0x8B31U
#define P11_GL_FRAGMENT_SHADER 0x8B30U
#define P11_GL_COMPILE_STATUS 0x8B81U
#define P11_GL_LINK_STATUS 0x8B82U

/* The user explicitly reduced the long stress run to twenty frames. */
#ifndef P11_FRAME_COUNT
#define P11_FRAME_COUNT 20U
#endif
#ifndef P11_SURFACE_WIDTH
#define P11_SURFACE_WIDTH 64
#endif
#ifndef P11_SURFACE_HEIGHT
#define P11_SURFACE_HEIGHT 64
#endif
#ifndef P11_TEXTURE_WIDTH
#define P11_TEXTURE_WIDTH P11_SURFACE_WIDTH
#endif
#ifndef P11_TEXTURE_HEIGHT
#define P11_TEXTURE_HEIGHT P11_SURFACE_HEIGHT
#endif
#ifndef P11_RESIZED_WIDTH
#define P11_RESIZED_WIDTH P11_SURFACE_WIDTH
#endif
#ifndef P11_RESIZED_HEIGHT
#define P11_RESIZED_HEIGHT P11_SURFACE_HEIGHT
#endif
#if P11_WINDOW_BRIDGE
#ifndef P11_WINDOW_BUFFER_ADDRESS
#define P11_WINDOW_BUFFER_ADDRESS 0x07000000ULL
#endif
#endif

typedef unsigned int p11_egl_boolean_t;
typedef unsigned int p11_egl_enum_t;
typedef int p11_egl_int_t;
typedef void *p11_egl_display_t;
typedef void *p11_egl_device_t;
typedef void *p11_egl_config_t;
typedef void *p11_egl_context_t;
typedef void *p11_egl_surface_t;

#define P11_EGL_NO_DISPLAY ((p11_egl_display_t)0)
#define P11_EGL_NO_CONTEXT ((p11_egl_context_t)0)
#define P11_EGL_NO_SURFACE ((p11_egl_surface_t)0)

typedef p11_egl_boolean_t (*p11_egl_query_devices_fn)(
    p11_egl_int_t, p11_egl_device_t *, p11_egl_int_t *);
typedef p11_egl_display_t (*p11_egl_get_platform_display_fn)(
    p11_egl_enum_t, void *, const p11_egl_int_t *);
typedef p11_egl_display_t (*p11_egl_get_display_fn)(void *);
typedef p11_egl_boolean_t (*p11_egl_initialize_fn)(
    p11_egl_display_t, p11_egl_int_t *, p11_egl_int_t *);
typedef p11_egl_boolean_t (*p11_egl_bind_api_fn)(p11_egl_enum_t);
typedef p11_egl_boolean_t (*p11_egl_choose_config_fn)(
    p11_egl_display_t, const p11_egl_int_t *, p11_egl_config_t *,
    p11_egl_int_t, p11_egl_int_t *);
typedef p11_egl_context_t (*p11_egl_create_context_fn)(
    p11_egl_display_t, p11_egl_config_t, p11_egl_context_t,
    const p11_egl_int_t *);
typedef p11_egl_surface_t (*p11_egl_create_pbuffer_fn)(
    p11_egl_display_t, p11_egl_config_t, const p11_egl_int_t *);
typedef p11_egl_boolean_t (*p11_egl_make_current_fn)(
    p11_egl_display_t, p11_egl_surface_t, p11_egl_surface_t,
    p11_egl_context_t);
typedef p11_egl_boolean_t (*p11_egl_destroy_context_fn)(
    p11_egl_display_t, p11_egl_context_t);
typedef p11_egl_boolean_t (*p11_egl_destroy_surface_fn)(
    p11_egl_display_t, p11_egl_surface_t);
typedef p11_egl_boolean_t (*p11_egl_terminate_fn)(p11_egl_display_t);
typedef p11_egl_int_t (*p11_egl_get_error_fn)(void);
typedef void *(*p11_egl_get_proc_address_fn)(const char *);

typedef unsigned int p11_gl_enum_t;
typedef unsigned int p11_gl_uint_t;
typedef unsigned int p11_gl_bitfield_t;
typedef unsigned char p11_gl_boolean_t;
typedef int p11_gl_int_t;
typedef int p11_gl_sizei_t;
typedef int64_t p11_gl_sizeiptr_t;
typedef float p11_gl_float_t;
typedef char p11_gl_char_t;

typedef const unsigned char *(*p11_gl_get_string_fn)(p11_gl_enum_t);
typedef p11_gl_enum_t (*p11_gl_get_error_fn)(void);
typedef void (*p11_gl_viewport_fn)(p11_gl_int_t, p11_gl_int_t,
                                   p11_gl_sizei_t, p11_gl_sizei_t);
typedef void (*p11_gl_clear_color_fn)(p11_gl_float_t, p11_gl_float_t,
                                      p11_gl_float_t, p11_gl_float_t);
typedef void (*p11_gl_clear_fn)(p11_gl_bitfield_t);
typedef p11_gl_uint_t (*p11_gl_create_shader_fn)(p11_gl_enum_t);
typedef void (*p11_gl_shader_source_fn)(p11_gl_uint_t, p11_gl_sizei_t,
                                         const p11_gl_char_t *const *,
                                         const p11_gl_int_t *);
typedef void (*p11_gl_compile_shader_fn)(p11_gl_uint_t);
typedef void (*p11_gl_get_shader_iv_fn)(p11_gl_uint_t, p11_gl_enum_t,
                                        p11_gl_int_t *);
typedef void (*p11_gl_get_shader_info_log_fn)(p11_gl_uint_t, p11_gl_sizei_t,
                                              p11_gl_sizei_t *,
                                              p11_gl_char_t *);
typedef void (*p11_gl_delete_shader_fn)(p11_gl_uint_t);
typedef p11_gl_uint_t (*p11_gl_create_program_fn)(void);
typedef void (*p11_gl_attach_shader_fn)(p11_gl_uint_t, p11_gl_uint_t);
typedef void (*p11_gl_link_program_fn)(p11_gl_uint_t);
typedef void (*p11_gl_get_program_iv_fn)(p11_gl_uint_t, p11_gl_enum_t,
                                         p11_gl_int_t *);
typedef void (*p11_gl_get_program_info_log_fn)(p11_gl_uint_t, p11_gl_sizei_t,
                                               p11_gl_sizei_t *,
                                               p11_gl_char_t *);
typedef void (*p11_gl_delete_program_fn)(p11_gl_uint_t);
typedef void (*p11_gl_use_program_fn)(p11_gl_uint_t);
typedef void (*p11_gl_gen_vertex_arrays_fn)(p11_gl_sizei_t, p11_gl_uint_t *);
typedef void (*p11_gl_bind_vertex_array_fn)(p11_gl_uint_t);
typedef void (*p11_gl_delete_vertex_arrays_fn)(p11_gl_sizei_t,
                                               const p11_gl_uint_t *);
typedef void (*p11_gl_gen_buffers_fn)(p11_gl_sizei_t, p11_gl_uint_t *);
typedef void (*p11_gl_bind_buffer_fn)(p11_gl_enum_t, p11_gl_uint_t);
typedef void (*p11_gl_buffer_data_fn)(p11_gl_enum_t, p11_gl_sizeiptr_t,
                                      const void *, p11_gl_enum_t);
typedef void (*p11_gl_delete_buffers_fn)(p11_gl_sizei_t,
                                         const p11_gl_uint_t *);
typedef void (*p11_gl_enable_vertex_attrib_array_fn)(p11_gl_uint_t);
typedef void (*p11_gl_vertex_attrib_pointer_fn)(p11_gl_uint_t, p11_gl_int_t,
                                                p11_gl_enum_t,
                                                p11_gl_boolean_t,
                                                p11_gl_sizei_t, const void *);
typedef void (*p11_gl_draw_arrays_fn)(p11_gl_enum_t, p11_gl_int_t,
                                      p11_gl_sizei_t);
typedef void (*p11_gl_finish_fn)(void);
typedef void (*p11_gl_read_pixels_fn)(p11_gl_int_t, p11_gl_int_t,
                                      p11_gl_sizei_t, p11_gl_sizei_t,
                                      p11_gl_enum_t, p11_gl_enum_t, void *);
typedef void (*p11_gl_gen_framebuffers_fn)(p11_gl_sizei_t, p11_gl_uint_t *);
typedef void (*p11_gl_bind_framebuffer_fn)(p11_gl_enum_t, p11_gl_uint_t);
typedef void (*p11_gl_delete_framebuffers_fn)(p11_gl_sizei_t,
                                              const p11_gl_uint_t *);
typedef void (*p11_gl_gen_textures_fn)(p11_gl_sizei_t, p11_gl_uint_t *);
typedef void (*p11_gl_bind_texture_fn)(p11_gl_enum_t, p11_gl_uint_t);
typedef void (*p11_gl_delete_textures_fn)(p11_gl_sizei_t,
                                          const p11_gl_uint_t *);
typedef void (*p11_gl_tex_parameteri_fn)(p11_gl_enum_t, p11_gl_enum_t,
                                         p11_gl_int_t);
typedef void (*p11_gl_tex_image_2d_fn)(p11_gl_enum_t, p11_gl_int_t,
                                       p11_gl_int_t, p11_gl_sizei_t,
                                       p11_gl_sizei_t, p11_gl_int_t,
                                       p11_gl_enum_t, p11_gl_enum_t,
                                       const void *);
typedef void (*p11_gl_framebuffer_texture_2d_fn)(p11_gl_enum_t,
                                                 p11_gl_enum_t,
                                                 p11_gl_enum_t,
                                                 p11_gl_uint_t,
                                                 p11_gl_int_t);
typedef p11_gl_enum_t (*p11_gl_check_framebuffer_status_fn)(p11_gl_enum_t);

typedef struct p11_gl_api {
    p11_gl_get_string_fn get_string;
    p11_gl_get_error_fn get_error;
    p11_gl_viewport_fn viewport;
    p11_gl_clear_color_fn clear_color;
    p11_gl_clear_fn clear;
    p11_gl_create_shader_fn create_shader;
    p11_gl_shader_source_fn shader_source;
    p11_gl_compile_shader_fn compile_shader;
    p11_gl_get_shader_iv_fn get_shader_iv;
    p11_gl_get_shader_info_log_fn get_shader_info_log;
    p11_gl_delete_shader_fn delete_shader;
    p11_gl_create_program_fn create_program;
    p11_gl_attach_shader_fn attach_shader;
    p11_gl_link_program_fn link_program;
    p11_gl_get_program_iv_fn get_program_iv;
    p11_gl_get_program_info_log_fn get_program_info_log;
    p11_gl_delete_program_fn delete_program;
    p11_gl_use_program_fn use_program;
    p11_gl_gen_vertex_arrays_fn gen_vertex_arrays;
    p11_gl_bind_vertex_array_fn bind_vertex_array;
    p11_gl_delete_vertex_arrays_fn delete_vertex_arrays;
    p11_gl_gen_buffers_fn gen_buffers;
    p11_gl_bind_buffer_fn bind_buffer;
    p11_gl_buffer_data_fn buffer_data;
    p11_gl_delete_buffers_fn delete_buffers;
    p11_gl_enable_vertex_attrib_array_fn enable_vertex_attrib_array;
    p11_gl_vertex_attrib_pointer_fn vertex_attrib_pointer;
    p11_gl_draw_arrays_fn draw_arrays;
    p11_gl_finish_fn finish;
    p11_gl_read_pixels_fn read_pixels;
    p11_gl_gen_framebuffers_fn gen_framebuffers;
    p11_gl_bind_framebuffer_fn bind_framebuffer;
    p11_gl_delete_framebuffers_fn delete_framebuffers;
    p11_gl_gen_textures_fn gen_textures;
    p11_gl_bind_texture_fn bind_texture;
    p11_gl_delete_textures_fn delete_textures;
    p11_gl_tex_parameteri_fn tex_parameteri;
    p11_gl_tex_image_2d_fn tex_image_2d;
    p11_gl_framebuffer_texture_2d_fn framebuffer_texture_2d;
    p11_gl_check_framebuffer_status_fn check_framebuffer_status;
} p11_gl_api_t;

/* These are loaded through the guest's pinned glibc/GLVND path at runtime. */
#define P11_RTLD_NOW 2
#define P11_RTLD_GLOBAL 0x100

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern const char *dlerror(void);
extern int setenv(const char *, const char *, int);

static void p11_emit(const char *text)
{
    if (text == 0) return;
    size_t remaining = strlen(text);
    while (remaining != 0U) {
        ssize_t written = write(STDOUT_FILENO, text, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || (size_t)written > remaining) break;
        text += (size_t)written;
        remaining -= (size_t)written;
    }
}

static void p11_emit_value(const char *prefix, const char *value)
{
    p11_emit(prefix);
    if (value != 0) p11_emit(value);
    p11_emit("\n");
}

static void p11_emit_u32(const char *prefix, uint32_t value)
{
    char digits[12];
    size_t start = sizeof(digits) - 1U;
    digits[start] = '\0';
    digits[--start] = '\n';
    do {
        digits[--start] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    p11_emit(prefix);
    p11_emit(&digits[start]);
}

static int p11_contains_nvidia(const char *text)
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

#if !P11_WINDOW_BRIDGE
static int p11_pixel_is_red(const uint8_t pixel[4])
{
    return pixel[0] >= 180U && pixel[1] <= 80U && pixel[2] <= 80U &&
           pixel[3] >= 180U;
}

static int p11_pixel_is_clear(const uint8_t pixel[4])
{
    return pixel[0] <= 32U && pixel[1] <= 32U && pixel[2] <= 32U &&
           pixel[3] >= 180U;
}
#endif

static int p11_load_gl_api(void *handle,
                           p11_egl_get_proc_address_fn get_proc_address,
                           p11_gl_api_t *api)
{
#define P11_GL_LOAD(field, type, name)                                      \
    do {                                                                    \
        void *p11_symbol = handle != 0 ? dlsym(handle, name) : 0;           \
        if (p11_symbol == 0 && get_proc_address != 0)                       \
            p11_symbol = get_proc_address(name);                           \
        api->field = (type)p11_symbol;                                     \
        if (api->field == 0) return 0;                                      \
    } while (0)

    P11_GL_LOAD(get_string, p11_gl_get_string_fn, "glGetString");
    P11_GL_LOAD(get_error, p11_gl_get_error_fn, "glGetError");
    P11_GL_LOAD(viewport, p11_gl_viewport_fn, "glViewport");
    P11_GL_LOAD(clear_color, p11_gl_clear_color_fn, "glClearColor");
    P11_GL_LOAD(clear, p11_gl_clear_fn, "glClear");
    P11_GL_LOAD(create_shader, p11_gl_create_shader_fn, "glCreateShader");
    P11_GL_LOAD(shader_source, p11_gl_shader_source_fn, "glShaderSource");
    P11_GL_LOAD(compile_shader, p11_gl_compile_shader_fn, "glCompileShader");
    P11_GL_LOAD(get_shader_iv, p11_gl_get_shader_iv_fn, "glGetShaderiv");
    P11_GL_LOAD(get_shader_info_log, p11_gl_get_shader_info_log_fn,
                "glGetShaderInfoLog");
    P11_GL_LOAD(delete_shader, p11_gl_delete_shader_fn, "glDeleteShader");
    P11_GL_LOAD(create_program, p11_gl_create_program_fn, "glCreateProgram");
    P11_GL_LOAD(attach_shader, p11_gl_attach_shader_fn, "glAttachShader");
    P11_GL_LOAD(link_program, p11_gl_link_program_fn, "glLinkProgram");
    P11_GL_LOAD(get_program_iv, p11_gl_get_program_iv_fn, "glGetProgramiv");
    P11_GL_LOAD(get_program_info_log, p11_gl_get_program_info_log_fn,
                "glGetProgramInfoLog");
    P11_GL_LOAD(delete_program, p11_gl_delete_program_fn, "glDeleteProgram");
    P11_GL_LOAD(use_program, p11_gl_use_program_fn, "glUseProgram");
    P11_GL_LOAD(gen_vertex_arrays, p11_gl_gen_vertex_arrays_fn,
                "glGenVertexArrays");
    P11_GL_LOAD(bind_vertex_array, p11_gl_bind_vertex_array_fn,
                "glBindVertexArray");
    P11_GL_LOAD(delete_vertex_arrays, p11_gl_delete_vertex_arrays_fn,
                "glDeleteVertexArrays");
    P11_GL_LOAD(gen_buffers, p11_gl_gen_buffers_fn, "glGenBuffers");
    P11_GL_LOAD(bind_buffer, p11_gl_bind_buffer_fn, "glBindBuffer");
    P11_GL_LOAD(buffer_data, p11_gl_buffer_data_fn, "glBufferData");
    P11_GL_LOAD(delete_buffers, p11_gl_delete_buffers_fn, "glDeleteBuffers");
    P11_GL_LOAD(enable_vertex_attrib_array,
                p11_gl_enable_vertex_attrib_array_fn,
                "glEnableVertexAttribArray");
    P11_GL_LOAD(vertex_attrib_pointer, p11_gl_vertex_attrib_pointer_fn,
                "glVertexAttribPointer");
    P11_GL_LOAD(draw_arrays, p11_gl_draw_arrays_fn, "glDrawArrays");
    P11_GL_LOAD(finish, p11_gl_finish_fn, "glFinish");
    P11_GL_LOAD(read_pixels, p11_gl_read_pixels_fn, "glReadPixels");
    P11_GL_LOAD(gen_framebuffers, p11_gl_gen_framebuffers_fn,
                "glGenFramebuffers");
    P11_GL_LOAD(bind_framebuffer, p11_gl_bind_framebuffer_fn,
                "glBindFramebuffer");
    P11_GL_LOAD(delete_framebuffers, p11_gl_delete_framebuffers_fn,
                "glDeleteFramebuffers");
    P11_GL_LOAD(gen_textures, p11_gl_gen_textures_fn, "glGenTextures");
    P11_GL_LOAD(bind_texture, p11_gl_bind_texture_fn, "glBindTexture");
    P11_GL_LOAD(delete_textures, p11_gl_delete_textures_fn,
                "glDeleteTextures");
    P11_GL_LOAD(tex_parameteri, p11_gl_tex_parameteri_fn,
                "glTexParameteri");
    P11_GL_LOAD(tex_image_2d, p11_gl_tex_image_2d_fn, "glTexImage2D");
    P11_GL_LOAD(framebuffer_texture_2d,
                p11_gl_framebuffer_texture_2d_fn,
                "glFramebufferTexture2D");
    P11_GL_LOAD(check_framebuffer_status,
                p11_gl_check_framebuffer_status_fn,
                "glCheckFramebufferStatus");
#undef P11_GL_LOAD
    return 1;
}

static p11_gl_uint_t p11_compile_shader(const p11_gl_api_t *api,
                                        p11_gl_enum_t type,
                                        const char *source)
{
    p11_gl_uint_t shader = api->create_shader(type);
    const p11_gl_char_t *sources[] = {(const p11_gl_char_t *)source};
    p11_gl_int_t compiled = 0;
    if (shader == 0U) return 0U;
    api->shader_source(shader, 1, sources, 0);
    api->compile_shader(shader);
    api->get_shader_iv(shader, P11_GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        api->delete_shader(shader);
        return 0U;
    }
    return shader;
}

static p11_gl_uint_t p11_link_program(const p11_gl_api_t *api,
                                      p11_gl_uint_t vertex_shader,
                                      p11_gl_uint_t fragment_shader)
{
    p11_gl_uint_t program = api->create_program();
    p11_gl_int_t linked = 0;
    if (program == 0U) return 0U;
    api->attach_shader(program, vertex_shader);
    api->attach_shader(program, fragment_shader);
    api->link_program(program);
    api->get_program_iv(program, P11_GL_LINK_STATUS, &linked);
    if (linked == 0) {
        api->delete_program(program);
        return 0U;
    }
    return program;
}

#if P11_WINDOW_BRIDGE
static int p12_window_command(int descriptor, uint32_t operation,
                              int32_t x, int32_t y, uint32_t width,
                              uint32_t height) {
    uint64_t stride = (uint64_t)width * sizeof(uint32_t);
    uint64_t length = 0U;
    linux_window_command_t command = {
        .operation = operation,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    if (operation == LINUX_WINDOW_OPERATION_PRESENT) {
        if (width == 0U || height == 0U || stride > UINT32_MAX ||
            stride > UINT64_MAX / height) return 0;
        length = stride * height;
        command.stride = (uint32_t)stride;
        command.address = P11_WINDOW_BUFFER_ADDRESS;
        command.length = length;
    }
    return ioctl(descriptor, (unsigned long)LINUX_WINDOW_IOCTL_COMMIT,
                 &command) == 0;
}

static int p12_pixel_is_red_bgra(const uint8_t pixel[4]) {
    return pixel[0] <= 80U && pixel[1] <= 80U && pixel[2] >= 180U &&
           pixel[3] >= 180U;
}

static int p12_pixel_is_clear_bgra(const uint8_t pixel[4]) {
    return pixel[0] <= 32U && pixel[1] <= 32U && pixel[2] <= 32U &&
           pixel[3] >= 180U;
}

static int p12_readback_present(const p11_gl_api_t *api, int descriptor,
                                uint32_t width, uint32_t height) {
    uint64_t pixel_count = (uint64_t)width * height;
    uint64_t byte_count = pixel_count * 4U;
    uint8_t *pixels = (uint8_t *)(uintptr_t)P11_WINDOW_BUFFER_ADDRESS;
    if (api == 0 || descriptor < 0 || width == 0U || height == 0U ||
        byte_count > SIZE_MAX ||
        (uint64_t)width > UINT32_MAX / sizeof(uint32_t)) return 0;
    api->read_pixels(0, 0, (p11_gl_sizei_t)width, (p11_gl_sizei_t)height,
                     P11_GL_RGBA, P11_GL_UNSIGNED_BYTE, pixels);
    if (api->get_error() != P11_GL_NO_ERROR) return 0;
    for (uint64_t index = 0U; index < pixel_count; ++index) {
        uint8_t red = pixels[index * 4U];
        pixels[index * 4U] = pixels[index * 4U + 2U];
        pixels[index * 4U + 2U] = red;
    }
    for (uint32_t row = 0U; row < height / 2U; ++row) {
        uint8_t *top = pixels + (uint64_t)row * width * 4U;
        uint8_t *bottom = pixels +
                          (uint64_t)(height - 1U - row) * width * 4U;
        for (uint32_t column = 0U; column < width * 4U; ++column) {
            uint8_t value = top[column];
            top[column] = bottom[column];
            bottom[column] = value;
        }
    }
    if (!p12_pixel_is_red_bgra(
            pixels + ((uint64_t)height / 2U * width + width / 2U) * 4U) ||
        !p12_pixel_is_clear_bgra(pixels)) return 0;
    return p12_window_command(
               descriptor, LINUX_WINDOW_OPERATION_PRESENT, 0, 0,
               width, height) != 0;
}
#endif

int main(void)
{
    static const char vertex_source[] =
        "#version 330 core\n"
        "layout(location = 0) in vec2 p11_position;\n"
        "void main() { gl_Position = vec4(p11_position, 0.0, 1.0); }\n";
    static const char fragment_source[] =
        "#version 330 core\n"
        "out vec4 p11_color;\n"
        "void main() { p11_color = vec4(1.0, 0.0, 0.0, 1.0); }\n";
    static const float vertices[] = {
        -0.80F, -0.80F,
         0.80F, -0.80F,
         0.00F,  0.80F,
    };
    const char *failure_marker = "OS_P11_FAILURE_STAGE_UNKNOWN\n";
    void *egl_handle = 0;
    void *gl_handle = 0;
    p11_egl_display_t display = P11_EGL_NO_DISPLAY;
    p11_egl_context_t context = P11_EGL_NO_CONTEXT;
    p11_egl_surface_t surface = P11_EGL_NO_SURFACE;
    p11_egl_device_t devices[8] = {0};
    p11_egl_config_t config = 0;
    p11_egl_int_t device_count = 0;
    p11_egl_int_t config_count = 0;
    p11_egl_int_t major = 0;
    p11_egl_int_t minor = 0;
    p11_egl_query_devices_fn query_devices = 0;
    p11_egl_get_platform_display_fn get_platform_display = 0;
    p11_egl_get_display_fn get_display = 0;
    p11_egl_initialize_fn initialize = 0;
    p11_egl_bind_api_fn bind_api = 0;
    p11_egl_choose_config_fn choose_config = 0;
    p11_egl_create_context_fn create_context = 0;
    p11_egl_create_pbuffer_fn create_pbuffer = 0;
    p11_egl_make_current_fn make_current = 0;
    p11_egl_destroy_context_fn destroy_context = 0;
    p11_egl_destroy_surface_fn destroy_surface = 0;
    p11_egl_terminate_fn terminate_display = 0;
    p11_egl_get_error_fn get_egl_error = 0;
    p11_egl_get_proc_address_fn get_proc_address = 0;
    p11_gl_api_t gl = {0};
    p11_gl_uint_t vertex_shader = 0U;
    p11_gl_uint_t fragment_shader = 0U;
    p11_gl_uint_t program = 0U;
    p11_gl_uint_t vertex_array = 0U;
    p11_gl_uint_t vertex_buffer = 0U;
    p11_gl_uint_t framebuffer = 0U;
    p11_gl_uint_t color_texture = 0U;
#if P11_WINDOW_BRIDGE
    int window_descriptor = -1;
    uint32_t render_width = P11_SURFACE_WIDTH;
    uint32_t render_height = P11_SURFACE_HEIGHT;
#endif
    int result = 1;

    p11_emit("OS_P11_EGL_PROBE_BEGIN\n");
#if P11_WINDOW_BRIDGE
    p11_emit("OS_P12_EGL_PROBE_BEGIN\n");
#endif
    if (setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               P11_APP_ROOT "/egl/10_nvidia.json", 1) != 0) {
        failure_marker = "OS_P11_GLVND_ENV_FAIL\n";
        goto cleanup;
    }
    egl_handle = dlopen(P11_APP_ROOT "/lib/libEGL.so.1",
                        P11_RTLD_NOW | P11_RTLD_GLOBAL);
    if (egl_handle == 0) {
        failure_marker = "OS_P11_LIBEGL_FAIL\n";
        goto cleanup;
    }
    get_proc_address = (p11_egl_get_proc_address_fn)
        dlsym(egl_handle, "eglGetProcAddress");
    if (get_proc_address == 0) {
        failure_marker = "OS_P11_EGL_GETPROC_FAIL\n";
        goto cleanup;
    }
    get_display = (p11_egl_get_display_fn)dlsym(egl_handle, "eglGetDisplay");
    if (get_display == 0)
        get_display = (p11_egl_get_display_fn)get_proc_address("eglGetDisplay");
    query_devices = (p11_egl_query_devices_fn)
        get_proc_address("eglQueryDevicesEXT");
    get_platform_display = (p11_egl_get_platform_display_fn)
        get_proc_address("eglGetPlatformDisplayEXT");
    initialize = (p11_egl_initialize_fn)dlsym(egl_handle, "eglInitialize");
    bind_api = (p11_egl_bind_api_fn)dlsym(egl_handle, "eglBindAPI");
    choose_config = (p11_egl_choose_config_fn)
        dlsym(egl_handle, "eglChooseConfig");
    create_context = (p11_egl_create_context_fn)
        dlsym(egl_handle, "eglCreateContext");
    create_pbuffer = (p11_egl_create_pbuffer_fn)
        dlsym(egl_handle, "eglCreatePbufferSurface");
    make_current = (p11_egl_make_current_fn)dlsym(egl_handle, "eglMakeCurrent");
    destroy_context = (p11_egl_destroy_context_fn)
        dlsym(egl_handle, "eglDestroyContext");
    destroy_surface = (p11_egl_destroy_surface_fn)
        dlsym(egl_handle, "eglDestroySurface");
    terminate_display = (p11_egl_terminate_fn)dlsym(egl_handle, "eglTerminate");
    get_egl_error = (p11_egl_get_error_fn)dlsym(egl_handle, "eglGetError");
    if (get_display == 0 || query_devices == 0 || initialize == 0 ||
        bind_api == 0 || choose_config == 0 || create_context == 0 ||
        create_pbuffer == 0 || make_current == 0 || destroy_context == 0 ||
        destroy_surface == 0 || terminate_display == 0 || get_egl_error == 0) {
        failure_marker = "OS_P11_EGL_SYMBOL_FAIL\n";
        goto cleanup;
    }
    if (query_devices(8, devices, &device_count) != P11_EGL_TRUE ||
        device_count <= 0 || devices[0] == 0) {
        failure_marker = "OS_P11_NVIDIA_DEVICE_FAIL\n";
        goto cleanup;
    }
    p11_emit("OS_P11_NVIDIA_DEVICE_PASS\n");
#if P11_WINDOW_BRIDGE
    p11_emit("OS_P12_NVIDIA_DEVICE_PASS\n");
#endif
    if (get_platform_display != 0)
        display = get_platform_display(P11_EGL_PLATFORM_DEVICE_EXT,
                                        devices[0], 0);
    if (display == P11_EGL_NO_DISPLAY) display = get_display(0);
    if (display == P11_EGL_NO_DISPLAY) {
        failure_marker = "OS_P11_DISPLAY_FAIL\n";
        goto cleanup;
    }
    if (bind_api(P11_EGL_OPENGL_API) != P11_EGL_TRUE) {
        failure_marker = "OS_P11_BIND_API_FAIL\n";
        goto cleanup;
    }
    if (initialize(display, &major, &minor) != P11_EGL_TRUE) {
        failure_marker = "OS_P11_EGL_INITIALIZE_FAIL\n";
        goto cleanup;
    }
    p11_emit("OS_P11_EGL_INITIALIZED\n");
#if P11_WINDOW_BRIDGE
    p11_emit("OS_P12_EGL_INITIALIZED\n");
#endif
    {
        const p11_egl_int_t attributes[] = {
            P11_EGL_SURFACE_TYPE, P11_EGL_PBUFFER_BIT,
            P11_EGL_RENDERABLE_TYPE, P11_EGL_OPENGL_BIT,
            P11_EGL_RED_SIZE, 8, P11_EGL_GREEN_SIZE, 8,
            P11_EGL_BLUE_SIZE, 8, P11_EGL_ALPHA_SIZE, 8,
            P11_EGL_NONE
        };
        if (choose_config(display, attributes, &config, 1, &config_count) !=
                P11_EGL_TRUE || config_count <= 0 || config == 0) {
            failure_marker = "OS_P11_CONFIG_FAIL\n";
            goto cleanup;
        }
    }
    context = create_context(
        display, config, P11_EGL_NO_CONTEXT,
        (const p11_egl_int_t[]){
            P11_EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            P11_EGL_CONTEXT_MINOR_VERSION_KHR, 3,
            P11_EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            P11_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            P11_EGL_NONE});
    if (context == P11_EGL_NO_CONTEXT) {
        failure_marker = "OS_P11_CREATE_CONTEXT_FAIL\n";
        goto cleanup;
    }
    surface = create_pbuffer(display, config,
                             (const p11_egl_int_t[]){
                                 P11_EGL_WIDTH, P11_TEXTURE_WIDTH,
                                 P11_EGL_HEIGHT, P11_TEXTURE_HEIGHT,
                                 P11_EGL_NONE});
    if (surface == P11_EGL_NO_SURFACE) {
        p11_emit_u32("OS_P11_EGL_PBUFFER_CREATE_ERROR=",
                     (uint32_t)get_egl_error());
        failure_marker = "OS_P11_PBUFFER_CREATE_FAIL\n";
        goto cleanup;
    }
    if (make_current(display, surface, surface, context) != P11_EGL_TRUE) {
        p11_emit_u32("OS_P11_EGL_PBUFFER_CURRENT_ERROR=",
                     (uint32_t)get_egl_error());
        failure_marker = "OS_P11_PBUFFER_CURRENT_FAIL\n";
        goto cleanup;
    }
    p11_emit("OS_P11_PBUFFER_CURRENT\n");
    p11_emit("OS_P11_CONTEXT_CURRENT\n");
#if P11_WINDOW_BRIDGE
    p11_emit("OS_P12_PBUFFER_CURRENT\n");
    p11_emit("OS_P12_CONTEXT_CURRENT\n");
#endif

    gl_handle = dlopen("libOpenGL.so.0", P11_RTLD_NOW | P11_RTLD_GLOBAL);
    if (!p11_load_gl_api(gl_handle, get_proc_address, &gl)) {
        failure_marker = "OS_P11_GL_SYMBOL_FAIL\n";
        goto cleanup;
    }
    {
        const unsigned char *vendor = gl.get_string(P11_GL_VENDOR);
        const unsigned char *renderer = gl.get_string(P11_GL_RENDERER);
        const unsigned char *version = gl.get_string(P11_GL_VERSION);
        if (vendor == 0 || renderer == 0 ||
            !p11_contains_nvidia((const char *)vendor)) {
            failure_marker = "OS_P11_GL_VENDOR_FAIL\n";
            goto cleanup;
        }
        p11_emit_value("OS_P11_GL_VENDOR=", (const char *)vendor);
        p11_emit_value("OS_P11_GL_RENDERER=", (const char *)renderer);
        p11_emit_value("OS_P11_GL_VERSION=", (const char *)version);
#if P11_WINDOW_BRIDGE
        p11_emit_value("OS_P12_GL_VENDOR=", (const char *)vendor);
        p11_emit_value("OS_P12_GL_RENDERER=", (const char *)renderer);
        p11_emit_value("OS_P12_GL_VERSION=", (const char *)version);
#endif
    }
    (void)gl.get_error();
    gl.gen_framebuffers(1, &framebuffer);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_CREATE_FAIL\n";
        goto cleanup;
    }
    gl.gen_textures(1, &color_texture);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_CREATE_FAIL\n";
        goto cleanup;
    }
    if (framebuffer == 0U || color_texture == 0U) {
        failure_marker = "OS_P11_FBO_CREATE_FAIL\n";
        goto cleanup;
    }
    gl.bind_framebuffer(P11_GL_FRAMEBUFFER, framebuffer);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_SETUP_FAIL\n";
        goto cleanup;
    }
    gl.bind_texture(P11_GL_TEXTURE_2D, color_texture);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_SETUP_FAIL\n";
        goto cleanup;
    }
    gl.tex_parameteri(P11_GL_TEXTURE_2D, P11_GL_TEXTURE_MIN_FILTER,
                      (p11_gl_int_t)P11_GL_NEAREST);
    gl.tex_parameteri(P11_GL_TEXTURE_2D, P11_GL_TEXTURE_MAG_FILTER,
                      (p11_gl_int_t)P11_GL_NEAREST);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_SETUP_FAIL\n";
        goto cleanup;
    }
    gl.tex_image_2d(P11_GL_TEXTURE_2D, 0, (p11_gl_int_t)P11_GL_RGBA8,
                    P11_TEXTURE_WIDTH, P11_TEXTURE_HEIGHT, 0,
                    P11_GL_RGBA, P11_GL_UNSIGNED_BYTE, (const void *)0);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_SETUP_FAIL\n";
        goto cleanup;
    }
    gl.framebuffer_texture_2d(P11_GL_FRAMEBUFFER,
                              P11_GL_COLOR_ATTACHMENT0,
                              P11_GL_TEXTURE_2D, color_texture, 0);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_FBO_SETUP_FAIL\n";
        goto cleanup;
    }
    {
        p11_gl_enum_t framebuffer_status =
            gl.check_framebuffer_status(P11_GL_FRAMEBUFFER);
        p11_gl_enum_t framebuffer_error = gl.get_error();
        if (framebuffer_status != P11_GL_FRAMEBUFFER_COMPLETE ||
            framebuffer_error != P11_GL_NO_ERROR) {
            failure_marker = "OS_P11_FBO_STATUS_FAIL\n";
            goto cleanup;
        }
    }
    p11_emit("OS_P11_FBO_COMPLETE\n");
    vertex_shader = p11_compile_shader(&gl, P11_GL_VERTEX_SHADER,
                                       vertex_source);
    fragment_shader = p11_compile_shader(&gl, P11_GL_FRAGMENT_SHADER,
                                         fragment_source);
    if (vertex_shader == 0U || fragment_shader == 0U) {
        failure_marker = "OS_P11_SHADER_COMPILE_FAIL\n";
        goto cleanup;
    }
    program = p11_link_program(&gl, vertex_shader, fragment_shader);
    if (program == 0U) {
        failure_marker = "OS_P11_PROGRAM_LINK_FAIL\n";
        goto cleanup;
    }
    gl.gen_vertex_arrays(1, &vertex_array);
    gl.gen_buffers(1, &vertex_buffer);
    if (vertex_array == 0U || vertex_buffer == 0U) {
        failure_marker = "OS_P11_BUFFER_CREATE_FAIL\n";
        goto cleanup;
    }
    gl.bind_vertex_array(vertex_array);
    gl.bind_buffer(P11_GL_ARRAY_BUFFER, vertex_buffer);
    gl.buffer_data(P11_GL_ARRAY_BUFFER, (p11_gl_sizeiptr_t)sizeof(vertices),
                   vertices, P11_GL_STATIC_DRAW);
    gl.enable_vertex_attrib_array(0U);
    gl.vertex_attrib_pointer(0U, 2, P11_GL_FLOAT, 0U,
                             (p11_gl_sizei_t)(2U * sizeof(float)), 0);
    gl.viewport(0, 0, P11_SURFACE_WIDTH, P11_SURFACE_HEIGHT);
    gl.use_program(program);
    if (gl.get_error() != P11_GL_NO_ERROR) {
        failure_marker = "OS_P11_GL_SETUP_FAIL\n";
        goto cleanup;
    }
#if P11_WINDOW_BRIDGE
    window_descriptor = open(LINUX_WINDOW_DEVICE_PATH,
                             P12_O_RDWR | P12_O_CLOEXEC);
    if (window_descriptor < 0) {
        failure_marker = "OS_P12_WINDOW_OPEN_FAIL\n";
        goto cleanup;
    }
#endif
#if P11_WINDOW_BRIDGE
    for (uint32_t frame = 0U; frame < P11_FRAME_COUNT; ++frame) {
        gl.clear_color(0.0F, 0.0F, 0.0F, 1.0F);
        gl.clear(P11_GL_COLOR_BUFFER_BIT);
        gl.draw_arrays(P11_GL_TRIANGLES, 0, 3);
        gl.finish();
        if (!p12_readback_present(&gl, window_descriptor, render_width,
                                  render_height)) {
            failure_marker = "OS_P12_READBACK_HBUFFER_FAIL\n";
            goto cleanup;
        }
        if (frame == 0U) {
            p11_emit("OS_P12_READBACK_HBUFFER_PASS\n");
            p11_emit("OS_P12_VISIBLE_PASS\n");
            if (!p12_window_command(window_descriptor,
                                    LINUX_WINDOW_OPERATION_MOVE, 64, 48,
                                    0U, 0U)) {
                failure_marker = "OS_P12_MOVE_FAIL\n";
                goto cleanup;
            }
            p11_emit("OS_P12_MOVE_PASS\n");
            if (!p12_window_command(window_descriptor,
                                    LINUX_WINDOW_OPERATION_RESIZE, 0, 0,
                                    P11_RESIZED_WIDTH, P11_RESIZED_HEIGHT)) {
                failure_marker = "OS_P12_RESIZE_FAIL\n";
                goto cleanup;
            }
            render_width = P11_RESIZED_WIDTH;
            render_height = P11_RESIZED_HEIGHT;
            gl.viewport(0, 0, (p11_gl_sizei_t)render_width,
                        (p11_gl_sizei_t)render_height);
            if (gl.get_error() != P11_GL_NO_ERROR) {
                failure_marker = "OS_P12_RESIZE_GL_FAIL\n";
                goto cleanup;
            }
            p11_emit("OS_P12_RESIZE_PASS\n");
        }
    }
    if (!p12_window_command(window_descriptor,
                            LINUX_WINDOW_OPERATION_CLOSE, 0, 0, 0U, 0U)) {
        failure_marker = "OS_P12_CLOSE_FAIL\n";
        goto cleanup;
    }
    p11_emit("OS_P12_CLOSE_PASS\n");
    if (close(window_descriptor) != 0) {
        window_descriptor = -1;
        failure_marker = "OS_P12_FD_CLOSE_FAIL\n";
        goto cleanup;
    }
    window_descriptor = -1;
    p11_emit_u32("OS_P12_RENDER_PASS frames=", P11_FRAME_COUNT);
#else
    for (uint32_t frame = 0U; frame < P11_FRAME_COUNT; ++frame) {
        uint8_t center[4] = {0};
        uint8_t outside[4] = {0};
        gl.clear_color(0.0F, 0.0F, 0.0F, 1.0F);
        gl.clear(P11_GL_COLOR_BUFFER_BIT);
        gl.draw_arrays(P11_GL_TRIANGLES, 0, 3);
        gl.finish();
        gl.read_pixels(P11_SURFACE_WIDTH / 2, P11_SURFACE_HEIGHT / 2, 1, 1,
                       P11_GL_RGBA, P11_GL_UNSIGNED_BYTE, center);
        gl.read_pixels(0, 0, 1, 1, P11_GL_RGBA, P11_GL_UNSIGNED_BYTE, outside);
        if (gl.get_error() != P11_GL_NO_ERROR ||
            !p11_pixel_is_red(center) || !p11_pixel_is_clear(outside)) {
            failure_marker = "OS_P11_TRIANGLE_PIXEL_FAIL\n";
            goto cleanup;
        }
        if (frame == 0U)
            p11_emit("OS_P11_TRIANGLE_PIXEL_PASS\n");
    }
    p11_emit_u32("OS_P11_STRESS_PASS frames=", P11_FRAME_COUNT);
#endif
    result = 0;

cleanup:
#if P11_WINDOW_BRIDGE
    if (window_descriptor >= 0) (void)close(window_descriptor);
#endif
    if (program != 0U) gl.delete_program(program);
    if (vertex_shader != 0U) gl.delete_shader(vertex_shader);
    if (fragment_shader != 0U) gl.delete_shader(fragment_shader);
    if (vertex_buffer != 0U) gl.delete_buffers(1, &vertex_buffer);
    if (vertex_array != 0U) gl.delete_vertex_arrays(1, &vertex_array);
    if (color_texture != 0U) gl.delete_textures(1, &color_texture);
    if (framebuffer != 0U) gl.delete_framebuffers(1, &framebuffer);
    if (surface != P11_EGL_NO_SURFACE && destroy_surface != 0)
        (void)destroy_surface(display, surface);
    if (context != P11_EGL_NO_CONTEXT && destroy_context != 0)
        (void)destroy_context(display, context);
    if (display != P11_EGL_NO_DISPLAY && terminate_display != 0)
        (void)terminate_display(display);
    if (gl_handle != 0 && dlclose(gl_handle) != 0) result = 1;
    if (egl_handle != 0 && dlclose(egl_handle) != 0) result = 1;
    if (result != 0) {
        p11_emit(failure_marker);
        p11_emit("OS_P11_FAIL\n");
        return 1;
    }
#if P11_WINDOW_BRIDGE
    p11_emit("OS_P12_PASS\n");
#endif
    p11_emit("OS_P11_PASS\n");
    return 0;
}
