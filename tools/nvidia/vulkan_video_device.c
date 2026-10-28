#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <uapi/video_decoder.h>

/* 创建仅包含必要传输/计算及 H264 解码或编码功能的设备。FFmpeg 自动创建设备会启用无关的编码/光流引擎及其队列。 */
struct video_device {
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance;
};
static const char *extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};
static const char *hevc_extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME};
static const char *av1_extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME};
static const char *encode_extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME};

/* 能力探测不会分配逻辑设备或解码会话。向页面公开能力前，先检查实际 profile 和输出格式。 */
int video_decode_device_query(uint32_t codec, os_video_decode_capabilities_t *output) {
    if (!output) return AVERROR(EINVAL);
    memset(output, 0, sizeof(*output));
    if (codec < OS_VIDEO_CODEC_H264 || codec > OS_VIDEO_CODEC_AV1)
        return AVERROR(EINVAL);
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Video capability probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo create = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app};
    int result = AVERROR(ENOSYS);
    if (vkCreateInstance(&create,NULL,&instance) != VK_SUCCESS) return result;
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_caps =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(
            instance,"vkGetPhysicalDeviceVideoCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR get_formats =
        (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)vkGetInstanceProcAddr(
            instance,"vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    uint32_t count=0;
    if (!get_caps || !get_formats ||
        vkEnumeratePhysicalDevices(instance,&count,NULL)!=VK_SUCCESS ||
        !count || count>32) goto done;
    VkPhysicalDevice devices[32];
    if (vkEnumeratePhysicalDevices(instance,&count,devices)!=VK_SUCCESS || count>32) goto done;
    VkVideoDecodeH264ProfileInfoKHR h264 = {
        .sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR,
        .stdProfileIdc=STD_VIDEO_H264_PROFILE_IDC_HIGH,
        .pictureLayout=VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR};
    VkVideoDecodeH265ProfileInfoKHR hevc = {
        .sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR,
        .stdProfileIdc=STD_VIDEO_H265_PROFILE_IDC_MAIN};
    VkVideoDecodeAV1ProfileInfoKHR av1 = {
        .sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR,
        .stdProfile=STD_VIDEO_AV1_PROFILE_MAIN};
    VkVideoProfileInfoKHR profile = {.sType=VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
        .chromaSubsampling=VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        .lumaBitDepth=VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .chromaBitDepth=VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR};
    profile.pNext = codec==OS_VIDEO_CODEC_H264 ? (const void*)&h264 :
                    codec==OS_VIDEO_CODEC_HEVC ? (const void*)&hevc : (const void*)&av1;
    profile.videoCodecOperation = codec==OS_VIDEO_CODEC_H264 ? VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR :
        codec==OS_VIDEO_CODEC_HEVC ? VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR : VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    for (uint32_t i=0;i<count;++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i],&properties);
        if (properties.vendorID!=0x10de ||
            properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        VkVideoDecodeH264CapabilitiesKHR c264={.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
        VkVideoDecodeH265CapabilitiesKHR c265={.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
        VkVideoDecodeAV1CapabilitiesKHR cav1={.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};
        VkVideoDecodeCapabilitiesKHR decode={.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,
            .pNext=codec==OS_VIDEO_CODEC_H264 ? (void*)&c264 : codec==OS_VIDEO_CODEC_HEVC ? (void*)&c265 : (void*)&cav1};
        VkVideoCapabilitiesKHR caps={.sType=VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,.pNext=&decode};
        if (get_caps(devices[i],&profile,&caps)!=VK_SUCCESS) continue;
        VkVideoProfileListInfoKHR list={.sType=VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
            .profileCount=1,.pProfiles=&profile};
        VkPhysicalDeviceVideoFormatInfoKHR info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
            .pNext=&list,.imageUsage=VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR};
        uint32_t formats_count=0;
        if (get_formats(devices[i],&info,&formats_count,NULL)!=VK_SUCCESS ||
            !formats_count || formats_count>64) continue;
        VkVideoFormatPropertiesKHR formats[64];
        memset(formats,0,sizeof(formats));
        for (uint32_t j=0;j<formats_count;++j) formats[j].sType=VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        if (get_formats(devices[i],&info,&formats_count,formats)!=VK_SUCCESS || formats_count>64) continue;
        for (uint32_t j=0;j<formats_count;++j) {
            if (formats[j].format!=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) continue;
            if (!caps.minCodedExtent.width || !caps.minCodedExtent.height ||
                caps.maxCodedExtent.width<caps.minCodedExtent.width ||
                caps.maxCodedExtent.height<caps.minCodedExtent.height) break;
            *output=(os_video_decode_capabilities_t){caps.minCodedExtent.width,
                caps.minCodedExtent.height,caps.maxCodedExtent.width,caps.maxCodedExtent.height};
            result=0;
            goto done;
        }
    }
done:
    vkDestroyInstance(instance,NULL);
    return result;
}
static int video_choose_queue_families(const VkQueueFamilyProperties2 *queues,
                                      const VkQueueFamilyVideoPropertiesKHR *video,
                                      uint32_t count, int mode,
                                      int *transfer, int *codec_queue) {
    int encode = mode == 1;
    VkQueueFlags required_queue = encode ? VK_QUEUE_VIDEO_ENCODE_BIT_KHR : VK_QUEUE_VIDEO_DECODE_BIT_KHR;
    VkVideoCodecOperationFlagsKHR required_codec = encode ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR :
        mode == 2 ? VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR :
        mode == 3 ? VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR :
                    VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    *transfer = *codec_queue = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!queues[i].queueFamilyProperties.queueCount) continue;
        VkQueueFlags flags = queues[i].queueFamilyProperties.queueFlags;
        if (*transfer < 0 && (flags & VK_QUEUE_COMPUTE_BIT)) *transfer = (int)i;
        if (*codec_queue < 0 && (flags & required_queue) &&
            (video[i].videoCodecOperations & required_codec)) *codec_queue = (int)i;
    }
    return *transfer >= 0 && *codec_queue >= 0 && *transfer != *codec_queue;
}
static void release(AVHWDeviceContext *ctx) {
    AVVulkanDeviceContext *vk=ctx->hwctx;
    if(vk->act_dev){vkDeviceWaitIdle(vk->act_dev);vkDestroyDevice(vk->act_dev,NULL);}
    if(vk->inst)vkDestroyInstance(vk->inst,NULL);
    free(ctx->user_opaque);
}
#define CHECK(call) do {VkResult r=(call);if(r!=VK_SUCCESS){fprintf(stderr,"VIDEO_DEVICE_FAIL %s result=%d\n",#call,r);goto fail;}}while(0)
static int video_device_create_mode(AVBufferRef **output, int mode) {
    int encode = mode == 1;
    if (!output) return AVERROR(EINVAL);
    *output = NULL;
    AVBufferRef *hw=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if(!hw)return AVERROR(ENOMEM);
    AVHWDeviceContext *ctx=(AVHWDeviceContext*)hw->data;
    AVVulkanDeviceContext *vk=ctx->hwctx;
    struct video_device *own=calloc(1,sizeof(*own));
    if(!own){av_buffer_unref(&hw);return AVERROR(ENOMEM);}
    ctx->user_opaque=own;ctx->free=release;
    vk->get_proc_addr=vkGetInstanceProcAddr;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Vulkan Video",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ic,NULL,&vk->inst));
    uint32_t n=0;CHECK(vkEnumeratePhysicalDevices(vk->inst,&n,NULL));
    if(!n||n>32)goto fail;
    VkPhysicalDevice devices[32];CHECK(vkEnumeratePhysicalDevices(vk->inst,&n,devices));
    for(uint32_t i=0;i<n;++i) {
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(devices[i],&p);
        if(p.vendorID==0x10de&&p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){vk->phys_dev=devices[i];break;}
    }
    if(!vk->phys_dev)goto fail;
    own->f13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    if (encode) {
        own->video_maintenance.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR;
        own->f13.pNext=&own->video_maintenance;
    }
    own->f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    own->f12.pNext=&own->f13;
    own->f11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    own->f11.pNext=&own->f12;
    VkPhysicalDeviceFeatures2 available={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&own->f11};
    vkGetPhysicalDeviceFeatures2(vk->phys_dev,&available);
    if(!own->f12.timelineSemaphore||!own->f13.synchronization2||
       (encode&&!own->video_maintenance.videoMaintenance1))goto fail;
    VkBool32 ycbcr=own->f11.samplerYcbcrConversion,bda=own->f12.bufferDeviceAddress,m4=own->f13.maintenance4;
    own->f11=(VkPhysicalDeviceVulkan11Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&own->f12,.samplerYcbcrConversion=ycbcr};
    own->f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&own->f13,.timelineSemaphore=1,.bufferDeviceAddress=bda};
    own->f13=(VkPhysicalDeviceVulkan13Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=1,.maintenance4=m4};
    if (encode) own->f13.pNext=&own->video_maintenance;
    vk->device_features=(VkPhysicalDeviceFeatures2){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&own->f11};
    vkGetPhysicalDeviceQueueFamilyProperties2(vk->phys_dev,&n,NULL);
    if(!n||n>32)goto fail;
    VkQueueFamilyProperties2 qp[32]={0};VkQueueFamilyVideoPropertiesKHR vp[32]={0};
    for(uint32_t i=0;i<n;++i){vp[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;qp[i].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;qp[i].pNext=&vp[i];}
    vkGetPhysicalDeviceQueueFamilyProperties2(vk->phys_dev,&n,qp);
    const VkVideoCodecOperationFlagsKHR codec = encode ? VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR :
        mode == 2 ? VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR :
        mode == 3 ? VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR :
                    VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    int transfer=-1,decode=-1;
    if(!video_choose_queue_families(qp,vp,n,mode,&transfer,&decode))goto fail;
    float priority=1;
    VkDeviceQueueCreateInfo qi[2]={
        {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=(uint32_t)transfer,.queueCount=1,.pQueuePriorities=&priority},
        {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=(uint32_t)decode,.queueCount=1,.pQueuePriorities=&priority}};
    const char **selected_extensions = encode ? encode_extensions :
        mode == 2 ? hevc_extensions : mode == 3 ? av1_extensions : extensions;
    uint32_t extension_count = encode ? 4U : 3U;
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&vk->device_features,
        .queueCreateInfoCount=2,.pQueueCreateInfos=qi,.enabledExtensionCount=extension_count,.ppEnabledExtensionNames=selected_extensions};
    CHECK(vkCreateDevice(vk->phys_dev,&dc,NULL,&vk->act_dev));
    vk->enabled_dev_extensions=selected_extensions;vk->nb_enabled_dev_extensions=extension_count;
    vk->qf[0]=(AVVulkanDeviceQueueFamily){.idx=transfer,.num=1,.flags=qp[transfer].queueFamilyProperties.queueFlags};
    vk->qf[1]=(AVVulkanDeviceQueueFamily){.idx=decode,.num=1,.flags=qp[decode].queueFamilyProperties.queueFlags,.video_caps=codec};
    vk->nb_qf=2;
    /* 已弃用的固定索引仍属于 FFmpeg 7 ABI。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    vk->queue_family_index=transfer;vk->nb_graphics_queues=1;
    vk->queue_family_tx_index=transfer;vk->nb_tx_queues=1;
    vk->queue_family_comp_index=transfer;vk->nb_comp_queues=1;
    vk->queue_family_decode_index=encode ? -1 : decode;vk->nb_decode_queues=encode ? 0 : 1;
    vk->queue_family_encode_index=encode ? decode : -1;vk->nb_encode_queues=encode ? 1 : 0;
#pragma GCC diagnostic pop
    int err=av_hwdevice_ctx_init(hw);
    if(err<0){av_buffer_unref(&hw);return err;}
    if (encode) printf("VIDEO_ENCODE_DEVICE_READY transfer_family=%d encode_family=%d queues=2\n",transfer,decode);
    else printf("VIDEO_DEVICE_READY transfer_family=%d decode_family=%d queues=2\n",transfer,decode);
    *output=hw;return 0;
fail:
    av_buffer_unref(&hw);return AVERROR_EXTERNAL;
}
int video_device_create(AVBufferRef **output) {
    return video_device_create_mode(output, 0);
}
/* 同一编码格式的并存解码器共享设备，队列同步仍由 FFmpeg 设备上下文负责。
 * 只等待所属编码的锁；最后一个解码器退出后在锁外释放，不常驻占用显存。 */
