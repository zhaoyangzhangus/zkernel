#include <uapi/video_decoder.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct os_video_decoder {
    AVCodecContext *codec;
    AVBufferRef *device;
    uint32_t codec_type;
    AVFrame *hardware, *download;
    AVPacket *packet;
    AVBufferPool *packet_pool;
    size_t packet_capacity;
};

int video_device_create(AVBufferRef **output);
int video_decode_device_create(AVBufferRef **output, uint32_t codec);
void video_decode_device_release(AVBufferRef **device, uint32_t codec);
int video_decode_device_query(uint32_t codec, os_video_decode_capabilities_t *output);

static enum AVPixelFormat select_hardware(AVCodecContext *codec,
                                         const enum AVPixelFormat *formats) {
    (void)codec;
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        if (*formats == AV_PIX_FMT_VULKAN) return *formats;
    /* 绝不允许 FFmpeg 静默改用 CPU 解码 H.264。 */
    return AV_PIX_FMT_NONE;
}

uint32_t os_video_decoder_abi(void) { return OS_VIDEO_DECODER_ABI; }

int os_video_decoder_query(uint32_t codec, os_video_decode_capabilities_t *output) {
    if (!output) return AVERROR(EINVAL);
    memset(output, 0, sizeof(*output));
    enum AVCodecID id;
    switch (codec) {
    case OS_VIDEO_CODEC_H264: id = AV_CODEC_ID_H264; break;
    case OS_VIDEO_CODEC_HEVC: id = AV_CODEC_ID_HEVC; break;
    case OS_VIDEO_CODEC_AV1: id = AV_CODEC_ID_AV1; break;
    default: return AVERROR(EINVAL);
    }
    if (!avcodec_find_decoder(id)) return AVERROR(ENOSYS);
    return video_decode_device_query(codec, output);
}

void os_video_decoder_destroy(os_video_decoder_t *decoder) {
    if (!decoder) return;
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->download);
    av_frame_free(&decoder->hardware);
    avcodec_free_context(&decoder->codec);
    av_buffer_pool_uninit(&decoder->packet_pool);
    video_decode_device_release(&decoder->device, decoder->codec_type);
    free(decoder);
}

int os_video_decoder_create(uint32_t codec_type, const uint8_t *extra,
                            uint32_t extra_size, os_video_decoder_t **output) {
    if (!output) return AVERROR(EINVAL);
    *output = NULL;
    if ((!extra && extra_size) || extra_size > 16U * 1024U * 1024U)
        return AVERROR(EINVAL);
    enum AVCodecID codec_id;
    switch (codec_type) {
    case OS_VIDEO_CODEC_H264: codec_id = AV_CODEC_ID_H264; break;
    case OS_VIDEO_CODEC_HEVC: codec_id = AV_CODEC_ID_HEVC; break;
    case OS_VIDEO_CODEC_AV1: codec_id = AV_CODEC_ID_AV1; break;
    default: return AVERROR(EINVAL);
    }
    os_video_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return AVERROR(ENOMEM);
    decoder->codec_type = codec_type;
    int result = video_decode_device_create(&decoder->device, codec_type);
    if (result < 0) goto fail;
    const AVCodec *codec = avcodec_find_decoder(codec_id);
    if (!codec) { result = AVERROR(ENOSYS); goto fail; }
    decoder->codec = avcodec_alloc_context3(codec);
    decoder->hardware = av_frame_alloc();
    decoder->download = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    if (!decoder->codec || !decoder->hardware || !decoder->download ||
        !decoder->packet) { result = AVERROR(ENOMEM); goto fail; }
    decoder->codec->get_format = select_hardware;
    decoder->codec->hw_device_ctx = av_buffer_ref(decoder->device);
    if (!decoder->codec->hw_device_ctx) { result = AVERROR(ENOMEM); goto fail; }
    decoder->codec->pkt_timebase = (AVRational){1, 1000000};
    decoder->codec->thread_count = 1;
    if (extra_size) {
        decoder->codec->extradata = av_mallocz(
            (size_t)extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!decoder->codec->extradata) { result = AVERROR(ENOMEM); goto fail; }
        memcpy(decoder->codec->extradata, extra, extra_size);
        decoder->codec->extradata_size = (int)extra_size;
    }
    result = avcodec_open2(decoder->codec, codec, NULL);
    if (result < 0) goto fail;
    *output = decoder;
    return 0;
fail:
    os_video_decoder_destroy(decoder);
    return result;
}

