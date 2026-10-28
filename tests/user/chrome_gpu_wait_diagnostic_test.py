"""Exercise the real command-buffer wait and generation handling without a GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() / 'gpu/ipc/client/command_buffer_proxy_impl.cc').read_text()
def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#define TRACE_EVENT2(...) ((void)0)
static std::ostringstream diagnostic;
#define LOG(x) diagnostic
namespace base {struct AutoLock {explicit AutoLock(int) {}};}
namespace mojo {struct SyncCallRestrictions {struct ScopedAllowSyncCall {};};}
namespace gpu {
namespace error {enum Error {kNoError, kLost};}
struct CommandBuffer {
 struct State {error::Error error=error::kNoError; uint32_t generation=0;
 uint32_t set_get_buffer_count=0; int32_t get_offset=0;};
};
}
struct Client {int lost=0;void OnGpuControlLostContextMaybeReentrant(){++lost;}};
struct Channel {
 bool succeeds=true;int calls=0;gpu::CommandBuffer::State reply;
 Channel& GetGpuChannel(){return *this;}
 bool WaitForGetOffsetInRange(int,uint32_t,int32_t,int32_t,gpu::CommandBuffer::State* s){
 ++calls;*s=reply;return succeeds;
 }
};
struct CommandBufferProxyImpl {
 gpu::CommandBuffer::State last_state_;
 int shared_state_lock_=0,route_id_=1,failures=0,state_errors=0;
 Client* gpu_control_client_=nullptr;Channel* channel_;
 void CheckLock(){} void TryUpdateState(){}
 void OnGpuSyncReplyError(){++failures;last_state_.error=gpu::error::kLost;}
 void OnGpuStateError(){++state_errors;}
 bool InRange(int a,int b,int value){return value>=a&&value<=b;}
 gpu::CommandBuffer::State WaitForGetOffsetInRange(uint32_t,int32_t,int32_t);
 void SetStateFromMessageReply(const gpu::CommandBuffer::State&);
};
'''
test = r'''
int main(){
 for(int kind=0;kind<7;++kind){
  diagnostic.str("");diagnostic.clear();Channel channel;CommandBufferProxyImpl p;
  p.channel_=&channel;p.last_state_={gpu::error::kNoError,10,2,0};
  channel.reply={gpu::error::kNoError,11,2,8};
  if(kind==1)channel.succeeds=false;
  if(kind==2)channel.reply.generation=9;
  if(kind==3)channel.reply.set_get_buffer_count=3;
  if(kind==4)channel.reply.get_offset=20;
  if(kind==5){p.last_state_.generation=UINT32_MAX;channel.reply.generation=0;}
  if(kind==6)channel.reply.error=gpu::error::kLost;
  auto result=p.WaitForGetOffsetInRange(2,5,10);
  bool invalid=kind>=1&&kind<=4;
  assert(channel.calls==1&&p.failures==int(invalid));
  assert((result.error==gpu::error::kLost)==(invalid||kind==6));
  assert(diagnostic.str().empty()==!invalid);
  if(invalid){
   assert(diagnostic.str().find(kind==1?"reply=0":"reply=1")!=std::string::npos);
   assert(diagnostic.str().find("route=1 expected_buffer=2 range=5:10")!=std::string::npos);
   assert(diagnostic.str().find("reply_generation=")!=std::string::npos);
  }
  int calls=channel.calls;p.WaitForGetOffsetInRange(2,5,10);
  assert(channel.calls==calls); // satisfied or already lost: never busy-retry.
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-gpu-wait-') as directory:
    path = Path(directory)
    code = fixture + function('gpu::CommandBuffer::State CommandBufferProxyImpl::WaitForGetOffsetInRange(')
    code += function('void CommandBufferProxyImpl::SetStateFromMessageReply(') + test
    (path / 'test.cc').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('GPU wait: success, transport failure, stale reply, buffer/offset mismatch, generation wrap and context loss passed')
