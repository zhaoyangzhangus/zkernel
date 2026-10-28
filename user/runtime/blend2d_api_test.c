#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <blend2d/blend2d.h>

#include "blend2d_api_test.h"

#define BLEND2D_TEST_WIDTH  128U
#define BLEND2D_TEST_HEIGHT 96U

static uint32_t g_blend2d_test_pixels[BLEND2D_TEST_WIDTH *
                                      BLEND2D_TEST_HEIGHT];

static bool blend2d_result_ok(BLResult result) {
    return result == BL_SUCCESS;
}

int blend2d_api_test(void) {
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
    BLImageData image_data = {0};
    BLImageInfo image_info = {0};
    BLSizeI scaled_size = {64, 48};
    BLLinearGradientValues gradient_values = {0, 0, 0, 1};
    BLRectI gradient_rect = {4, 4, 120, 12};
    BLRect path_rect = {24, 22, 48, 30};
    BLPoint origin = {0, 0};
    BLMatrix2D matrix;
    BLPoint matrix_input = {2, 3};
    BLPoint matrix_output = {0, 0};
    BLRectI stroke_rect = {16, 16, 96, 64};
    BLPointI text_origin = {12, 84};
    const uint8_t *encoded_data = 0;
    size_t encoded_size = 0U;
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

    result = bl_runtime_init();
    if (!blend2d_result_ok(result)) goto cleanup;
    runtime_ready = true;

    result = bl_image_init_as_from_data(
        &image, (int)BLEND2D_TEST_WIDTH, (int)BLEND2D_TEST_HEIGHT,
        BL_FORMAT_XRGB32, g_blend2d_test_pixels,
        (intptr_t)(BLEND2D_TEST_WIDTH * sizeof(uint32_t)),
        BL_DATA_ACCESS_RW, 0, 0);
    if (!blend2d_result_ok(result)) goto cleanup;
    image_ready = true;

    result = bl_context_init_as(&context, &image, 0);
    if (!blend2d_result_ok(result)) goto cleanup;
    context_ready = true;
    context_active = true;
    if (!blend2d_result_ok(bl_context_set_comp_op(&context, BL_COMP_OP_SRC_COPY)) ||
        !blend2d_result_ok(bl_context_fill_all_rgba32(&context, 0xFF101A30U))) {
        goto cleanup;
    }

    result = bl_path_init(&path);
    if (!blend2d_result_ok(result)) goto cleanup;
    path_ready = true;
    if (!blend2d_result_ok(bl_path_add_rect_d(
            &path, &path_rect, BL_GEOMETRY_DIRECTION_CW)) ||
        !blend2d_result_ok(bl_context_fill_path_d_rgba32(
            &context, &origin, &path, 0xFF1CC8A0U)) ||
        !blend2d_result_ok(bl_context_set_stroke_width(&context, 3.0)) ||
        !blend2d_result_ok(bl_context_stroke_path_d_rgba32(
            &context, &origin, &path, 0xFF7DD6FFU))) {
        goto cleanup;
    }

    gradient_values.x1 = 0;
    gradient_values.y1 = 1;
    result = bl_gradient_init_as(
        &gradient, BL_GRADIENT_TYPE_LINEAR, &gradient_values,
        BL_EXTEND_MODE_PAD, 0, 0U, 0);
    if (!blend2d_result_ok(result)) goto cleanup;
    gradient_ready = true;
    if (!blend2d_result_ok(bl_gradient_add_stop_rgba32(
            &gradient, 0.0, 0xFF2A6A92U)) ||
        !blend2d_result_ok(bl_gradient_add_stop_rgba32(
            &gradient, 1.0, 0xFF163B68U)) ||
        !blend2d_result_ok(bl_context_fill_rect_i_ext(
            &context, &gradient_rect, (const BLUnknown *)&gradient))) {
        goto cleanup;
    }

    if (!blend2d_result_ok(bl_matrix2d_set_translation(&matrix, 5.0, 7.0)) ||
        !blend2d_result_ok(bl_matrix2d_map_pointd_array(
            &matrix, &matrix_output, &matrix_input, 1U)) ||
        matrix_output.x != 7.0 || matrix_output.y != 10.0) {
        goto cleanup;
    }
    {
        double translation[2] = {8.0, 0.0};
        if (!blend2d_result_ok(bl_context_apply_transform_op(
                &context, BL_TRANSFORM_OP_TRANSLATE, translation)) ||
            !blend2d_result_ok(bl_context_stroke_rect_i_rgba32(
                &context, &stroke_rect, 0xFFE5B567U))) {
            goto cleanup;
        }
    }

    result = bl_font_data_init(&font_data);
    if (!blend2d_result_ok(result)) goto cleanup;
    font_data_ready = true;
    if (!blend2d_result_ok(bl_font_data_create_from_file(
            &font_data, "/EFI/OS/fonts/default.ttf", BL_FILE_READ_NO_FLAGS))) {
        goto cleanup;
    }
    result = bl_font_face_init(&font_face);
    if (!blend2d_result_ok(result)) goto cleanup;
    font_face_ready = true;
    if (!blend2d_result_ok(bl_font_face_create_from_data(
            &font_face, &font_data, 0U))) goto cleanup;
    result = bl_font_init(&font);
    if (!blend2d_result_ok(result)) goto cleanup;
    font_ready = true;
    if (!blend2d_result_ok(bl_font_create_from_face(
            &font, &font_face, 18.0f))) goto cleanup;
    result = bl_glyph_buffer_init(&glyph_buffer);
    if (!blend2d_result_ok(result)) goto cleanup;
    glyph_buffer_ready = true;
    if (!blend2d_result_ok(bl_glyph_buffer_set_text(
            &glyph_buffer, "System Blend2D", SIZE_MAX,
            BL_TEXT_ENCODING_UTF8)) ||
        !blend2d_result_ok(bl_font_shape(&font, &glyph_buffer)) ||
        !blend2d_result_ok(bl_context_fill_utf8_text_i_rgba32(
            &context, &text_origin, &font, "System Blend2D", SIZE_MAX,
            0xFFFFFFFFU))) goto cleanup;

    if (!blend2d_result_ok(bl_context_end(&context))) goto cleanup;
    context_active = false;

    result = bl_image_init(&scaled);
    if (!blend2d_result_ok(result)) goto cleanup;
    scaled_ready = true;
    if (!blend2d_result_ok(bl_image_scale(
            &scaled, &image, &scaled_size, BL_IMAGE_SCALE_FILTER_BILINEAR)) ||
        !blend2d_result_ok(bl_image_get_data(&scaled, &image_data)) ||
        image_data.size.w != scaled_size.w ||
        image_data.size.h != scaled_size.h) goto cleanup;

    result = bl_image_codec_init(&codec);
    if (!blend2d_result_ok(result)) goto cleanup;
    codec_ready = true;
    if (!blend2d_result_ok(bl_image_codec_find_by_name(
            &codec, "PNG", 3U, 0))) goto cleanup;
    result = bl_array_init(&encoded, BL_OBJECT_TYPE_ARRAY_UINT8);
    if (!blend2d_result_ok(result)) goto cleanup;
    encoded_ready = true;
    result = bl_image_encoder_init(&encoder);
    if (!blend2d_result_ok(result)) goto cleanup;
    encoder_ready = true;
    result = bl_image_codec_create_encoder(&codec, &encoder);
    if (!blend2d_result_ok(result)) goto cleanup;
    if (!blend2d_result_ok(bl_image_encoder_write_frame(
            &encoder, &encoded, &image))) goto cleanup;
    encoded_data = (const uint8_t *)bl_array_get_data(&encoded);
    encoded_size = bl_array_get_size(&encoded);
    if (encoded_data == 0 || encoded_size < 8U ||
        encoded_data[0] != 0x89U || encoded_data[1] != 'P' ||
        encoded_data[2] != 'N' || encoded_data[3] != 'G') goto cleanup;

    result = bl_image_decoder_init(&decoder);
    if (!blend2d_result_ok(result)) goto cleanup;
    decoder_ready = true;
    result = bl_image_codec_create_decoder(&codec, &decoder);
    if (!blend2d_result_ok(result)) goto cleanup;
    if (!blend2d_result_ok(bl_image_decoder_read_info(
            &decoder, &image_info, encoded_data, encoded_size)) ||
        image_info.size.w != (int)BLEND2D_TEST_WIDTH ||
        image_info.size.h != (int)BLEND2D_TEST_HEIGHT) goto cleanup;
    result = bl_image_init(&decoded);
    if (!blend2d_result_ok(result)) goto cleanup;
    decoded_ready = true;
    if (!blend2d_result_ok(bl_image_decoder_read_frame(
            &decoder, &decoded, encoded_data, encoded_size))) goto cleanup;

    success = g_blend2d_test_pixels[0] == 0xFF101A30U &&
              g_blend2d_test_pixels[30U * BLEND2D_TEST_WIDTH + 40U] !=
                  0xFF101A30U;

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
