"""运行真实 USB 音频分包与完成路径，验证空环通知不会伪装成播放失败。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
r=(root/'kernel/drivers/usb/xhci/audio_runtime.c').read_text()
policy=(root/'kernel/drivers/usb/xhci/audio.c').read_text()
def function(source,name):
 start=source.rfind('\n',0,source.index(name+'('))+1
 brace=source.index('{',start);end=brace+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
code='''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/audio.h>
#include "kernel/drivers/usb/xhci/internal.h"
'''+r[r.index('#define XHCI_RING_TRB_COUNT'):r.index('bool xhci_configure_audio_endpoint')]+r'''
static xhci_slot_device_t slot;
static unsigned error;
uint64_t xhci_dma_address(const dma_mapping_t *m){return m->mapped_length;}
static unsigned publication_stage, publications;
void dma_sync_for_device(dma_mapping_t *m){
 xhci_device_context_t *d=&slot.context;
 xhci_trb_t *ring=d->audio_ring.cpu;
 if(m==&d->audio_buffer.mapping) {
  assert(publication_stage==0);publication_stage=1;
 } else {
  assert(m==&d->audio_ring.mapping && d->audio_transfer_pending);
  unsigned first_cycle=d->audio_cycle;
  if(d->audio_enqueue<=d->audio_transfer_first)first_cycle^=1;
  bool visible=(ring[d->audio_transfer_first].control&1)==first_cycle;
  if(publication_stage==1) {assert(!visible);publication_stage=2;}
  else {assert(publication_stage==3 && visible);publication_stage=4;}
 }
}
void dma_sync_for_cpu(dma_mapping_t *m){assert(m);}
void dma_wmb(void){
 if(publication_stage==2)publication_stage=3;
 else {assert(publication_stage==4);publication_stage=0;++publications;}
}
void xhci_set_error(uint32_t e){error=e;}
xhci_slot_device_t *xhci_topology_slot(uint32_t n){return n==4?&slot:0;}
uint64_t audio_stream_bytes_for_frames(const audio_stream_t *s,uint32_t f){assert(s);return f*4U;}
kstatus_t audio_stream_period_read(audio_stream_t *s,uint32_t p,void *b,uint64_t n){assert(s&&p<2);memset(b,0x55,n);return K_OK;}
kstatus_t audio_stream_period_write(audio_stream_t *s,uint32_t p,const void *b,uint64_t n){(void)s;(void)p;(void)b;(void)n;assert(0);return K_EINVAL;}
'''+(root/'kernel/drivers/usb/xhci/transfer.c').read_text()+function(r,'xhci_queue_audio_transfer_device')+r[r.index('bool xhci_handle_audio_transfer_event'):]+r'''
static xhci_state_t *controller;
static bool locked;
xhci_state_t *xhci_controller_state(void) { return controller; }
bool xhci_topology_audio_configured(void) { return true; }
static void xhci_audio_lock(void) { assert(!locked); locked=true; }
static void xhci_audio_unlock(void) { assert(locked); locked=false; }
static xhci_device_context_t *xhci_audio_context(xhci_state_t *s) {
 assert(s && locked); return &slot.context;
}
void audio_stream_get_format(const audio_stream_t *s,audio_format_t *f) {
 assert(s); *f=(audio_format_t){.period_count=2,.period_frames=960};
}
'''+function(policy,'xhci_audio_invalidate_stream')+'kstatus_t\n'+function(policy,'xhci_audio_stream_queue')+'kstatus_t\n'+function(policy,'xhci_audio_stream_start')+'kstatus_t\n'+function(policy,'xhci_audio_stream_stop')+'kstatus_t\n'+function(policy,'xhci_audio_stream_reset')+'kstatus_t\n'+function(policy,'xhci_audio_stream_disconnect')+r'''
int main(void){
 xhci_trb_t ring[256]={0};uint8_t buffer[4096],mmio[4096]={0};
 xhci_state_t state={.initialized=true,.mmio=mmio};controller=&state;
 slot.used=true;xhci_device_context_t *d=&slot.context;
 d->device_slot=4;d->device_speed=3;d->audio_endpoint=5;d->audio_max_packet=248;
 d->audio_interval=1;d->audio_channels=2;d->audio_bit_resolution=16;
 d->audio_ring.cpu=ring;d->audio_ring.mapping.mapped_length=0x1000;
 d->audio_buffer.cpu=buffer;d->audio_buffer.mapping.mapped_length=0x2000;
 d->audio_cycle=1;d->audio_stream=(void *)1;d->audio_stream_queued=true;d->audio_frames=960;
 assert(xhci_queue_audio_transfer_device(&state,d));assert(d->audio_enqueue==160&&d->audio_transfer_pending);
 for(unsigned i=0;i<160;i++){
  assert(ring[i].parameter==0x2000+i*24 && ring[i].status==24);
  assert(!!(ring[i].control&(1U<<5))==(i==159));
 }
 xhci_trb_t event={.status=14U<<24,.control=(32U<<10)|(10U<<16)|(4U<<24)};
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending&&d->audio_stream_queued&&!d->audio_transfer_failed&&!error);
 event.status=1U<<24;event.parameter=0x1000;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending&&d->audio_stream_queued&&d->audio_completed==0);
 event.parameter=0x1000+159*16;assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_pending&&!d->audio_stream_queued&&d->audio_completed==1);
 event.status=14U<<24;assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_failed&&!error&&d->audio_completed==1);
 d->audio_stream_queued=true;d->audio_period=1;
 assert(xhci_queue_audio_transfer_device(&state,d));assert(d->audio_enqueue==65&&d->audio_cycle==0);
 assert((ring[255].control&3U)==3U);
 /* 上一周期延迟到达的成功事件不能释放新周期。 */
 event.status=1U<<24;assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending&&d->audio_stream_queued&&d->audio_completed==1);
 /* 非环地址、未对齐地址及 Event Data 参数都不属于当前提交。 */
 uint64_t invalid[]={0,0x0ff0,0x1001,0x1000+255*16,UINT64_MAX};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
  event.status=23U<<24;event.parameter=invalid[i];
  assert(xhci_handle_audio_transfer_event(&state,&event));
  assert(d->audio_transfer_pending && !d->audio_transfer_failed && !error);
 }
 event.parameter=0x1000+200*16;event.control|=1U<<2;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending && !d->audio_transfer_failed && !error);
 event.control&=~(1U<<2);event.parameter=0x1000+159*16;
 /* 延迟的旧错误和旧成功一样，不能释放当前 DMA 周期。 */
 event.status=23U<<24;assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending && d->audio_stream_queued && !error);
 event.parameter=0x1000+200*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_failed&&error==87);
 /* 中间 TD 出错后，尾包尚未退出，仍不能复用 DMA 缓冲。 */
 assert(d->audio_transfer_pending);
 event.status=1U<<24;event.parameter=0x1000+64*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_pending && d->audio_transfer_failed);
 assert(d->audio_completed==2);
 /* 全速 EarPods：一个 20 ms 周期由 20 个 192 字节包组成。 */
 d->audio_transfer_pending=false;d->audio_stream_queued=true;
 d->device_speed=1;d->audio_max_packet=192;d->audio_enqueue=0;d->audio_cycle=1;
 assert(xhci_queue_audio_transfer_device(&state,d)&&d->audio_enqueue==20);
 for(unsigned i=0;i<20;i++) {
  assert(ring[i].parameter==0x2000+i*192&&ring[i].status==192);
  assert(!!(ring[i].control&(1U<<5))==(i==19));
 }
 event.status=1U<<24;event.parameter=0x1000+19*16;
 assert(xhci_handle_audio_transfer_event(&state,&event)&&!d->audio_transfer_pending&&d->audio_completed==3);
 assert(xhci_handle_audio_transfer_event(&state,&event)&&d->audio_completed==3);
 d->audio_stream_queued=true;d->audio_max_packet=23;
 unsigned before=d->audio_enqueue;
 assert(!xhci_queue_audio_transfer_device(&state,d)&&d->audio_enqueue==before);
 /* 参数预检失败时，不读音频数据，不修改环、缓冲或提交状态。 */
 d->audio_max_packet=192;
 xhci_device_context_t saved=*d;
 xhci_trb_t saved_ring[256];uint8_t saved_buffer[4096];
 memcpy(saved_ring,ring,sizeof(ring));memset(buffer,0xa5,sizeof(buffer));
 memcpy(saved_buffer,buffer,sizeof(buffer));
 for(unsigned invalid=0;invalid<8;++invalid) {
  *d=saved;
  switch(invalid) {
   case 0:d->audio_interval=0;break;
   case 1:d->audio_interval=12;break;
   case 2:d->audio_enqueue=255;break;
   case 3:d->audio_cycle=2;break;
   case 4:d->audio_endpoint=16;break;
   case 5:d->audio_buffer.mapping.mapped_length=0;break;
   case 6:d->audio_buffer.mapping.mapped_length=UINT64_MAX-100;break;
   default:d->audio_max_packet=23;break;
  }
  unsigned enqueue=d->audio_enqueue,cycle=d->audio_cycle;
  assert(!xhci_queue_audio_transfer_device(&state,d));
  assert(!memcmp(saved_ring,ring,sizeof(ring)));
  assert(!memcmp(saved_buffer,buffer,sizeof(buffer)));
  assert(d->audio_enqueue==enqueue && d->audio_cycle==cycle);
  assert(!d->audio_transfer_pending && publication_stage==0);
 }
 *d=saved;
 /* 没有应用持有流时不能提交静默包，关闭前遗留的完成事件也不能自重启。 */
 d->audio_stream=0;d->audio_stream_queued=false;d->audio_max_packet=192;
 d->audio_transfer_pending=false;before=d->audio_enqueue;
 memset(buffer,0xa5,sizeof(buffer));
 assert(!xhci_queue_audio_transfer_device(&state,d));
 assert(d->audio_enqueue==before&&!d->audio_transfer_pending&&buffer[0]==0xa5);
 d->audio_transfer_pending=true;
 event.status=1U<<24;event.parameter=0x1000+19*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_pending&&d->audio_enqueue==before&&d->audio_completed==4);
 /* 旧流 DMA 完成时，新流已入队：先排空旧传输，再发布新周期。 */
 d->audio_stream=(void*)2;d->audio_stream_running=true;
 d->audio_stream_queued=true;d->audio_frames=960;
 d->audio_transfer_pending=true;
 d->audio_transfer_generation=7;d->audio_stream_generation=7;
 assert(xhci_audio_stream_stop(d->audio_stream)==K_OK && !locked);
 assert(d->audio_stream_generation==8 && !d->audio_stream_queued);
 assert(xhci_audio_stream_queue(d->audio_stream,0,960)==K_OK && !locked);
 assert(xhci_audio_stream_queue(d->audio_stream,0,960)==K_EBUSY && !locked);
 assert(xhci_audio_stream_start(d->audio_stream)==K_OK && !locked);
 assert(d->audio_enqueue==20 && d->audio_transfer_generation==7);
 unsigned completed=d->audio_completed;
 event.status=1U<<24;event.parameter=0x1000+19*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_completed==completed+1 && d->audio_transfer_pending);
 assert(d->audio_stream_queued && d->audio_transfer_generation==8);
 assert(d->audio_enqueue==40);
 /* 重复旧事件不能确认新周期。 */
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending && d->audio_stream_queued);
 event.parameter=0x1000+39*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_pending && !d->audio_stream_queued);
 assert(d->audio_completed==completed+2);
 /* 旧流出错也不能使已等待的新流继承错误。 */
 d->audio_transfer_pending=true;d->audio_stream_queued=true;
 d->audio_transfer_generation=8;d->audio_stream_generation=9;
 event.status=23U<<24;event.parameter=0x1000+39*16;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending && d->audio_stream_queued);
 assert(d->audio_transfer_generation==9);
 /* 重置只清除逻辑所有权，不能覆盖仍在途的 DMA。 */
 unsigned generation=d->audio_stream_generation;
 unsigned enqueue=d->audio_enqueue;
 uint64_t tail=0x1000+((d->audio_transfer_first+d->audio_transfer_packets-1)%255)*16;
 assert(xhci_audio_stream_reset(d->audio_stream)==K_OK && !locked);
 assert(d->audio_stream_generation==generation+1);
 assert(!d->audio_stream_running && !d->audio_stream_queued);
 assert(d->audio_period==0 && d->audio_frames==0);
 assert(d->audio_transfer_pending && d->audio_enqueue==enqueue);
 assert(xhci_audio_stream_queue(d->audio_stream,1,960)==K_OK);
 assert(xhci_audio_stream_start(d->audio_stream)==K_OK);
 event.status=1U<<24;event.parameter=tail;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(d->audio_transfer_pending && d->audio_stream_queued);
 assert(d->audio_transfer_generation==generation+1);
 /* 解绑后旧尾包只回收 DMA，不能重启已关闭的流。 */
 generation=d->audio_stream_generation;enqueue=d->audio_enqueue;
 tail=0x1000+((d->audio_transfer_first+d->audio_transfer_packets-1)%255)*16;
 assert(xhci_audio_stream_disconnect(d->audio_stream)==K_OK && !locked);
 assert(!d->audio_stream && !d->audio_stream_running && !d->audio_stream_queued);
 assert(d->audio_stream_generation==generation+1 && d->audio_transfer_pending);
 event.parameter=tail;
 assert(xhci_handle_audio_transfer_event(&state,&event));
 assert(!d->audio_transfer_pending && d->audio_enqueue==enqueue);
 assert(xhci_audio_stream_disconnect((void*)2)==K_ENOENT && !locked);
 assert(xhci_audio_stream_reset(0)==K_EINVAL && !locked);
 /* 不同起点和 cycle 均须正确跨越 Link TRB，最后一次性提交游标。 */
 unsigned starts[]={0,1,64,94,95,234,235,253,254};
 for(unsigned speed=0;speed<2;++speed)
  for(unsigned initial_cycle=0;initial_cycle<2;++initial_cycle)
   for(unsigned n=0;n<sizeof(starts)/sizeof(starts[0]);++n) {
    *d=saved;d->audio_enqueue=starts[n];d->audio_cycle=initial_cycle;
    d->device_speed=speed?3:1;d->audio_max_packet=speed?248:192;
    unsigned packets=speed?160:20,packet_bytes=speed?24:192;
    memset(ring,0,sizeof(ring));
    assert(xhci_queue_audio_transfer_device(&state,d));
    unsigned index=starts[n],cycle=initial_cycle;
    for(unsigned packet=0;packet<packets;++packet) {
     assert((ring[index].control&1)==cycle);
     assert(ring[index].parameter==0x2000+packet*packet_bytes);
     assert(ring[index].status==packet_bytes);
     assert(!!(ring[index].control&32)==(packet+1==packets));
     if(++index==255) {
      assert((ring[255].control&1)==cycle);
      assert((ring[255].control&2)!=0);
      index=0;cycle^=1;
     }
    }
    assert(d->audio_enqueue==index && d->audio_cycle==cycle);
   }
 assert(publications>5 && publication_stage==0);
 puts("USB Audio DMA: HS/FS packets, ordered publication, IOC boundary, cycle wrap, stop/reset/disconnect generation handoff, stale completion and real failure passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=str(Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root),'-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/usb/xhci'),'-x','c','-','-o',exe],input=code,text=True,check=True)
 subprocess.run([exe],check=True)
