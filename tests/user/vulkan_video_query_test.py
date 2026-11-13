"""Compile the actual FFmpeg query routine against a checking Vulkan driver."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'build/nvidia/vulkan/ffmpeg-7.1.1/libavutil/vulkan.c').read_text()
start = source.index('VkResult ff_vk_exec_get_query(')
end = source.index('\n}\n', start) + 3
function = source[start:end]
assert 'pool->query_64bit = !!query_64bit;' in source
decode = (root / 'build/nvidia/vulkan/ffmpeg-7.1.1/libavcodec/vulkan_decode.c').read_text()
assert 'if (ret == VK_SUCCESS)\n            av_log(avctx,' in decode
assert 'int32_t *result;' in decode
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define AV_LOG_ERROR 1
typedef struct { PFN_vkGetQueryPoolResults GetQueryPoolResults; } FFVulkanFunctions;
typedef struct { FFVulkanFunctions vkfn; AVVulkanDeviceContext *hwctx; } FFVulkanContext;
typedef struct { int query_64bit, query_statuses, nb_queries; size_t qd_size;
                 VkQueryPool query_pool; } FFVkExecPool;
typedef struct { FFVkExecPool *parent; void *query_data; unsigned query_idx; } FFVkExecContext;
static void av_log(void *a,int b,const char *c,...) {(void)a;(void)b;(void)c;}
static unsigned calls, count, bits, statuses;
static VkResult answer;
static VKAPI_ATTR VkResult VKAPI_CALL query(VkDevice d,VkQueryPool p,uint32_t first,
    uint32_t n,size_t size,void *data,VkDeviceSize stride,VkQueryResultFlags flags) {
    (void)d;(void)p;assert(first==7);++calls;assert(n==count);
    size_t width=bits ? 8 : 4;
    assert(stride==width*(statuses ? 2 : 1));assert(size==n*stride);
    assert(!!(flags&VK_QUERY_RESULT_64_BIT)==!!bits);
    assert(!!(flags&VK_QUERY_RESULT_WITH_STATUS_BIT_KHR)==!!statuses);
    assert(flags&VK_QUERY_RESULT_WAIT_BIT);
    if(answer==VK_SUCCESS)memset(data,1,size);
    return answer;
}
'''
# 使用真实的公共设备类型；此处只需 act_dev，因此保持定义精简。
fixture = fixture.replace('typedef struct { FFVulkanFunctions vkfn;',
    'typedef struct { VkDevice act_dev; } AVVulkanDeviceContext;\n'
    'typedef struct { FFVulkanFunctions vkfn;')
test = r'''
int main(void) {
 AVVulkanDeviceContext dev={0};FFVulkanContext s={{query},&dev};
 unsigned char storage[128];
 for(bits=0;bits<2;bits++)for(statuses=0;statuses<2;statuses++)
 for(count=1;count<=4;count++) {
  FFVkExecPool pool={bits,statuses,(int)count,count*(bits?8:4)*(statuses?2:1),0};
  FFVkExecContext e={&pool,storage,7};void *out;
  VkResult results[]={VK_SUCCESS,VK_NOT_READY,VK_ERROR_DEVICE_LOST};
  for(unsigned i=0;i<3;i++) {
   answer=results[i];out=(void*)1;
   assert(ff_vk_exec_get_query(&s,&e,&out,VK_QUERY_RESULT_WAIT_BIT|
       VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_STATUS_BIT_KHR)==answer);
   assert(out==(answer==VK_SUCCESS ? storage : NULL));
  }
  answer=VK_SUCCESS;assert(ff_vk_exec_get_query(&s,&e,NULL,VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS);
  unsigned previous=calls;e.query_data=NULL;
  assert(ff_vk_exec_get_query(&s,&e,&out,0)==VK_INCOMPLETE);assert(calls==previous);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-query-test-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(fixture + function + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'build/nvidia/vulkan/sdk/include'),
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=5)
print('Vulkan query regression: 32/64-bit, multiple queries, status stride and stale-result rejection passed')
