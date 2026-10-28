"""重放 5070 的 LPIB==CBL 实测边界，验证完成计数和重复读取。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/audio/hda/core.c').read_text()
start = source.index('kstatus_t hda_audio_stream_complete(')
end = source.index('\nkstatus_t hda_audio_stream_stop(', start)
code = source[:source.index('static hda_controller_t g_hda;')] + r'''
#include <assert.h>
#include <stdio.h>
static hda_controller_t g_hda;
static device_t device;
static audio_stream_t *stream=(void*)1;
static bool locked, running=true;
static uint32_t position, active;
static uint32_t stream_state=AUDIO_STREAM_RUNNING;
struct device *hda_audio_device(void) {return &device;}
device_t *audio_stream_device(const audio_stream_t *s) {assert(s==stream);return &device;}
void audio_stream_get_format(const audio_stream_t *s,audio_format_t *f) {
 assert(s==stream);*f=(audio_format_t){.period_count=2,.period_frames=960};
}
void audio_stream_get_stats(audio_stream_t *s,audio_stream_stats_t *stats) {
 assert(s==stream);*stats=(audio_stream_stats_t){.state=stream_state,.active_period=active};
}
uint64_t audio_stream_period_bytes(const audio_stream_t *s) {assert(s==stream);return 3840;}
static hda_audio_binding_t *hda_binding_for_stream(audio_stream_t *s) {assert(s==stream);return &g_hda.playback;}
static void hda_audio_lock(hda_controller_t *c) {assert(c==&g_hda&&!locked);locked=true;}
static void hda_audio_unlock(hda_controller_t *c) {assert(c==&g_hda&&locked);locked=false;}
static uint32_t hda_read32(const hda_controller_t *c,uint32_t offset) {
 assert(c==&g_hda&&locked);
 if(offset==HDA_SD_CTL)return running?HDA_SD_CTL_RUN:0;
 assert(offset==HDA_SD_LPIB);return position;
}
''' + source[start:end] + r'''
int main(void) {
 g_hda.playback.stream=stream;
 for(unsigned cycle=0;cycle<10000;cycle++) {
  for(unsigned p=0;p<2;p++) {
   active=p;position=(p+1)*3840;
   assert(hda_audio_stream_complete(stream,p,960)==K_OK && !locked);
   assert(g_hda.playback.completed_bytes==((uint64_t)cycle*2+p+1)*3840);
   active=1-p;
   assert(hda_audio_stream_complete(stream,active,960)==K_EAGAIN && !locked);
   if(p==1) {
    position=0;
    assert(hda_audio_stream_complete(stream,active,960)==K_EAGAIN && !locked);
   }
  }
 }
 uint64_t completed=g_hda.playback.completed_bytes,played=g_hda.playback.played_bytes;
 position=7681;assert(hda_audio_stream_complete(stream,0,960)==K_EIO);
 position=UINT32_MAX;assert(hda_audio_stream_complete(stream,0,960)==K_EIO);
 assert(g_hda.playback.completed_bytes==completed&&g_hda.playback.played_bytes==played&&!locked);
 position=3840;assert(hda_audio_stream_complete(stream,0,959)==K_EINVAL);
 assert(hda_audio_stream_complete(stream,1,960)==K_EBUSY);
 running=false;assert(hda_audio_stream_complete(stream,0,960)==K_EBUSY);
 running=true;stream_state=AUDIO_STREAM_CONFIGURED;
 assert(hda_audio_stream_complete(stream,0,960)==K_EBUSY);
 stream_state=AUDIO_STREAM_RUNNING;
 assert(hda_audio_stream_complete(stream,0,0)==K_OK && !locked);
 puts("HDA completion: 20000 periods, exact CBL wrap, duplicate position, invalid MMIO and stream state passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    exe=str(Path(d)/'test')
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c','-','-o',exe],
                   input=code,text=True,check=True)
    subprocess.run([exe],check=True)
