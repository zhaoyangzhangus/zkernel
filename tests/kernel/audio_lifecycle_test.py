"""Production audio lifecycle with controlled allocation, DMA and backend hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/audio/core.c').read_text()
source = source.replace('atomic_compare_exchange_strong_explicit(&g_audio_init_state',
                        'counted_cas(&g_audio_init_state')
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
static _Thread_local unsigned depth,caller_depth;
static _Thread_local int heap_budget=-1,page_budget=-1,map_budget=-1;
static _Thread_local _Bool fail_unmap;
static _Thread_local int usb_disconnect_status,hda_disconnect_status;
static _Thread_local void *failed_stream;
static atomic_uint attempts,heaps,page_count,mappings,callbacks;
static _Bool parallel;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned desired,
                         memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,desired,success,failure);
}
''' + source + r'''
static device_t device;
static bool allowed(int *budget) {
    if(*budget==0)return false;
    if(*budget>0)--*budget;
    return true;
}
void *kzalloc(size_t n,uint32_t flags) {
    assert(!flags && depth==caller_depth+(n!=sizeof(audio_stream_t)));
    if(!allowed(&heap_budget))return 0;
    void *p=calloc(1,n);assert(p);atomic_fetch_add(&heaps,1);return p;
}
void kfree(void *p) {assert(p && depth<=caller_depth+1);assert(atomic_fetch_sub(&heaps,1));free(p);}
page_t *page_alloc(uint8_t order,page_alloc_flags_t flags) {
    assert(!order && flags==(PAGE_ALLOC_ZERO|PAGE_ALLOC_DMA32) && depth==caller_depth+1);
    if(!allowed(&page_budget))return 0;
    page_t *p=calloc(1,sizeof(*p)+PAGE_SIZE);assert(p);atomic_fetch_add(&page_count,1);return p;
}
void page_free(page_t *p) {assert(p && depth==caller_depth+1);assert(atomic_fetch_sub(&page_count,1));free(p);}
paddr_t page_to_phys(const page_t *p) {assert(p);return paddr_make((uintptr_t)(p+1));}
void *phys_to_direct(paddr_t p) {return (void *)(uintptr_t)p.value;}
void object_get(void *p) {assert(p==&device);assert(atomic_fetch_add(&device.object.refs.value,1));}
void object_put(void *p) {
    assert(p && depth==caller_depth);object_header_t *h=p;
    unsigned before=atomic_fetch_sub(&h->refs.value,1);assert(before);
    if(before==1) {assert(p!=&device && h->ops);h->ops->destroy(p);}
}
kstatus_t dma_map_pages(device_t *d,page_t **p,uint32_t n,enum dma_direction direction,dma_mapping_t *out) {
    assert(d==&device && p && n && depth==caller_depth+1);
    assert(direction==DMA_TO_DEVICE || direction==DMA_FROM_DEVICE);
    if(!allowed(&map_budget))return K_EIO;
    *out=(dma_mapping_t){.device=d,.pages=p,.page_count=n,.direction=direction};
    atomic_fetch_add(&device.object.refs.value,1);atomic_fetch_add(&mappings,1);return K_OK;
}
kstatus_t dma_unmap_checked(dma_mapping_t *m) {
    assert(depth==caller_depth+1 && m && m->device==&device);
    if(fail_unmap)return K_EIO;
    assert(atomic_fetch_sub(&mappings,1));assert(atomic_fetch_sub(&device.object.refs.value,1)>1);
    *m=(dma_mapping_t){0};return K_OK;
}
void dma_sync_for_device(dma_mapping_t *m) {assert(m && depth==caller_depth+1);}
void dma_sync_for_cpu(dma_mapping_t *m) {assert(m && depth==caller_depth+1);}
kstatus_t xhci_audio_stream_disconnect(audio_stream_t *s) {
    assert(s && depth==caller_depth && !atomic_load(&s->lock.state));
    atomic_fetch_add(&callbacks,1);return !failed_stream || s==failed_stream ? usb_disconnect_status : K_ENOENT;
}
kstatus_t hda_audio_stream_disconnect(audio_stream_t *s) {
    assert(s && depth==caller_depth && !atomic_load(&s->lock.state));
    atomic_fetch_add(&callbacks,1);return !failed_stream || s==failed_stream ? hda_disconnect_status : K_ENOENT;
}
static audio_format_t format={.sample_rate=48000,.channels=2,.sample_format=AUDIO_SAMPLE_S16_LE,
                             .period_frames=128,.period_count=4};
static void clean(void) {
    assert(depth==caller_depth && !atomic_load(&heaps) && !atomic_load(&page_count));
    assert(!atomic_load(&mappings) && atomic_load(&device.object.refs.value)==1);
}
static void *initializer(void *unused) {(void)unused;assert(audio_core_init());return 0;}
static void cycle(void) {
    audio_stream_t *s=0;
    assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK && s);
    assert(audio_stream_configure(s,&format)==K_OK && depth==caller_depth);
    assert(audio_stream_period_bytes(s)==512 && audio_stream_period_page_count(s,0)==1);
    unsigned char input[512],output[512];
    for(unsigned i=0;i<512;++i)input[i]=(unsigned char)(i*17);
    assert(audio_stream_period_write(s,0,input,512)==K_OK);
    assert(audio_stream_period_read(s,0,output,512)==K_OK);
    for(unsigned i=0;i<512;++i)assert(input[i]==output[i]);
    assert(audio_stream_queue(s,0,128)==K_OK && audio_stream_queue(s,1,128)==K_OK);
    assert(audio_stream_queue(s,0,128)==K_EBUSY);
    /* 播放中的周期不能在返回繁忙之前先被改写。 */
    unsigned char replacement[512];memset(replacement,0x5a,sizeof(replacement));
    assert(audio_stream_period_write(s,0,replacement,sizeof(replacement))==K_EBUSY);
    assert(audio_stream_period_read(s,0,output,512)==K_OK);
    assert(!memcmp(input,output,sizeof(input)));

    assert(audio_stream_start(s)==K_OK && audio_stream_configure(s,&format)==K_EBUSY);
    assert(audio_stream_complete(s,0,129)==K_EINVAL && audio_stream_complete(s,2,128)==K_EBUSY);
    assert(audio_stream_complete(s,0,0)==K_OK && audio_stream_complete(s,1,128)==K_OK);
    assert(s->state==AUDIO_STREAM_XRUN && s->underruns==1 && s->completed_frames==256);
    assert(audio_stream_recover(s)==K_EAGAIN);
    assert(audio_stream_queue(s,2,64)==K_OK && audio_stream_recover(s)==K_OK);
    assert(audio_stream_complete(s,2,64)==K_OK && audio_stream_controller_reset(s)==K_OK);
    assert(audio_stream_stop(s)==K_OK && audio_stream_disconnect(s)==K_OK);
    assert(audio_stream_disconnect(s)==K_OK && audio_stream_queue(s,0,1)==K_EDEVREMOVED);
    assert(depth==caller_depth);audio_stream_destroy(s);assert(depth==caller_depth);
}

static atomic_uint submit_ready;
static atomic_bool submit_go;
struct submitter {audio_stream_t *stream;unsigned id;kstatus_t status;};
static void *submit_worker(void *argument) {
    struct submitter *w=argument;unsigned char payload[512];
    memset(payload,(int)w->id,sizeof(payload));
    atomic_fetch_add(&submit_ready,1);
    while(!atomic_load(&submit_go))sched_yield();
    w->status=audio_stream_submit(w->stream,0,payload,sizeof(payload),128,0);
    assert(!depth);return 0;
}
static void submit_races(void) {
    for(unsigned round=0;round<50;++round) {
        audio_stream_t *s=0;pthread_t workers[8];struct submitter args[8];
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
        assert(audio_stream_configure(s,&format)==K_OK);
        atomic_store(&submit_ready,0);atomic_store(&submit_go,false);
        for(unsigned i=0;i<8;++i) {
            args[i]=(struct submitter){s,i+1,K_EIO};
            assert(!pthread_create(workers+i,0,submit_worker,args+i));
        }
        while(atomic_load(&submit_ready)<8)sched_yield();
        atomic_store(&submit_go,true);unsigned winner=0;
        for(unsigned i=0;i<8;++i) {
            assert(!pthread_join(workers[i],0));
            if(args[i].status==K_OK){assert(!winner);winner=args[i].id;}
            else assert(args[i].status==K_EBUSY);
        }
        assert(winner && s->queued_total==1 && s->queued_frames==128);
        unsigned char output[512],payload[512];memset(payload,0xd7,sizeof(payload));
        assert(audio_stream_period_read(s,0,output,512)==K_OK);
        for(unsigned i=0;i<512;++i)assert(output[i]==winner);
        assert(audio_stream_submit(s,1,payload,511,128,0)==K_EINVAL);
        assert(audio_stream_submit(s,4,payload,512,128,0)==K_EINVAL);
        assert(audio_stream_submit(s,1,payload,512,129,0)==K_EINVAL);
        assert(audio_stream_start(s)==K_OK);
        assert(audio_stream_submit(s,0,payload,512,128,0)==K_EBUSY);
        assert(audio_stream_complete(s,0,128)==K_OK);
        assert(audio_stream_submit(s,0,payload,512,128,0)==K_OK);
        assert(audio_stream_recover(s)==K_OK);
        assert(audio_stream_complete(s,0,128)==K_OK);
        assert(audio_stream_disconnect(s)==K_OK);
        assert(audio_stream_submit(s,0,payload,512,128,0)==K_EDEVREMOVED);
        audio_stream_destroy(s);clean();
    }
    /* 采集 DMA 回调必须仍能写回活动周期，不能套用播放所有权约束。 */
    audio_stream_t *s=0;unsigned char payload[512],output[512];
    memset(payload,0x71,sizeof(payload));
    assert(audio_stream_create(&device,AUDIO_CAPTURE,&s)==K_OK);
    assert(audio_stream_configure(s,&format)==K_OK);
    assert(audio_stream_submit(s,0,payload,512,128,0)==K_EINVAL);
    assert(audio_stream_queue(s,0,128)==K_OK && audio_stream_start(s)==K_OK);
    assert(audio_stream_period_write(s,0,payload,512)==K_OK);
    assert(audio_stream_complete(s,0,128)==K_OK);
    assert(audio_stream_period_read(s,0,output,512)==K_OK);
    assert(!memcmp(payload,output,512));audio_stream_destroy(s);clean();
}
static void *worker(void *unused) {(void)unused;for(unsigned i=0;i<100;++i)cycle();assert(!depth);return 0;}
int main(void) {
    refcount_init(&device.object.refs,1);pthread_t threads[8];parallel=true;
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));parallel=false;
    assert(atomic_load(&attempts)==8);
    for(unsigned i=0;i<10000;++i)assert(audio_core_init());
    assert(atomic_load(&attempts)==8);clean();
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();caller_depth=nesting;
        cycle();clean();
        audio_stream_t *unchanged=(void *)(uintptr_t)1;heap_budget=0;
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&unchanged)==K_ENOMEM);
        assert(unchanged==(void *)(uintptr_t)1);heap_budget=-1;clean();
        for(unsigned layout=0;layout<2;++layout) {
        format.period_frames=layout?4096:128;
        for(unsigned kind=0;kind<3;++kind)
        for(unsigned fail=0;fail<((kind==1 && layout)?16U:4U);++fail) {
            audio_stream_t *s=0;assert(audio_stream_create(&device,AUDIO_CAPTURE,&s)==K_OK);
            if(kind==0)heap_budget=fail;
            if(kind==1)page_budget=fail;
            if(kind==2)map_budget=fail;
            assert(audio_stream_configure(s,&format)==(kind==2?K_EIO:K_ENOMEM));
            heap_budget=page_budget=map_budget=-1;
            assert(s->state==AUDIO_STREAM_NEW && depth==nesting);
            assert(!atomic_load(&page_count) && !atomic_load(&mappings));
            audio_stream_destroy(s);clean();
        }
        }
        format.period_frames=128;
        audio_stream_t *s=0;assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
        assert(audio_stream_configure(s,&format)==K_OK);unsigned retained=atomic_load(&heaps);
        fail_unmap=true;
        assert(audio_stream_disconnect(s)==K_EIO && s->state==AUDIO_STREAM_CONFIGURED);
        assert(audio_stream_configure(s,&format)==K_EIO && depth==nesting);
        assert(atomic_load(&heaps)==retained && atomic_load(&page_count)==4 && atomic_load(&mappings)==4);
        fail_unmap=false;assert(audio_stream_disconnect(s)==K_OK);audio_stream_destroy(s);clean();
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();caller_depth=0;assert(!depth);
    }
    /* 后端停止失败时，析构不能释放硬件仍可能引用的周期或流对象。 */
    for(unsigned backend=0;backend<2;++backend) {
        audio_stream_t *s=0;
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
        assert(audio_stream_configure(s,&format)==K_OK);
        assert(audio_stream_queue(s,0,128)==K_OK && audio_stream_start(s)==K_OK);
        unsigned retained=atomic_load(&heaps),refs=atomic_load(&device.object.refs.value);
        failed_stream=s;
        usb_disconnect_status=backend?K_ENOENT:K_EIO;
        hda_disconnect_status=backend?K_EIO:K_ENOENT;
        audio_stream_destroy(s);
        assert(atomic_load(&heaps)==retained && atomic_load(&page_count)==4);
        assert(atomic_load(&mappings)==4 && atomic_load(&device.object.refs.value)==refs);
        assert(s->state==AUDIO_STREAM_RUNNING);
        assert(g_audio_retired==s && !s->retired_next);
        /* 持续故障每次打开只重试一次，且不妨碍其他流创建。 */
        audio_stream_t *fresh=0;
        unsigned before=atomic_load(&callbacks);
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&fresh)==K_OK);
        assert(atomic_load(&callbacks)==before+2 && g_audio_retired==s);
        audio_stream_destroy(fresh);
        assert(atomic_load(&heaps)==retained && atomic_load(&mappings)==4);
        /* 后端恢复后，下次正常打开必须自动释放旧流及所有 DMA 引用。 */
        usb_disconnect_status=hda_disconnect_status=K_ENOENT;
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&fresh)==K_OK);
        assert(!g_audio_retired && atomic_load(&heaps)==1);
        assert(!atomic_load(&page_count) && !atomic_load(&mappings));
        audio_stream_destroy(fresh);failed_stream=0;clean();
    }
    usb_disconnect_status=hda_disconnect_status=K_OK;
    /* 多个失败映射同时退休，恢复后由并发打开各自摘取，不能重复释放或丢失。 */
    fail_unmap=true;
    for(unsigned i=0;i<4;++i) {
        audio_stream_t *s=0;
        assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
        assert(audio_stream_configure(s,&format)==K_OK);
        audio_stream_destroy(s);
    }
    assert(atomic_load(&page_count)==16 && atomic_load(&mappings)==16);
    fail_unmap=false;
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(!g_audio_retired);clean();
    submit_races();
    puts("Audio lifecycle: 50 eight-writer submission races, active playback ownership and capture writeback; eight initializers, zero repeated CAS, 32 nested depths, every period heap/page/DMA allocation failure, backend-stop and unmap retry recovery, concurrent retirement drain, unlocked backend callbacks and 400 concurrent complete lifecycles passed");
}
'''
# 直接执行真实 QUEUE 分支，故障由硬件后端钩子注入。
control_source = (root / 'kernel/syscall/audio.c').read_text()
queue_case = control_source[control_source.rindex('    case OS_AUDIO_CONTROL_QUEUE:'):
                            control_source.rindex('    case OS_AUDIO_CONTROL_COMPLETE:')]
fixture = r'''
#include <uapi/audio.h>
static kstatus_t backend_queue_status;
void *kmalloc(size_t n,uint32_t flags) {
 assert(!flags && depth==caller_depth);
 void *p=malloc(n);assert(p);atomic_fetch_add(&heaps,1);return p;
}
static kstatus_t copy_from_user(void *out,const void *in,size_t n) {
 memcpy(out,in,n);return K_OK;
}
static void audio_settings_apply_gain(void *data,uint64_t bytes,uint16_t format) {
 (void)data;(void)bytes;(void)format;
}
kstatus_t xhci_audio_stream_queue(audio_stream_t *stream,uint32_t period,uint32_t frames) {
 assert(stream && period<4 && frames);return backend_queue_status;
}
static kstatus_t queue_via_uapi(audio_stream_t *stream,const void *data,uint32_t period,uint32_t frames) {
 os_audio_control_t arguments={.period=period,.frames=frames,
     .buffer=(uint64_t)data,.buffer_size=(uint64_t)frames*4};
 kstatus_t status=K_OK;uint64_t expected=0,submission=0;void *temporary=0;
 switch(OS_AUDIO_CONTROL_QUEUE) {
''' + queue_case + r'''
 }
 return status;
}
static void queue_rollback_tests(void) {
 audio_stream_t *s=0;unsigned char payload[512];memset(payload,0x5c,sizeof(payload));
 assert(audio_stream_create(&device,AUDIO_PLAYBACK,&s)==K_OK);
 assert(audio_stream_configure(s,&format)==K_OK);
 int failures[]={K_EBUSY,K_EIO,K_EINVAL};
 for(unsigned i=0;i<3;++i) {
  backend_queue_status=failures[i];
  assert(queue_via_uapi(s,payload,0,128)==failures[i]);
  assert(s->queued_count==0 && s->queued_frames==0 && s->queued_total==0);
  assert(s->active_period==AUDIO_PERIOD_INVALID && s->periods[0].state==AUDIO_PERIOD_FREE);
 }
 backend_queue_status=K_OK;
 assert(queue_via_uapi(s,payload,0,128)==K_OK);
 uint64_t old=s->periods[0].submission;
 assert(audio_stream_controller_reset(s)==K_OK);
 uint64_t fresh=0;
 assert(audio_stream_submit(s,0,payload,512,128,&fresh)==K_OK && fresh!=old);
 assert(audio_stream_cancel_submit(s,0,old)==K_EAGAIN);
 assert(s->queued_count==1 && s->periods[0].state==AUDIO_PERIOD_ACTIVE);
 assert(audio_stream_cancel_submit(s,0,fresh)==K_OK);
 assert(audio_stream_cancel_submit(s,0,fresh)==K_EAGAIN && s->queued_count==0);
 /* 非活动周期取消不能影响已提交的活动周期。 */
 assert(audio_stream_submit(s,0,payload,512,128,&old)==K_OK);
 assert(audio_stream_submit(s,1,payload,256,64,&fresh)==K_OK);
 assert(audio_stream_cancel_submit(s,1,fresh)==K_OK);
 assert(s->active_period==0 && s->queued_count==1 && s->queued_frames==128);
 assert(audio_stream_start(s)==K_OK);
 assert(audio_stream_complete(s,0,128)==K_OK);
 assert(audio_stream_cancel_submit(s,0,old)==K_EAGAIN);
 assert(audio_stream_submit(s,0,payload,512,128,&fresh)==K_OK);
 assert(audio_stream_recover(s)==K_OK);
 assert(audio_stream_cancel_submit(s,0,fresh)==K_OK);
 assert(s->state==AUDIO_STREAM_XRUN && !s->queued_count);
 s->submission_sequence=UINT64_MAX;
 unsigned char before[512];assert(audio_stream_period_read(s,0,before,512)==K_OK);
 memset(payload,0xa3,sizeof(payload));
 assert(audio_stream_submit(s,0,payload,512,128,&fresh)==K_EOVERFLOW);
 unsigned char after[512];assert(audio_stream_period_read(s,0,after,512)==K_OK);
 assert(!memcmp(before,after,512));
 audio_stream_destroy(s);clean();
}
'''
code = code.replace('int main(void) {', fixture + '\nint main(void) {')
code = code.replace('    submit_races();', '    submit_races();\n    queue_rollback_tests();')
with tempfile.TemporaryDirectory(prefix='liteos-audio-lifecycle-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections',
                    '-Wl,--gc-sections','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
