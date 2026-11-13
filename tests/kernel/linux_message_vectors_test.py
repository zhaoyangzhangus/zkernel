"""Actual message load/gather/scatter with poisoned unused vectors and guards."""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()

def body(name):
    match = re.search(r'static [^\n]+ ' + name + r'\(', source)
    assert match
    start = source.index('{',match.start())
    end, depth = start+1, 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[match.start():end]

for name in ('linux_sendmsg','linux_recvmsg'):
    text = body(name)
    assert 'linux_iovec_t vectors[LINUX_MAX_IOV];' in text
    assert 'vectors[LINUX_MAX_IOV] = ' not in text
    assert text.index('linux_message_load(') < text.index('if (status != K_OK) return status;')

types = source[source.index('typedef struct linux_iovec {'):
               source.index('typedef struct linux_pollfd {')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define __user
#define LINUX_MAX_IOV 64U
#define K_OK 0
#define K_EINVAL -22
#define K_EOVERFLOW -75
#define LINUX_EFAULT -14
static unsigned calls,fail_call;
static uintptr_t forbidden;
static bool linux_user_pointer(uint64_t pointer,size_t length) {
 return length==0 || (pointer!=0 && pointer!=forbidden && pointer<=UINTPTR_MAX-length);
}
static int copy_from_user(void *out,const void *in,size_t length) {
 if(++calls==fail_call)return LINUX_EFAULT;
 assert(length==0 || (uintptr_t)in!=forbidden);memcpy(out,in,length);return K_OK;
}
static int copy_to_user(void *out,const void *in,size_t length) {
 if(++calls==fail_call)return LINUX_EFAULT;
 assert(length==0 || (uintptr_t)out!=forbidden);memcpy(out,in,length);return K_OK;
}
''' + types + '\n' + '\n'.join(body(name) for name in (
    'linux_message_load','linux_message_gather','linux_message_scatter')) + r'''
static uint8_t buffers[64][66],expected[4096],output[4098];
static linux_iovec_t input[64];
int main(void) {
 unsigned cases=0;forbidden=1;
 for(unsigned count=0;count<=64;++count)for(unsigned mode=0;mode<3;++mode) {
  linux_msghdr_t header={0};header.iov_length=count;
  header.iov_pointer=count?(uintptr_t)input:forbidden;
  size_t total=0;
  for(unsigned i=0;i<count;++i) {
   memset(buffers[i],0xa5,sizeof(buffers[i]));
   unsigned length=mode==0?0:mode==1?1:(i*17U+count)%65;
   for(unsigned j=0;j<length;++j)buffers[i][j+1]=(uint8_t)(i+j+7);
   input[i]=(linux_iovec_t){(uintptr_t)(buffers[i]+1),length};
   memcpy(expected+total,buffers[i]+1,length);total+=length;
  }
  struct {uint64_t a;linux_iovec_t vectors[64];uint64_t b;} scratch;
  memset(&scratch,0xcd,sizeof(scratch));scratch.a=17;scratch.b=23;
  linux_msghdr_t loaded;uint64_t loaded_total=UINT64_MAX;
  calls=0;fail_call=0;
  assert(linux_message_load((uintptr_t)&header,&loaded,scratch.vectors,&loaded_total)==K_OK);
  assert(loaded_total==total && calls==(count?2U:1U));
  assert(!memcmp(scratch.vectors,input,count*sizeof(input[0])));
  const uint8_t *tail=(const uint8_t*)(scratch.vectors+count);
  for(size_t n=0;n<(64-count)*sizeof(input[0]);++n)assert(tail[n]==0xcd);
  assert(scratch.a==17 && scratch.b==23);
  for(unsigned size=0;size<=total+1;++size) {
   size_t copied=123,limit=size<total?size:total;
   memset(output,0xa5,sizeof(output));calls=0;
   assert(linux_message_gather(scratch.vectors,count,output+1,size,&copied)==K_OK);
   assert(copied==limit && !memcmp(output+1,expected,limit));
   assert(output[0]==0xa5 && output[limit+1]==0xa5);
   for(unsigned i=0;i<count;++i)memset(buffers[i]+1,0xcc,input[i].length);
   calls=0;
   assert(linux_message_scatter(scratch.vectors,count,expected,size)==(size<=total?K_OK:LINUX_EFAULT));
   size_t position=0;
   for(unsigned i=0;i<count;++i) {
    size_t n=input[i].length<limit-position?input[i].length:limit-position;
    assert(!memcmp(buffers[i]+1,expected+position,n));
    for(size_t j=n;j<input[i].length;++j)assert(buffers[i][j+1]==0xcc);
    assert(buffers[i][0]==0xa5 && buffers[i][65]==0xa5);position+=n;
   }
   /* 为下一次 gather 还原源字节。 */
   position=0;for(unsigned i=0;i<count;++i){memcpy(buffers[i]+1,expected+position,input[i].length);position+=input[i].length;}
   ++cases;
  }
  for(unsigned failure=1;failure<=(count?2U:1U);++failure) {
   calls=0;fail_call=failure;loaded_total=123;
   assert(linux_message_load((uintptr_t)&header,&loaded,scratch.vectors,&loaded_total)==LINUX_EFAULT);
   assert(loaded_total==123 && calls==failure);
  }
  if(total) {
   size_t copied=123;memset(output,0xa5,sizeof(output));calls=0;fail_call=1;
   assert(linux_message_gather(scratch.vectors,count,output+1,total,&copied)==LINUX_EFAULT);
   assert(copied==123 && output[1]==0xa5 && calls==1);
   calls=0;
   assert(linux_message_scatter(scratch.vectors,count,expected,total)==LINUX_EFAULT && calls==1);
   fail_call=0;
  }
 }
 linux_msghdr_t header={0},loaded;linux_iovec_t vectors[64];uint64_t total;
 fail_call=0;header.iov_length=65;
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==K_EINVAL);
 header.iov_length=1;header.iov_pointer=forbidden;
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==LINUX_EFAULT);
 header.iov_pointer=(uintptr_t)input;input[0]=(linux_iovec_t){forbidden,1};
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==LINUX_EFAULT);
 input[0].length=0;
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==K_OK && total==0);
 header.control_length=1;header.control=0;
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==LINUX_EFAULT);
 header.control_length=0;header.iov_length=4;
 for(unsigned i=0;i<4;++i)input[i]=(linux_iovec_t){0x1000,UINT64_C(1)<<62};
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,&total)==K_EOVERFLOW);
 assert(linux_message_load((uintptr_t)&header,0,vectors,&total)==LINUX_EFAULT);
 assert(linux_message_load((uintptr_t)&header,&loaded,0,&total)==LINUX_EFAULT);
 assert(linux_message_load((uintptr_t)&header,&loaded,vectors,0)==LINUX_EFAULT);
 size_t copied=123;
 assert(linux_message_gather(0,0,output,0,&copied)==K_EINVAL && copied==123);
 assert(linux_message_scatter(vectors,0,0,0)==K_EINVAL);
 printf("Message vectors: %u exact gather/scatter cases, 0..64 active entries, poisoned unused slots, guards, copy faults and invalid pointers passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-message-vectors-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    negative=code.replace('index < count && copied < capacity',
                          'index <= count && copied < capacity')
    assert negative!=code
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode and ('AddressSanitizer' in result.stderr or 'Assertion' in result.stderr)
    print('Negative control rejected: gather inspects an unused poisoned vector')
