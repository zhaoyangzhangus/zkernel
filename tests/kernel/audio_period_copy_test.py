"""Production audio copy/period routing and kernel memcpy against byte-copy oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/audio/core.c').read_text()
helper = source[source.index('static void audio_copy_bytes('):
                source.index('static uint32_t audio_sample_bytes(')]
period = source[source.index('static kstatus_t audio_stream_period_copy_locked('):
                source.index('/* 在同一临界区检查、复制并发布')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/audio.h>
void *liteos_memcpy(void *,const void *,size_t);
static unsigned copies,locked,locks,unlocks;
static void *counted_copy(void *out,const void *in,size_t n) {
    ++copies;return liteos_memcpy(out,in,n);
}
#define AUDIO_PERIOD_FREE 0U
typedef struct {page_t **pages;uint32_t page_count;uint32_t state;} audio_period_t;
struct audio_stream {
    audio_stream_state_t state;
    audio_direction_t direction;
    audio_format_t format;
    uint64_t period_bytes;
    audio_period_t periods[2];
};
static void audio_lock(audio_stream_t *stream) {assert(stream && !locked);locked=1;++locks;}
static void audio_unlock(audio_stream_t *stream) {assert(stream && locked);locked=0;++unlocks;}
static page_t pages[3];
static unsigned char physical[3*PAGE_SIZE],buffer[3*PAGE_SIZE+64],expected[3*PAGE_SIZE+64];
paddr_t page_to_phys(const page_t *p) {
    assert(locked && p>=pages && p<pages+3);return paddr_make((uint64_t)(p-pages)*PAGE_SIZE);
}
void *phys_to_direct(paddr_t p) {
    assert(locked && p.value<sizeof(physical));return physical+p.value;
}
''' + helper.replace('memcpy(', 'counted_copy(') + period + r'''
static void oracle(unsigned char *out,const unsigned char *in,size_t n) {
    for(size_t i=0;i<n;++i)out[i]=in[i];
}
int main(void) {
    unsigned char actual[256],model[256];unsigned cases=0;
    audio_copy_bytes(0,0,0);assert(!copies);
    for(unsigned a=0;a<32;++a)for(unsigned b=0;b<64;++b)for(unsigned n=0;n<=128;++n) {
        for(unsigned i=0;i<256;++i)actual[i]=model[i]=(unsigned char)(i*19+3);
        copies=0;audio_copy_bytes(actual+a,actual+b,n);oracle(model+a,model+b,n);
        assert(!memcmp(actual,model,sizeof(actual)));
        unsigned distance=a>=b?a-b:b-a;
        assert(copies==(unsigned)(n && distance>=n));++cases;
    }
    audio_stream_t stream={.state=AUDIO_STREAM_CONFIGURED,.period_bytes=sizeof(physical)};
    stream.format.period_count=1;
    page_t *list[3]={pages,pages+1,pages+2};
    stream.periods[0]=(audio_period_t){.pages=list,.page_count=3};
    unsigned sizes[]={1,63,64,65,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,2*PAGE_SIZE,3*PAGE_SIZE};
    for(unsigned align=0;align<16;++align)for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        unsigned n=sizes[i];
        for(unsigned j=0;j<sizeof(buffer);++j)buffer[j]=(unsigned char)(j*13+align);
        memset(physical,0xa5,sizeof(physical));memset(expected,0xa5,sizeof(physical));
        oracle(expected,buffer+align,n);copies=locks=unlocks=0;
        assert(audio_stream_period_write(&stream,0,buffer+align,n)==K_OK);
        assert(!memcmp(physical,expected,sizeof(physical)));
        assert(copies==(n+PAGE_SIZE-1)/PAGE_SIZE && locks==1 && unlocks==1 && !locked);
        memset(buffer,0xb7,sizeof(buffer));memset(expected,0xb7,sizeof(expected));
        oracle(expected+align,physical,n);copies=locks=unlocks=0;
        assert(audio_stream_period_read(&stream,0,buffer+align,n)==K_OK);
        assert(!memcmp(buffer,expected,sizeof(buffer)));
        assert(copies==(n+PAGE_SIZE-1)/PAGE_SIZE && locks==1 && unlocks==1 && !locked);
    }
    locks=unlocks=copies=0;
    assert(audio_stream_period_read(0,0,buffer,1)==K_EINVAL);
    assert(audio_stream_period_read(&stream,0,0,1)==K_EINVAL);
    assert(audio_stream_period_read(&stream,0,buffer,0)==K_EINVAL);
    assert(!locks && !unlocks && !copies);
    assert(audio_stream_period_read(&stream,1,buffer,1)==K_EINVAL && !locked);
    assert(audio_stream_period_read(&stream,0,buffer,sizeof(physical)+1)==K_EINVAL && !locked);
    stream.state=AUDIO_STREAM_NEW;
    assert(audio_stream_period_read(&stream,0,buffer,1)==K_EINVAL && !locked);
    stream.state=AUDIO_STREAM_DISCONNECTED;
    assert(audio_stream_period_read(&stream,0,buffer,1)==K_EDEVREMOVED && !locked);
    stream.state=AUDIO_STREAM_CONFIGURED;
    for(unsigned missing=0;missing<2;++missing) {
        if(missing)list[1]=0;else stream.periods[0].page_count=1;
        memset(buffer,0xb7,sizeof(buffer));memset(expected,0xb7,sizeof(expected));
        oracle(expected,physical,PAGE_SIZE);copies=0;
        assert(audio_stream_period_read(&stream,0,buffer,PAGE_SIZE+1)==K_EIO && !locked);
        assert(copies==1 && !memcmp(buffer,expected,sizeof(buffer)));
        stream.periods[0].page_count=3;list[1]=pages+1;
    }
    assert(locks==unlocks);
    printf("Audio copy: %u forward-byte oracle cases including overlap, 288 aligned multi-page read/write routes, canaries and unchanged validation/partial-failure/lock boundaries passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-audio-copy-') as directory:
    obj = str(Path(directory)/'string.o')
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-x','none',obj,'-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