typedef struct video_decode_device_entry {
    pthread_mutex_t lock;
    AVBufferRef *device;
    unsigned users;
} video_decode_device_entry_t;

static video_decode_device_entry_t decode_devices[3] = {
    {.lock = PTHREAD_MUTEX_INITIALIZER},
    {.lock = PTHREAD_MUTEX_INITIALIZER},
    {.lock = PTHREAD_MUTEX_INITIALIZER},
};

int video_decode_device_create(AVBufferRef **output, uint32_t codec) {
    if (!output) return AVERROR(EINVAL);
    *output = NULL;
    if (codec < OS_VIDEO_CODEC_H264 || codec > OS_VIDEO_CODEC_AV1)
        return AVERROR(EINVAL);
    video_decode_device_entry_t *entry = &decode_devices[codec - OS_VIDEO_CODEC_H264];
    AVBufferRef *retired = NULL;
    int result = pthread_mutex_lock(&entry->lock);
    if (result) return AVERROR(result);
    if (entry->users == UINT_MAX) {
        result = AVERROR(EOVERFLOW);
    } else {
        if (!entry->device)
            result = video_device_create_mode(&entry->device,
                codec == OS_VIDEO_CODEC_H264 ? 0 : (int)codec);
        if (!result) {
            *output = av_buffer_ref(entry->device);
            if (*output) ++entry->users;
            else {
                result = AVERROR(ENOMEM);
                if (!entry->users) {
                    /* 分配引用失败时也在锁外销毁设备，避免驱动等待占住池锁。 */
                    retired = entry->device;
                    entry->device = NULL;
                }
            }
        }
    }
    pthread_mutex_unlock(&entry->lock);
    av_buffer_unref(&retired);
    return result;
}

void video_decode_device_release(AVBufferRef **device, uint32_t codec) {
    if (!device || !*device) return;
    if (codec < OS_VIDEO_CODEC_H264 || codec > OS_VIDEO_CODEC_AV1) return;
    video_decode_device_entry_t *entry = &decode_devices[codec - OS_VIDEO_CODEC_H264];
    AVBufferRef *retired = NULL;
    if (pthread_mutex_lock(&entry->lock)) return;
    if (entry->device && entry->users && entry->device->data == (*device)->data) {
        /* 脱离池的最后一份引用在解锁后释放，驱动等待不能阻塞新建解码器。 */
        if (--entry->users == 0) {
            retired = entry->device;
            entry->device = NULL;
        }
    }
    pthread_mutex_unlock(&entry->lock);
    av_buffer_unref(device);
    av_buffer_unref(&retired);
}
int video_encode_device_create(AVBufferRef **output) {
    return video_device_create_mode(output, 1);
}
