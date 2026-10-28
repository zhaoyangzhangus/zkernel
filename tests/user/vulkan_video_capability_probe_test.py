"""Run the real profile/NV12 probe with fake Vulkan entry points, no host GPU."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'tools/nvidia/vulkan_video_device.c').read_text()
start=source.index('int video_decode_device_query(')
end=source.index('static int video_choose_queue_families(',start)
function=source[start:end]
fixture=r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <uapi/video_decoder.h>
#define AVERROR(e) (-(e))
static int failure;
static unsigned creates,destroys,calls;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
 const VkAllocationCallbacks *allocation,VkInstance *out){
 assert(info->pApplicationInfo->apiVersion==VK_API_VERSION_1_3&&!allocation);
 ++creates;if(failure==1)return VK_ERROR_INITIALIZATION_FAILED;
 *out=(VkInstance)(uintptr_t)1;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance i,const VkAllocationCallbacks *a){
 assert(i&&!a);++destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i,uint32_t *n,VkPhysicalDevice *p){
 assert(i);if(failure==3)return VK_ERROR_INITIALIZATION_FAILED;
 if(!p){*n=failure==4?33:1;return VK_SUCCESS;}
 p[0]=(VkPhysicalDevice)(uintptr_t)2;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p,VkPhysicalDeviceProperties *out){
 assert(p);memset(out,0,sizeof(*out));
 out->vendorID=failure==5?0x1002:0x10de;
 out->deviceType=failure==6?VK_PHYSICAL_DEVICE_TYPE_CPU:VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
}
static VKAPI_ATTR VkResult VKAPI_CALL capabilities(VkPhysicalDevice p,const VkVideoProfileInfoKHR *profile,VkVideoCapabilitiesKHR *out){
 assert(p&&profile&&out);++calls;
 assert(profile->chromaSubsampling==VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR);
 assert(profile->lumaBitDepth==VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
 assert(profile->chromaBitDepth==VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR);
 assert(out->sType==VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR&&out->pNext);
 const VkVideoDecodeCapabilitiesKHR *decode=out->pNext;
 assert(decode->sType==VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR&&decode->pNext);
 switch(profile->videoCodecOperation){
 case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
  assert(((VkVideoDecodeH264ProfileInfoKHR*)profile->pNext)->stdProfileIdc==STD_VIDEO_H264_PROFILE_IDC_HIGH);break;
 case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
  assert(((VkVideoDecodeH265ProfileInfoKHR*)profile->pNext)->stdProfileIdc==STD_VIDEO_H265_PROFILE_IDC_MAIN);break;
 case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
  assert(((VkVideoDecodeAV1ProfileInfoKHR*)profile->pNext)->stdProfile==STD_VIDEO_AV1_PROFILE_MAIN);break;
 default:assert(0);
 }
 if(failure==7)return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;
 out->minCodedExtent=(VkExtent2D){failure==10?0:16,16};
 out->maxCodedExtent=(VkExtent2D){failure==11?8:8192,8192};return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL formats(VkPhysicalDevice p,const VkPhysicalDeviceVideoFormatInfoKHR *info,uint32_t *n,VkVideoFormatPropertiesKHR *out){
 assert(p&&info->pNext&&info->imageUsage==VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR);
 if(!out){*n=failure==8?65:1;return VK_SUCCESS;}
 assert(out[0].sType==VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR);
 if(failure==12)return VK_INCOMPLETE;
 out[0].format=failure==9?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
 return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char *name){
 assert(i);if(failure==2)return NULL;
 if(!strcmp(name,"vkGetPhysicalDeviceVideoCapabilitiesKHR"))return (PFN_vkVoidFunction)capabilities;
 assert(!strcmp(name,"vkGetPhysicalDeviceVideoFormatPropertiesKHR"));return (PFN_vkVoidFunction)formats;
}
''' + function + r'''
int main(void){
 os_video_decode_capabilities_t out;
 assert(video_decode_device_query(2,NULL)==-EINVAL&&!creates);
 memset(&out,0xff,sizeof(out));assert(video_decode_device_query(99,&out)==-EINVAL&&!out.max_width&&!creates);
 for(unsigned codec=1;codec<=3;++codec){
  assert(!video_decode_device_query(codec,&out));
  assert(out.min_width==16&&out.max_width==8192&&creates==destroys);
 }
 for(failure=1;failure<=12;++failure){
  unsigned old=destroys;memset(&out,0xff,sizeof(out));
  assert(video_decode_device_query(2,&out)<0&&!out.min_width&&!out.max_width);
  assert(destroys==old+(failure==1?0:1));
 }
 assert(calls);
}
'''
with tempfile.TemporaryDirectory(prefix='video-capability-') as temporary:
    binary=str(Path(temporary)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),
                    '-I'+str(root/'build/nvidia/vulkan/sdk/include'),
                    '-x','c','-','-o',binary],input=fixture,text=True,check=True)
    subprocess.run([binary],check=True)
print('Actual Vulkan capability probe: profile/NV12/extent validation, 12 failure paths and instance cleanup PASS')
