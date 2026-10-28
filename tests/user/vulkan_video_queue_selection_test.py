"""Execute production queue selection: encode support is not decode support."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/nvidia/vulkan_video_device.c').read_text()
helper = source[source.index('static int video_choose_queue_families('):
                source.index('static void release(')]
fixture = r'''
#include <stdint.h>
#include <assert.h>
typedef uint32_t VkQueueFlags;
typedef uint32_t VkVideoCodecOperationFlagsKHR;
#define VK_QUEUE_COMPUTE_BIT 1U
#define VK_QUEUE_VIDEO_DECODE_BIT_KHR 2U
#define VK_QUEUE_VIDEO_ENCODE_BIT_KHR 4U
#define VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR 1U
#define VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR 2U
#define VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR 4U
#define VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR 8U
typedef struct { struct { uint32_t queueCount,queueFlags; } queueFamilyProperties; } VkQueueFamilyProperties2;
typedef struct { uint32_t videoCodecOperations; } VkQueueFamilyVideoPropertiesKHR;
'''
checks = r'''
int main(void) {
 VkQueueFamilyProperties2 q[3]={{{1,1}},{{1,2}},{{1,4}}};
 VkQueueFamilyVideoPropertiesKHR v[3]={{0},{1},{2}};
 int transfer, codec;
 assert(video_choose_queue_families(q,v,3,0,&transfer,&codec)&&transfer==0&&codec==1);
 assert(!video_choose_queue_families(q,v,3,2,&transfer,&codec));
 v[1].videoCodecOperations=4;
 assert(video_choose_queue_families(q,v,3,2,&transfer,&codec)&&codec==1);
 assert(!video_choose_queue_families(q,v,3,3,&transfer,&codec));
 v[1].videoCodecOperations=8;
 assert(video_choose_queue_families(q,v,3,3,&transfer,&codec)&&codec==1);
 v[1].videoCodecOperations=1;
 assert(video_choose_queue_families(q,v,3,1,&transfer,&codec)&&transfer==0&&codec==2);
 assert(!video_choose_queue_families(q,v,2,1,&transfer,&codec));
 v[2].videoCodecOperations=1;assert(!video_choose_queue_families(q,v,3,1,&transfer,&codec));
 v[2].videoCodecOperations=2;q[2].queueFamilyProperties.queueCount=0;
 assert(!video_choose_queue_families(q,v,3,1,&transfer,&codec));
 q[2].queueFamilyProperties.queueCount=1;q[0].queueFamilyProperties.queueCount=0;
 assert(!video_choose_queue_families(q,v,3,1,&transfer,&codec));
 q[0].queueFamilyProperties.queueCount=1;
 assert(!video_choose_queue_families(q,v,0,0,&transfer,&codec)&&transfer==-1&&codec==-1);
 q[0].queueFamilyProperties.queueFlags=3;v[0].videoCodecOperations=1;
 assert(!video_choose_queue_families(q,v,1,0,&transfer,&codec));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + helper + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-no-pie', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Actual queue selection: encode/decode isolation, codec capability, empty/absent/shared queue rejection passed')
