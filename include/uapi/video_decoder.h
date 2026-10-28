#pragma once
#include <stdint.h>

/* 独立的 Vulkan Video 解码器 ABI。同一实例上的所有调用均串行执行。
 * 输出存储仅借用至下一次 receive/reset/destroy 调用。
 * NV12 回读不是零拷贝；解码本身必须在 Vulkan 硬件上运行。 */
#define OS_VIDEO_DECODER_ABI 1U
#define OS_VIDEO_AGAIN (-11)
#define OS_VIDEO_EOF (-541478725)
#define OS_VIDEO_CODEC_H264 1U
#define OS_VIDEO_CODEC_HEVC 2U
#define OS_VIDEO_CODEC_AV1 3U
typedef struct os_video_decoder os_video_decoder_t;
/* 查询 8 位 4:2:0 解码至 NV12 的能力，而不只是检查扩展标志位。 */
typedef struct os_video_decode_capabilities {
    uint32_t min_width, min_height, max_width, max_height;
} os_video_decode_capabilities_t;
typedef struct os_video_frame {
    uint32_t width, height;
    int32_t stride_y, stride_uv;
    const uint8_t *y, *uv;
    int64_t timestamp_us;
} os_video_frame_t;

/* 在跨库复制像素前验证借用平面的布局。奇数宽度的 NV12 最后一个色度样本
 * 需要两个交错字节。 */
static inline int os_video_frame_layout_valid(const os_video_frame_t *frame) {
    if (!frame || !frame->width || !frame->height ||
        frame->width >= INT32_MAX || frame->height > INT32_MAX ||
        !frame->y || !frame->uv || frame->stride_y <= 0 || frame->stride_uv <= 0)
        return 0;
    uint32_t chroma_bytes = ((frame->width + 1U) / 2U) * 2U;
    return (uint32_t)frame->stride_y >= frame->width &&
           (uint32_t)frame->stride_uv >= chroma_bytes;
}

#ifdef __cplusplus
extern "C" {
#endif
uint32_t os_video_decoder_abi(void);
int os_video_decoder_query(uint32_t codec, os_video_decode_capabilities_t *output);
/* 新增的 ABI-1 入口；原有 H.264 入口仍受支持。 */
int os_video_decoder_create(uint32_t codec, const uint8_t *extra,
                            uint32_t extra_size, os_video_decoder_t **output);
int os_video_decoder_create_h264(const uint8_t *extra, uint32_t extra_size,
                                os_video_decoder_t **output);
/* Null packet and zero length drain the stream. */
int os_video_decoder_send(os_video_decoder_t *, const uint8_t *, uint32_t,
                          int64_t timestamp_us);
int os_video_decoder_receive(os_video_decoder_t *, os_video_frame_t *);
void os_video_decoder_reset(os_video_decoder_t *);
void os_video_decoder_destroy(os_video_decoder_t *);
#ifdef __cplusplus
}
#endif
