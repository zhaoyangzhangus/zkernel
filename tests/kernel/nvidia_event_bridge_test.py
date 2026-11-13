"""Execute actual RM callbacks/registration with deterministic close overlap."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
begin = source.index('/* Static zero is the unlocked atomic spinlock state.')
end = source.index('static nvidia_stack_t *nv_rm_vendor_frontend_stack_create(', begin)
callbacks = source[begin:end]
close = source[source.index('void nv_rm_vendor_frontend_close('):
               source.index('/*\n * NVKMS RM bridge')]
assert close.index('nv_rm_vendor_event_unregister(file)') < close.index('rm_cleanup_file_private(')
assert close.index('rm_cleanup_file_private(') < close.index('object_put(') < close.index('kfree(file)')
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sched.h>
#define NV_API_CALL
#define NV_OK 0
#define NV_ERR_GENERIC 1
#define NV_ERR_INVALID_ARGUMENT 2
#define K_OK 0
#define K_ENOENT -2
#define K_EBADF -9
#define K_ENOMEM -12
#define NV_PORT_EVENT_READ 1U
typedef unsigned NvU32,NvHandle;
typedef uint16_t NvU16;
typedef int NvS32,NvBool,kstatus_t;
typedef struct {unsigned token;} nv_file_private_t;
typedef struct {NvHandle hParent,hObject;NvU32 index,info32;NvU16 info16;
 nv_file_private_t *nvfp;NvU32 fd;NvS32 active;unsigned refcount;void *next;} nv_event_t;
typedef struct {uint32_t parent,object,index,info32;uint16_t info16;} nv_port_event_record_t;
typedef struct {nv_port_event_record_t records[16];unsigned count,notifications;bool dataless,closed;unsigned refs,gets,puts;} nv_port_event_source_t;
typedef struct nv_rm_vendor_frontend_file {nv_file_private_t vendor_private;
 nv_port_event_source_t *event_source;struct nv_rm_vendor_frontend_file *event_next;} nv_rm_vendor_frontend_file_t;
typedef pthread_mutex_t nv_port_spinlock_t;
static _Thread_local nv_port_spinlock_t *held;
static atomic_bool stall,arrived,release_post,remove_started,removed;
static void (*take_hook)(void);
static bool fail_post;
static nv_port_event_source_t *stalled_source;
static uint64_t nv_port_spin_lock_irqsave(nv_port_spinlock_t *lock) {
 assert(!held&&!pthread_mutex_lock(lock));held=lock;return 5070;
}
static void nv_port_spin_unlock_irqrestore(nv_port_spinlock_t *lock,uint64_t flags) {
 assert(held==lock&&flags==5070);held=NULL;assert(!pthread_mutex_unlock(lock));
}
static int nv_port_event_source_post_irq(nv_port_event_source_t *s,const nv_port_event_record_t *r) {
 assert(held&&s&&!s->closed);
 if(fail_post)return K_ENOMEM;
 if(s==stalled_source&&atomic_load(&stall)){atomic_store(&arrived,true);while(!atomic_load(&release_post))sched_yield();}
 if(r){assert(s->count<16);s->records[s->count++]=*r;}else s->dataless=true;
 ++s->notifications;return 0;
}
static int nv_port_event_source_take(nv_port_event_source_t *s,nv_port_event_record_t *r,bool *more) {
 assert(!held&&s&&s->refs>=2);if(take_hook)take_hook();assert(s->refs);
 if(s->closed)return K_EBADF;if(!s->count)return K_ENOENT;
 *r=s->records[0];for(unsigned i=1;i<s->count;++i)s->records[i-1]=s->records[i];
 --s->count;*more=s->count!=0;return 0;
}
static uint32_t nv_port_event_source_poll(nv_port_event_source_t *s,bool consume) {
 if(!s||s->closed)return 2;unsigned state=s->count||s->dataless?1:0;
 if(consume)s->dataless=false;return state;
}
static void *nv_port_event_source_wait_object(nv_port_event_source_t *s){return s;}
static void object_get(void *p){nv_port_event_source_t *s=p;assert(held&&s->refs);++s->refs;++s->gets;}
static void object_put(void *p){nv_port_event_source_t *s=p;assert(!held&&s->refs);--s->refs;++s->puts;}
''' + callbacks + r'''
static void *posting(void *event){nv_post_event(event,567,123,789,456,1);return NULL;}
static void *removing(void *file){atomic_store(&remove_started,true);nv_rm_vendor_event_unregister(file);atomic_store(&removed,true);return NULL;}
static nv_rm_vendor_frontend_file_t *closing_file;
static void close_after_read_pin(void) {
    assert(!held&&closing_file->event_source->refs==2);
    nv_rm_vendor_event_unregister(closing_file);
    closing_file->event_source->closed=true;
    object_put(closing_file->event_source); /* drop owner, read still pins source */
    assert(closing_file->event_source->refs==1);
    take_hook=NULL;
}
int main(void) {
 for(unsigned i=0;i<NV_RM_EVENT_BUCKETS;++i)
     assert(!pthread_mutex_init(&g_nv_rm_event_buckets[i].lock,NULL));
 nv_port_event_source_t sa={.refs=1},sb={.refs=1};
 nv_rm_vendor_frontend_file_t a={.event_source=&sa},b={.event_source=&sb};
 nv_rm_vendor_event_register(&a);nv_rm_vendor_event_register(&b);
 nv_event_t event={.hParent=42,.hObject=999,.nvfp=&a.vendor_private,.active=1,.fd=88,.refcount=7};
 fail_post=true;nv_post_event(&event,12,34,56,78,1);fail_post=false;
 assert(!sa.count&&!sa.notifications&&atomic_load(&g_nv_rm_event_drops)==1&&!atomic_load(&g_nv_rm_event_posts));
 nv_post_event(&event,12,34,56,78,1);
 nv_post_event(&event,13,35,57,79,1);
 assert(sa.count==2&&sa.notifications==2&&!sb.notifications);
 assert(atomic_load(&g_nv_rm_event_posts)==2&&atomic_load(&g_nv_rm_event_drops)==1);
 nv_event_t out={.nvfp=(void*)1,.next=(void*)2,.active=9};unsigned pending=99;
 unsigned refs_before=sa.gets;
 assert(nv_get_event(NULL,&out,&pending)==NV_ERR_INVALID_ARGUMENT&&!pending);
 pending=99;
 assert(nv_get_event(&a.vendor_private,NULL,&pending)==NV_ERR_INVALID_ARGUMENT&&!pending);
 assert(nv_get_event(&a.vendor_private,&out,NULL)==NV_ERR_INVALID_ARGUMENT);
 assert(sa.count==2&&sa.gets==refs_before&&out.nvfp==(void*)1&&out.next==(void*)2);
 assert(nv_get_event(&a.vendor_private,&out,&pending)==NV_OK&&pending==1);
 assert(out.hParent==42&&out.hObject==12&&out.index==34&&out.info32==56&&out.info16==78);
 assert(!out.nvfp&&!out.next&&!out.active&&!out.refcount&&!out.fd);
 assert(nv_get_event(&a.vendor_private,&out,&pending)==NV_OK&&!pending&&out.hObject==13);
 assert(nv_get_event(&a.vendor_private,&out,&pending)==NV_ERR_GENERIC&&!pending);
 nv_post_event(&event,0,0,0,0,0);
 assert(nv_rm_vendor_frontend_poll_state(&a,false)==1&&sa.dataless);
 assert(nv_rm_vendor_frontend_poll_state(&a,true)==1&&!sa.dataless);
 assert(nv_rm_vendor_frontend_wait_object(&a)==&sa);
 event.active=0;unsigned n=sa.notifications;nv_post_event(&event,1,2,3,4,1);assert(sa.notifications==n);
 event.active=1;nv_rm_vendor_event_unregister(&a);
 nv_post_event(&event,1,2,3,4,1);assert(sa.notifications==n);
 nv_get_event(&a.vendor_private,&out,&pending);assert(!pending);
 event.nvfp=(void*)(uintptr_t)1;nv_post_event(&event,1,2,3,4,1); /* retired pointer never dereferenced */
 event.nvfp=&a.vendor_private;nv_rm_vendor_event_register(&a);
 stalled_source=&sa;atomic_store(&stall,true);pthread_t post,remove;
 assert(!pthread_create(&post,NULL,posting,&event));
 while(!atomic_load(&arrived))sched_yield();
 nv_rm_vendor_frontend_file_t independent[256]={0};
 nv_port_event_source_t independent_source={.refs=1};
 unsigned chosen=0;
 while(chosen<256&&nv_rm_vendor_event_bucket(&independent[chosen].vendor_private)==nv_rm_vendor_event_bucket(&a.vendor_private))++chosen;
 assert(chosen<256);independent[chosen].event_source=&independent_source;
 nv_rm_vendor_event_register(&independent[chosen]);
 nv_event_t other={.nvfp=&independent[chosen].vendor_private,.active=1};
 nv_post_event(&other,987,65,43,21,1);
 assert(independent_source.count==1&&independent_source.notifications==1);
 nv_rm_vendor_event_unregister(&independent[chosen]); /* unrelated bucket stays live */
 assert(!pthread_create(&remove,NULL,removing,&a));
 while(!atomic_load(&remove_started))sched_yield();
 assert(!atomic_load(&removed));
 atomic_store(&release_post,true);
 assert(!pthread_join(post,NULL)&&!pthread_join(remove,NULL)&&atomic_load(&removed));
 sa.closed=true;n=sa.notifications;nv_post_event(&event,1,2,3,4,1);assert(sa.notifications==n);
 assert(nv_rm_vendor_frontend_poll_state(&a,false)==2);
 /* 清理后地址被重用：已失效的旧事件不能指向新状态。 */
 a.event_source=&sb;nv_rm_vendor_event_register(&a);event.active=0;
 nv_post_event(&event,1,2,3,4,1);assert(!sb.notifications);
 nv_rm_vendor_event_unregister(&a);nv_rm_vendor_event_unregister(&b);
 nv_post_event(NULL,0,0,0,0,0);
 assert(nv_get_event(NULL,&out,&pending)==NV_ERR_INVALID_ARGUMENT);
 assert(nv_rm_vendor_frontend_poll_state(NULL,false)==2&&!nv_rm_vendor_frontend_wait_object(NULL));
 assert(sa.refs==1&&sa.gets==sa.puts&&sb.refs==1&&sb.gets==sb.puts);
 nv_port_event_source_t sc={.refs=1};
 nv_rm_vendor_frontend_file_t c={.event_source=&sc};
 nv_rm_vendor_event_register(&c);closing_file=&c;take_hook=close_after_read_pin;
 pending=99;
 assert(nv_get_event(&c.vendor_private,&out,&pending)==NV_ERR_GENERIC&&!pending);
 assert(sc.refs==0&&sc.gets==1&&sc.puts==2);
 /* 覆盖冲突和所有已注册文件匹配情况，然后分别解除链表中部和头部节点的链接，不能
  * 影响相邻节点。 */
 nv_rm_vendor_frontend_file_t files[256]={0};
 nv_port_event_source_t sources[256]={0};
 unsigned occupancy[NV_RM_EVENT_BUCKETS]={0};
 for(unsigned i=0;i<256;++i) {
     sources[i].refs=1;files[i].event_source=&sources[i];
     nv_rm_vendor_event_register(&files[i]);
     ++occupancy[nv_rm_vendor_event_bucket(&files[i].vendor_private)-g_nv_rm_event_buckets];
 }
 unsigned populated=0,collisions=0;
 for(unsigned i=0;i<NV_RM_EVENT_BUCKETS;++i){populated+=occupancy[i]!=0;collisions+=occupancy[i]>1;}
 assert(populated>1&&collisions>0);
 for(unsigned i=0;i<256;++i) {
     nv_event_t e={.nvfp=&files[i].vendor_private,.active=1,.hParent=i};
     nv_post_event(&e,i+1000,i,0,0,1);
 }
 for(unsigned i=0;i<256;++i) {
     assert(nv_get_event(&files[i].vendor_private,&out,&pending)==NV_OK&&!pending);
     assert(out.hParent==i&&out.hObject==i+1000&&out.index==i&&sources[i].notifications==1);
 }
 for(unsigned parity=0;parity<2;++parity)
     for(unsigned i=parity;i<256;i+=2)nv_rm_vendor_event_unregister(&files[i]);
 for(unsigned i=0;i<NV_RM_EVENT_BUCKETS;++i) {
     assert(!g_nv_rm_event_buckets[i].files);
     assert(!pthread_mutex_destroy(&g_nv_rm_event_buckets[i].lock));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-event-bridge-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('RM event bridge: actual callbacks, payload translation, pending, inactive/retired pointers, registration/removal overlap and source lifetime ordering passed')