int os_video_decoder_create_h264(const uint8_t *extra, uint32_t extra_size,
                                os_video_decoder_t **output) {
    return os_video_decoder_create(OS_VIDEO_CODEC_H264, extra, extra_size, output);
}

int os_video_decoder_send(os_video_decoder_t *decoder, const uint8_t *data,
                          uint32_t size, int64_t timestamp_us) {
    if (!decoder || (!data && size) || size >= INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);
    if (!data && !size) return avcodec_send_packet(decoder->codec, NULL);
    if (!size) return AVERROR(EINVAL);
    av_packet_unref(decoder->packet);
    /* 缓冲池只复用已归还的存储；解码器保留的引用不会被下一包覆盖。 */
    if (size > decoder->packet_capacity) {
        AVBufferPool *pool = av_buffer_pool_init(
            (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE, NULL);
        if (!pool) return AVERROR(ENOMEM);
        av_buffer_pool_uninit(&decoder->packet_pool);
        decoder->packet_pool = pool;
        decoder->packet_capacity = size;
    }
    decoder->packet->buf = av_buffer_pool_get(decoder->packet_pool);
    if (!decoder->packet->buf) return AVERROR(ENOMEM);
    decoder->packet->data = decoder->packet->buf->data;
    decoder->packet->size = (int)size;
    memcpy(decoder->packet->data, data, size);
    memset(decoder->packet->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    decoder->packet->pts = timestamp_us;
    decoder->packet->dts = AV_NOPTS_VALUE;
    int result = avcodec_send_packet(decoder->codec, decoder->packet);
    av_packet_unref(decoder->packet);
    return result;
}

int os_video_decoder_receive(os_video_decoder_t *decoder,
                             os_video_frame_t *output) {
    if (!decoder || !output) return AVERROR(EINVAL);
    memset(output, 0, sizeof(*output));
    /* avcodec_receive_frame 会释放上一帧，即使调用出错也如此。 */
    int result = avcodec_receive_frame(decoder->codec, decoder->hardware);
    if (result < 0) return result;
    if (decoder->hardware->format != AV_PIX_FMT_VULKAN)
        return AVERROR(ENOSYS);
    /* 调用方会在接收下一帧前复制借用的像素。
     * NV12 可写存储保留到 reset 或实际尺寸变化时。 */
    if (decoder->download->buf[0] &&
        (decoder->download->width != decoder->hardware->width ||
         decoder->download->height != decoder->hardware->height))
        av_frame_unref(decoder->download);
    result = av_hwframe_transfer_data(decoder->download, decoder->hardware, 0);
    if (result < 0) return result;
    AVFrame *frame = decoder->download;
    os_video_frame_t decoded = {
        .width = (uint32_t)frame->width, .height = (uint32_t)frame->height,
        .stride_y = frame->linesize[0], .stride_uv = frame->linesize[1],
        .y = frame->data[0], .uv = frame->data[1],
        .timestamp_us = decoder->hardware->best_effort_timestamp,
    };
    if (frame->format != AV_PIX_FMT_NV12 || !os_video_frame_layout_valid(&decoded))
        return AVERROR(ENOSYS);
    *output = decoded;
    return 0;
}

void os_video_decoder_reset(os_video_decoder_t *decoder) {
    if (!decoder) return;
    avcodec_flush_buffers(decoder->codec);
    av_frame_unref(decoder->hardware);
    av_frame_unref(decoder->download);
    av_packet_unref(decoder->packet);
    av_buffer_pool_uninit(&decoder->packet_pool);
    decoder->packet_capacity = 0;
}
