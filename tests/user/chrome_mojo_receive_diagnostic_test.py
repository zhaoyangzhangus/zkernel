"""Execute the real Mojo receive loop with deterministic transport failures."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
source = (chromium_source()/'mojo/core/channel_posix.cc').read_text()
start = source.index('void ChannelPosix::OnFdReadable(')
body = source[start:source.index('\n}\n', start)+3]
fixture = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/types.h>
#define CHECK_EQ(a,b) assert((a)==(b))
#define DCHECK_GT(a,b) assert((a)>(b))
#define DCHECK(x) assert(x)
static bool locked;
static std::ostringstream logs;
struct Log {
 template<class T>Log& operator<<(const T& v){assert(!locked);logs<<v;return *this;}
};
#define LOG(x) Log{}
namespace base {
struct ScopedFD {int fd;};
struct AutoLock{explicit AutoLock(int){assert(!locked);locked=true;}~AutoLock(){locked=false;}};
}
static constexpr size_t kMaxBatchReadCapacity=256*1024;
static int read_mode,calls;
static ssize_t SocketRecvmsg(int fd,void*,size_t,std::vector<base::ScopedFD>* descriptors){
 assert(fd==17&&!locked);++calls;
 if(read_mode==1){errno=EAGAIN;return -1;}
 if(read_mode==2){errno=EIO;return -1;}
 if(read_mode==3){errno=EAGAIN;return 0;}
 descriptors->push_back({99});return 4;
}
struct ChannelPosix {
 enum class Error{kDisconnected,kReceivedMalformedData};
 struct Socket{int get(){return 17;}} socket_;
 std::deque<base::ScopedFD> incoming_fds_;
 int write_lock_=0,errors=0,completed=0;bool valid=true,capacity=true;
 std::unique_ptr<int> read_watcher_=std::make_unique<int>(1);
 Error error=Error::kDisconnected;char buffer[8];
 char *GetReadBuffer(size_t *size){*size=sizeof(buffer);return capacity?buffer:nullptr;}
 bool OnReadComplete(size_t bytes,size_t *next){assert(bytes==4&&!locked);++completed;*next=0;return valid;}
 void OnError(Error e){assert(!locked&&!read_watcher_);error=e;++errors;}
 void OnFdReadable(int);
};
'''
test = r'''
int main(){
 for(int mode=0;mode<6;++mode){
  logs.str("");logs.clear();calls=0;read_mode=mode;
  ChannelPosix channel;
  if(mode==4)channel.valid=false;
  if(mode==5)channel.capacity=false;
  channel.OnFdReadable(17);
  bool failure=mode>=2;
  assert(channel.errors==int(failure)&&!locked);
  assert(bool(channel.read_watcher_)==!failure);
  assert(logs.str().empty()==!failure);
  if(mode==0)assert(channel.completed==1&&channel.incoming_fds_.size()==1);
  if(mode==1)assert(!channel.completed&&calls==1);
  if(mode==2)assert(logs.str().find("reason=recvmsg result=-1 errno=5")!=std::string::npos);
  if(mode==3)assert(logs.str().find("reason=peer_closed result=0 errno=0")!=std::string::npos);
  if(mode==4)assert(logs.str().find("reason=message_validation bytes=4 pending_fds=1")!=std::string::npos);
  if(mode==5)assert(logs.str().find("read_capacity_overflow")!=std::string::npos&&!calls);
  if(mode>=4)assert(channel.error==ChannelPosix::Error::kReceivedMalformedData);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-mojo-receive-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(fixture+body+test)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-D__NATIVE_OS__',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Mojo receive: success/EAGAIN silent, syscall errno, EOF stale-errno rejection, malformed message, capacity overflow and unlocked error callbacks passed')
