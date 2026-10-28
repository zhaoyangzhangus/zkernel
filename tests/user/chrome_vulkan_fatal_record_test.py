"""Execute the failure-only record without a working logger or heap."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() / 'components/viz/service/gl/gpu_service_impl.cc').read_text()
start = source.index('void RecordNativeVulkanContextFatal(')
helper = source[start:source.index('\n}', start) + 2]
start = source.index('void GpuServiceImpl::MaybeExitOnContextLost(')
body = source[start:source.index('bool GpuServiceImpl::IsExiting()', start)]
assert body.index('RecordNativeVulkanContextFatal(context_lost_reason);') < body.index('LOG(FATAL)')
assert body.count('RecordNativeVulkanContextFatal(') == 1
assert body.index('if (in_host_process())') < body.index('RecordNativeVulkanContextFatal(')
assert body.index('gpu_preferences_.disable_vulkan_surface') < body.index('RecordNativeVulkanContextFatal(')
assert source.count('RecordNativeVulkanContextFatal(') == 2
program = r'''
#include <cassert>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstddef>
namespace gpu::error {enum ContextLostReason : unsigned {kUnknown=2,kOutOfMemory=3};}
static char captured[128];
static unsigned calls;
static size_t length;
static int write_result;
static long write(int fd,const void *data,size_t size){
 assert(fd==2 && size<sizeof(captured));++calls;length=size;
 memcpy(captured,data,size);captured[size]=0;
 return write_result;
}
''' + helper + r'''
int main(){
 const unsigned values[]={0,1,2,3,4,5,6,7,8,15,16,255,256,65535,0xffffffffU};
 for(unsigned value:values)for(int result:{-1,0,1,46}){
  write_result=result;calls=0;
  RecordNativeVulkanContextFatal(static_cast<gpu::error::ContextLostReason>(value));
  char expected[128];
  snprintf(expected,sizeof(expected),"OS_CHROME_VULKAN_CONTEXT_FATAL reason=0x%08x\n",value);
  assert(calls==1 && length==strlen(expected) && !strcmp(captured,expected));
 }
}
'''
program = program.replace('#include <cstddef>', '#include <cstddef>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='vulkan-fatal-record-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c++', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=10)
print('PASS: 60 reason/write-result cases, one bounded write before fatal logging, native Vulkan host-only call site')
