/* 仅供主机端诊断使用的元数据。绝不链接进客户机解码器。 */
#include <stddef.h>
#include <stdio.h>
#include <libavcodec/h264dec.h>
#include <libavcodec/vulkan_decode.h>

#define FIELD(key, type, field) printf("\"" key "\":%zu,\n", offsetof(type, field))
int main(void) {
    puts("{");
    FIELD("picture_info", FFVulkanDecodePicture, decode_info);
    FIELD("picture_slices", FFVulkanDecodePicture, slices_buf);
    FIELD("picture_slice_bytes", FFVulkanDecodePicture, slices_size);
    FIELD("buffer_ref_data", AVBufferRef, data);
    FIELD("vk_buffer_mapped", FFVkBuffer, mapped_mem);
    FIELD("vk_buffer_size", FFVkBuffer, size);
    FIELD("info_next", VkVideoDecodeInfoKHR, pNext);
    FIELD("info_setup", VkVideoDecodeInfoKHR, pSetupReferenceSlot);
    FIELD("info_count", VkVideoDecodeInfoKHR, referenceSlotCount);
    FIELD("info_refs", VkVideoDecodeInfoKHR, pReferenceSlots);
    FIELD("h264_std", VkVideoDecodeH264PictureInfoKHR, pStdPictureInfo);
    FIELD("picture_num", StdVideoDecodeH264PictureInfo, frame_num);
    FIELD("picture_poc", StdVideoDecodeH264PictureInfo, PicOrderCnt);
    FIELD("picture_flags", StdVideoDecodeH264PictureInfo, flags);
    FIELD("slot_index", VkVideoReferenceSlotInfoKHR, slotIndex);
    FIELD("slot_resource", VkVideoReferenceSlotInfoKHR, pPictureResource);
    FIELD("slot_next", VkVideoReferenceSlotInfoKHR, pNext);
    FIELD("ref_std", VkVideoDecodeH264DpbSlotInfoKHR, pStdReferenceInfo);
    FIELD("ref_num", StdVideoDecodeH264ReferenceInfo, FrameNum);
    FIELD("ref_poc", StdVideoDecodeH264ReferenceInfo, PicOrderCnt);
    FIELD("ref_flags", StdVideoDecodeH264ReferenceInfo, flags);
    FIELD("resource_view", VkVideoPictureResourceInfoKHR, imageViewBinding);
    FIELD("frame_pts", AVFrame, pts);
    FIELD("codec_private", AVCodecContext, priv_data);
    FIELD("h264_short_count", H264Context, short_ref_count);
    FIELD("h264_short_refs", H264Context, short_ref);
    FIELD("cpu_picture_num", H264Picture, frame_num);
    FIELD("cpu_picture_poc", H264Picture, field_poc);
    FIELD("cpu_picture_gap", H264Picture, invalid_gap);
    FIELD("codec_internal", AVCodecContext, internal);
    FIELD("internal_hwaccel", AVCodecInternal, hwaccel_priv_data);
    FIELD("decode_shared", FFVulkanDecodeContext, shared_ctx);
    FIELD("decode_dedicated", FFVulkanDecodeContext, dedicated_dpb);
    FIELD("shared_caps", FFVulkanDecodeShared, dec_caps);
    FIELD("shared_video_caps", FFVulkanDecodeShared, caps);
    FIELD("bitstream_alignment", VkVideoCapabilitiesKHR, minBitstreamBufferSizeAlignment);
    FIELD("decode_caps_flags", VkVideoDecodeCapabilitiesKHR, flags);
    printf("\"slot_size\":%zu,\"layout_version\":1\n}\n", sizeof(VkVideoReferenceSlotInfoKHR));
    return 0;
}
