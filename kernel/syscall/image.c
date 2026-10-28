/* REFACTOR_SYSCALL_IMAGE_OWNER：有界用户 PNG 验证和解码。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/kmem.h>
#include <uapi/image.h>

#include "../graphics/png.h"
#include "../graphics/png_internal.h"
#include "internal.h"

static bool image_encoded_range_valid(uint64_t encoded,
                                      uint64_t encoded_size) {
    if (encoded == 0U || encoded_size == 0U ||
        encoded_size > OS_IMAGE_MAX_ENCODED_BYTES ||
        encoded_size > SIZE_MAX) {
        return false;
    }
    return x86_user_range_valid((const void __user *)(uintptr_t)encoded,
                                (size_t)encoded_size);
}

static kstatus_t image_copy_encoded(uint64_t encoded, uint64_t encoded_size,
                                    uint8_t **copy) {
    uint8_t *buffer;
    kstatus_t status;

    if (copy == 0 || !image_encoded_range_valid(encoded, encoded_size)) {
        return K_EINVAL;
    }
    buffer = (uint8_t *)kmalloc((size_t)encoded_size, 0U);
    if (buffer == 0) return K_ENOMEM;
    status = copy_from_user(buffer,
                            (const void __user *)(uintptr_t)encoded,
                            (size_t)encoded_size);
    if (status != K_OK) {
        kfree(buffer);
        return status;
    }
    *copy = buffer;
    return K_OK;
}

static bool image_geometry_valid(const png_image_info_t *info,
                                 uint64_t *pixel_bytes) {
    uint64_t row_bytes;
    uint64_t scanline_bytes;
    uint64_t output_bytes;

    if (info == 0 || pixel_bytes == 0 || info->width == 0U ||
        info->height == 0U || info->channels == 0U ||
        (uint64_t)info->width > UINT64_MAX / info->channels) {
        return false;
    }
    row_bytes = (uint64_t)info->width * info->channels;
    if (row_bytes > UINT64_MAX - 1U ||
        (row_bytes + 1U) > UINT64_MAX / info->height) {
        return false;
    }
    scanline_bytes = (row_bytes + 1U) * info->height;
    if (scanline_bytes > OS_IMAGE_MAX_PIXEL_BYTES ||
        (uint64_t)info->width > UINT64_MAX / info->height) {
        return false;
    }
    output_bytes = (uint64_t)info->width * info->height;
    if (output_bytes > UINT64_MAX / 4U) return false;
    output_bytes *= 4U;
    if (output_bytes == 0U || output_bytes > OS_IMAGE_MAX_PIXEL_BYTES ||
        output_bytes > SIZE_MAX ||
        (uint64_t)info->width > UINT32_MAX / 4U) {
        return false;
    }
    *pixel_bytes = output_bytes;
    return true;
}

static kstatus_t image_query(const uint8_t *encoded, size_t encoded_size,
                             png_image_info_t *info, uint64_t *pixel_bytes) {
    if (encoded == 0 || info == 0 || pixel_bytes == 0 ||
        !png_parse_chunks(encoded, encoded_size, info, 0) ||
        !image_geometry_valid(info, pixel_bytes)) {
        return K_EINVAL;
    }
    return K_OK;
}

int64_t syscall_image_info(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    os_image_info_t arguments;
    png_image_info_t info = {0};
    uint8_t *encoded = 0;
    uint64_t pixel_bytes;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) {
        return K_EINVAL;
    }
    status = image_copy_encoded(arguments.encoded, arguments.encoded_size,
                                &encoded);
    if (status != K_OK) return status;
    status = image_query(encoded, (size_t)arguments.encoded_size,
                         &info, &pixel_bytes);
    kfree(encoded);
    if (status != K_OK) return status;

    arguments.width = info.width;
    arguments.height = info.height;
    arguments.stride = info.width * 4U;
    arguments.format = OS_IMAGE_PIXEL_RGBA8888;
    arguments.pixel_bytes = pixel_bytes;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_image_decode(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    os_image_decode_t arguments;
    png_image_info_t info = {0};
    desktop_png_image_t image = {0};
    uint8_t *encoded = 0;
    uint64_t pixel_bytes;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (arguments_pointer == 0U) return K_EINVAL;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.pixels == 0U || arguments.capacity == 0U ||
        arguments.capacity > OS_IMAGE_MAX_PIXEL_BYTES ||
        arguments.capacity > SIZE_MAX) {
        return K_EINVAL;
    }
    status = image_copy_encoded(arguments.encoded, arguments.encoded_size,
                                &encoded);
    if (status != K_OK) return status;
    status = image_query(encoded, (size_t)arguments.encoded_size,
                         &info, &pixel_bytes);
    if (status != K_OK) goto done;
    if (arguments.capacity < pixel_bytes ||
        !x86_user_range_valid((void __user *)(uintptr_t)arguments.pixels,
                              (size_t)pixel_bytes)) {
        status = arguments.capacity < pixel_bytes ? K_EOVERFLOW : K_EINVAL;
        goto done;
    }
    if (!desktop_png_decode(encoded, (size_t)arguments.encoded_size, &image) ||
        image.pixels == 0 || image.storage == 0 || image.width != info.width ||
        image.height != info.height || image.stride != info.width * 4U) {
        status = K_EINVAL;
        goto done;
    }
    status = copy_to_user((void __user *)(uintptr_t)arguments.pixels,
                          image.pixels, (size_t)pixel_bytes);
    if (status != K_OK) goto done;
    arguments.width = image.width;
    arguments.height = image.height;
    arguments.stride = image.stride;
    arguments.format = OS_IMAGE_PIXEL_RGBA8888;
    arguments.bytes_written = pixel_bytes;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));

done:
    if (image.storage != 0) kfree(image.storage);
    if (encoded != 0) kfree(encoded);
    return status;
}
