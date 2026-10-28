"""Measure actual socket layout and execute its ring helpers at real capacity."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
helpers = source[source.index('uint32_t socket_stream_write_locked('):
                 source.index('static uint32_t socket_active_ipv4_address(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/net/socket_model.h"
void socket_copy(void* d,const void* s,size_t n) {memmove(d,s,n);}
''' + helpers + r'''
static size_t physical(size_t request) {
 size_t n=4096;while(n<request+56)n*=2;return n;
}
int main(void) {
 assert(SOCKET_STREAM_BUFFER_SIZE>=8192 && SOCKET_STREAM_BUFFER_SIZE<=UINT16_MAX);
 size_t combined=sizeof(socket_private_t)+sizeof(socket_datagram_t)*SOCKET_RX_QUEUE_DEPTH;
 size_t tcp=sizeof(socket_private_t)+(SOCKET_STREAM_BUFFER_SIZE+7U)/8U;
 printf("stream_capacity=%u TCP_private_bytes=%zu TCP_buddy_bytes=%zu UDP_private_bytes=%zu UDP_buddy_bytes=%zu\n",
  SOCKET_STREAM_BUFFER_SIZE,tcp,physical(tcp),combined,physical(combined));
 assert(tcp<65536 && physical(tcp)==65536);
 assert(physical((SOCKET_STREAM_BUFFER_SIZE+7U)/8U)==8192);
 assert(physical(combined)==131072);
 socket_private_t* state=calloc(1,sizeof(*state));
 unsigned char* src=malloc(SOCKET_STREAM_BUFFER_SIZE);
 unsigned char* dst=malloc(SOCKET_STREAM_BUFFER_SIZE);assert(state && src && dst);
 for(unsigned i=0;i<SOCKET_STREAM_BUFFER_SIZE;++i)src[i]=(unsigned char)(i*37+(i>>8));
 unsigned positions[]={0,1,7,SOCKET_STREAM_BUFFER_SIZE/2,SOCKET_STREAM_BUFFER_SIZE-1};
 unsigned lengths[]={0,1,7,1460,8192,SOCKET_STREAM_BUFFER_SIZE-1,SOCKET_STREAM_BUFFER_SIZE};
 for(unsigned p=0;p<5;++p)for(unsigned n=0;n<7;++n) {
  unsigned start=positions[p],length=lengths[n];memset(state->stream_buffer,0xa5,SOCKET_STREAM_BUFFER_SIZE);
  state->stream_write=start;state->stream_read=start;
  unsigned next=socket_stream_write_locked(state,src,length);
  assert(next==(start+length)%SOCKET_STREAM_BUFFER_SIZE);
  assert(socket_stream_read_locked(state,dst,length)==next);
  assert(!memcmp(src,dst,length));
  for(unsigned i=length;i<SOCKET_STREAM_BUFFER_SIZE;++i)
   assert(state->stream_buffer[(start+i)%SOCKET_STREAM_BUFFER_SIZE]==0xa5);
  assert((uint16_t)(SOCKET_STREAM_BUFFER_SIZE-length)==SOCKET_STREAM_BUFFER_SIZE-length);
 }
 free(state);free(src);free(dst);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-stream-geometry-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DOS_KERNEL_BUILD','-I'+str(root),'-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
allocator = (root/'kernel/mm/kmalloc.c').read_text()
start = allocator.index('static void *allocate_large(')
allocate = allocator[start:allocator.index('\n}', start)+2]
start = allocator.index('typedef struct allocation_header {')
header = allocator[start:allocator.index('} allocation_header_t;', start)+len('} allocation_header_t;')]
defines = '\n'.join(line for line in allocator.splitlines() if re.match(
    r'#define KMEM_(MAGIC|PAYLOAD_OFFSET|TRAILER_SIZE|CLASS_LARGE|STATE_ALLOCATED|REDZONE_BYTE|FRONT_SIZE|POISON_ALLOC)\s', line))
probe = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "kernel/net/socket_model.h"
@DEFINES@
@HEADER@
#define BUDDY_MAX_ORDER 18
#define PAGE_OWNER_SLAB 1
typedef struct { unsigned owner; unsigned order; void *memory; } fixture_page_t;
static unsigned maximum_order=18, calls, live;
static size_t allocated_bytes;
static fixture_page_t *page_alloc(unsigned order, unsigned flags) {
 assert(!flags); ++calls;
 if(order>maximum_order)return NULL;
 fixture_page_t *p=malloc(sizeof(*p));assert(p);
 p->order=order;p->memory=malloc((size_t)PAGE_SIZE<<order);assert(p->memory);
 allocated_bytes+=(size_t)PAGE_SIZE<<order;++live;return p;
}
static uintptr_t page_to_phys(fixture_page_t *p){return (uintptr_t)p->memory;}
static void *phys_to_direct(uintptr_t p){return (void *)p;}
static void page_free(fixture_page_t *p){
 assert(live);--live;allocated_bytes-=(size_t)PAGE_SIZE<<p->order;
 free(p->memory);free(p);
}
static void bytes_fill(void *p,unsigned char value,size_t n){memset(p,value,n);}
static void *header_payload(allocation_header_t *p){return (char *)p+KMEM_PAYLOAD_OFFSET;}
static uint64_t allocation_cookie(allocation_header_t *h,void *p){assert(h && p);return 42;}
static void write_trailer(void *p,size_t n,uint64_t cookie){memcpy((char *)p+n,&cookie,sizeof(cookie));}
@ALLOCATE@
static void release(void *p,size_t requested,unsigned order){
 allocation_header_t *h=(void *)((char *)p-KMEM_PAYLOAD_OFFSET);
 fixture_page_t *page=(void *)h->owner;
 assert(h->requested==requested && h->magic==KMEM_MAGIC && page->order==order);
 assert(h->class_index==KMEM_CLASS_LARGE && h->state==KMEM_STATE_ALLOCATED);
 for(size_t i=sizeof(*h);i<KMEM_PAYLOAD_OFFSET;++i)
  assert(((unsigned char *)h)[i]==KMEM_REDZONE_BYTE);
 for(size_t i=0;i<requested;++i)assert(((unsigned char *)p)[i]==KMEM_POISON_ALLOC);
 uint64_t trailer;memcpy(&trailer,(char *)p+requested,sizeof(trailer));assert(trailer==h->cookie);
 page_free(page);
}
int main(void){
 size_t tcp=sizeof(socket_private_t)+(SOCKET_STREAM_BUFFER_SIZE+7U)/8U;
 size_t udp=sizeof(socket_private_t)+sizeof(socket_datagram_t)*SOCKET_RX_QUEUE_DEPTH;
 for(unsigned cycle=0;cycle<128;++cycle){
  void *t=allocate_large(tcp,0,false),*u=allocate_large(udp,0,false);
  assert(t && u && live==2 && allocated_bytes==196608);
  release(t,tcp,4);release(u,udp,5);assert(!live && !allocated_bytes);
 }
 /* Under fragmentation, a 64 KiB block still admits TCP without demanding
  * the 128 KiB block previously wasted on an unused datagram queue. */
 maximum_order=4;
 void *t=allocate_large(tcp,0,false);assert(t && allocated_bytes==65536);
 assert(!allocate_large(udp,0,false));release(t,tcp,4);
 assert(!live && !allocated_bytes && calls==258);
 puts("Actual allocate_large: 128 TCP/UDP lifecycles, orders 4/5, header/redzone/payload/trailer checks and fragmented TCP admission PASS");
}
'''.replace('@DEFINES@', defines).replace('@HEADER@', header).replace(
    '@ALLOCATE@', allocate.replace('page_t *', 'fixture_page_t *'))
with tempfile.TemporaryDirectory(prefix='socket-real-allocator-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(probe)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DOS_KERNEL_BUILD','-I'+str(root),'-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
print('Actual stream ring: full capacity, wrap, untouched bytes, wire-window bounds and Buddy allocation class passed')
