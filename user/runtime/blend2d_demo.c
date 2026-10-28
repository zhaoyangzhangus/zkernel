#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include <blend2d/blend2d.h>

#include "blend2d_demo.h"

static bool result_ok(BLResult result) {
    return result == BL_SUCCESS;
}

bool blend2d_draw_demo(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height,
                              uint32_t font_size) {
    BLImageCore image = {0};
    BLContextCore context = {0};
    BLPathCore path = {0};
    BLGradientCore gradient = {0};
    BLFontDataCore font_data = {0};
    BLFontFaceCore font_face = {0};
    BLFontCore font = {0};
    BLGlyphBufferCore glyph_buffer = {0};
    BLImageCodecCore codec = {0};
    BLImageEncoderCore encoder = {0};
    BLImageDecoderCore decoder = {0};
    BLArrayCore encoded = {0};
    BLImageCore decoded = {0};
    BLImageCore scaled = {0};
    BLImageData scaled_data = {0};
    BLImageInfo image_info = {0};
    BLSizeI scaled_size = {64, 48};
    BLLinearGradientValues gradient_values = {80, 48, 80, 228};
    BLRectI gradient_rect;
    BLRect path_rect;
    BLRectI stroke_rect;
    BLPoint origin = {0, 0};
    BLPointI text_origin;
    BLMatrix2D matrix;
    BLPoint matrix_input = {2, 3};
    BLPoint matrix_output = {0, 0};
    const uint8_t *encoded_data = 0;
    size_t encoded_size = 0U;
    const char text[] = "Blend2D C API";
    bool runtime_ready = false;
    bool image_ready = false;
    bool context_ready = false;
    bool context_active = false;
    bool path_ready = false;
    bool gradient_ready = false;
    bool font_data_ready = false;
    bool font_face_ready = false;
    bool font_ready = false;
    bool glyph_buffer_ready = false;
    bool codec_ready = false;
    bool encoder_ready = false;
    bool decoder_ready = false;
    bool encoded_ready = false;
    bool decoded_ready = false;
    bool scaled_ready = false;
    bool success = false;
    BLResult result;

    if (pixels == 0 || width == 0U || height == 0U || stride < width ||
        width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        stride > (uint32_t)(INTPTR_MAX / sizeof(uint32_t)) ||
        font_size < OS_BLEND2D_FONT_MIN ||
        font_size > OS_BLEND2D_FONT_MAX) return false;

    result = bl_runtime_init();
    if (!result_ok(result)) goto cleanup;
    runtime_ready = true;
    result = bl_image_init_as_from_data(
        &image, (int)width, (int)height, BL_FORMAT_XRGB32, pixels,
        (intptr_t)((uint64_t)stride * sizeof(uint32_t)), BL_DATA_ACCESS_RW,
        0, 0);
    if (!result_ok(result)) goto cleanup;
    image_ready = true;
    result = bl_context_init_as(&context, &image, 0);
    if (!result_ok(result)) goto cleanup;
    context_ready = true;
    context_active = true;
    if (!result_ok(bl_context_set_comp_op(&context, BL_COMP_OP_SRC_OVER))) {
        goto cleanup;
    }

    gradient_rect = (BLRectI){80, 48, (int)width - 160, 180};
    result = bl_gradient_init_as(
        &gradient, BL_GRADIENT_TYPE_LINEAR, &gradient_values,
        BL_EXTEND_MODE_PAD, 0, 0U, 0);
    if (!result_ok(result)) goto cleanup;
    gradient_ready = true;
    if (!result_ok(bl_gradient_add_stop_rgba32(
            &gradient, 0.0, 0xFF2A6A92U)) ||
        !result_ok(bl_gradient_add_stop_rgba32(
            &gradient, 1.0, 0xFF163B68U)) ||
        !result_ok(bl_context_fill_rect_i_ext(
            &context, &gradient_rect, (const BLUnknown *)&gradient))) {
        goto cleanup;
    }

    path_rect = (BLRect){96, 66, (double)width - 192.0, 144.0};
    result = bl_path_init(&path);
    if (!result_ok(result)) goto cleanup;
    path_ready = true;
    if (!result_ok(bl_path_add_rect_d(
            &path, &path_rect, BL_GEOMETRY_DIRECTION_CW)) ||
        !result_ok(bl_context_fill_path_d_rgba32(
            &context, &origin, &path, 0xA02A6A92U)) ||
        !result_ok(bl_context_set_stroke_width(&context, 3.0)) ||
        !result_ok(bl_context_stroke_path_d_rgba32(
            &context, &origin, &path, 0xFF7DD6FFU))) {
        goto cleanup;
    }

    if (!result_ok(bl_matrix2d_set_translation(&matrix, 5.0, 7.0)) ||
        !result_ok(bl_matrix2d_map_pointd_array(
            &matrix, &matrix_output, &matrix_input, 1U)) ||
        matrix_output.x != 7.0 || matrix_output.y != 10.0) goto cleanup;
    {
        double translation[2] = {24.0, 0.0};
        stroke_rect = (BLRectI){96, 104, (int)width - 192, 64};
        if (!result_ok(bl_context_apply_transform_op(
                &context, BL_TRANSFORM_OP_TRANSLATE, translation)) ||
            !result_ok(bl_context_stroke_rect_i_rgba32(
                &context, &stroke_rect, 0xFFE5B567U))) goto cleanup;
    }

    result = bl_font_data_init(&font_data);
    if (!result_ok(result)) goto cleanup;
    font_data_ready = true;
    if (!result_ok(bl_font_data_create_from_file(
            &font_data, "/EFI/OS/fonts/default.ttf", BL_FILE_READ_NO_FLAGS))) {
        goto cleanup;
    }
    result = bl_font_face_init(&font_face);
    if (!result_ok(result)) goto cleanup;
    font_face_ready = true;
    if (!result_ok(bl_font_face_create_from_data(
            &font_face, &font_data, 0U))) goto cleanup;
    result = bl_font_init(&font);
    if (!result_ok(result)) goto cleanup;
    font_ready = true;
    if (!result_ok(bl_font_create_from_face(&font, &font_face,
                                            (float)font_size))) {
        goto cleanup;
    }
    result = bl_glyph_buffer_init(&glyph_buffer);
    if (!result_ok(result)) goto cleanup;
    glyph_buffer_ready = true;
    if (!result_ok(bl_glyph_buffer_set_text(
            &glyph_buffer, text, sizeof(text) - 1U,
            BL_TEXT_ENCODING_UTF8)) ||
        !result_ok(bl_font_shape(&font, &glyph_buffer))) goto cleanup;
    text_origin = (BLPointI){104, (int)height - 48};
    if (!result_ok(bl_context_fill_utf8_text_i_rgba32(
            &context, &text_origin, &font, text, sizeof(text) - 1U,
            0xFFFFFFFFU))) goto cleanup;

    if (!result_ok(bl_context_end(&context))) goto cleanup;
    context_active = false;

    result = bl_image_init(&scaled);
    if (!result_ok(result)) goto cleanup;
    scaled_ready = true;
    if (!result_ok(bl_image_scale(
            &scaled, &image, &scaled_size, BL_IMAGE_SCALE_FILTER_BILINEAR)) ||
        !result_ok(bl_image_get_data(&scaled, &scaled_data)) ||
        scaled_data.size.w != scaled_size.w ||
        scaled_data.size.h != scaled_size.h) goto cleanup;

    result = bl_image_codec_init(&codec);
    if (!result_ok(result)) goto cleanup;
    codec_ready = true;
    if (!result_ok(bl_image_codec_find_by_name(&codec, "PNG", 3U, 0))) {
        goto cleanup;
    }
    result = bl_array_init(&encoded, BL_OBJECT_TYPE_ARRAY_UINT8);
    if (!result_ok(result)) goto cleanup;
    encoded_ready = true;
    result = bl_image_encoder_init(&encoder);
    if (!result_ok(result)) goto cleanup;
    encoder_ready = true;
    if (!result_ok(bl_image_codec_create_encoder(&codec, &encoder)) ||
        !result_ok(bl_image_encoder_write_frame(&encoder, &encoded, &image))) {
        goto cleanup;
    }
    encoded_data = (const uint8_t *)bl_array_get_data(&encoded);
    encoded_size = bl_array_get_size(&encoded);
    if (encoded_data == 0 || encoded_size < 8U ||
        encoded_data[0] != 0x89U || encoded_data[1] != 'P' ||
        encoded_data[2] != 'N' || encoded_data[3] != 'G') goto cleanup;
    result = bl_image_decoder_init(&decoder);
    if (!result_ok(result)) goto cleanup;
    decoder_ready = true;
    if (!result_ok(bl_image_codec_create_decoder(&codec, &decoder)) ||
        !result_ok(bl_image_decoder_read_info(
            &decoder, &image_info, encoded_data, encoded_size)) ||
        image_info.size.w != (int)width || image_info.size.h != (int)height) {
        goto cleanup;
    }
    result = bl_image_init(&decoded);
    if (!result_ok(result)) goto cleanup;
    decoded_ready = true;
    if (!result_ok(bl_image_decoder_read_frame(
            &decoder, &decoded, encoded_data, encoded_size))) goto cleanup;

    success = pixels[0] != 0U && pixels[(uint64_t)height / 2U * stride +
                                        width / 2U] != 0U;

cleanup:
    if (context_active) (void)bl_context_end(&context);
    if (scaled_ready) (void)bl_image_destroy(&scaled);
    if (decoded_ready) (void)bl_image_destroy(&decoded);
    if (decoder_ready) (void)bl_image_decoder_destroy(&decoder);
    if (encoder_ready) (void)bl_image_encoder_destroy(&encoder);
    if (encoded_ready) (void)bl_array_destroy(&encoded);
    if (codec_ready) (void)bl_image_codec_destroy(&codec);
    if (glyph_buffer_ready) (void)bl_glyph_buffer_destroy(&glyph_buffer);
    if (font_ready) (void)bl_font_destroy(&font);
    if (font_face_ready) (void)bl_font_face_destroy(&font_face);
    if (font_data_ready) (void)bl_font_data_destroy(&font_data);
    if (gradient_ready) (void)bl_gradient_destroy(&gradient);
    if (path_ready) (void)bl_path_destroy(&path);
    if (context_ready) (void)bl_context_destroy(&context);
    if (image_ready) (void)bl_image_destroy(&image);
    if (runtime_ready) (void)bl_runtime_shutdown();
    return success;
}
