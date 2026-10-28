"""执行真实系统声音控制面：持久化失败、设备选择、主音量与静音。"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/audio_settings.h').read_text().replace('#pragma once','')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <uapi/audio.h>
#include <uapi/syscall.h>
#include <uapi/file.h>
#define __user
#define K_OK 0
#define K_EIO -5
#define K_EINVAL -22
#define K_ENOENT -2
#define DEVICE_REMOVING 3
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_acq_rel __ATOMIC_ACQ_REL
typedef int kstatus_t;
typedef int io_mutex_t;
typedef int file_t;
typedef struct {atomic_uint state;} device_t;
static device_t hda,usb;
static bool usb_present=true,fail_save,has_saved;
static os_audio_settings_t saved,temporary;
static uint32_t xhci_audio_device_count(void){return usb_present?2U:0U;}
static device_t *hda_audio_device(void){return &hda;}
static device_t *xhci_audio_device(void){return usb_present?&usb:0;}
static void io_mutex_init(io_mutex_t *m,bool pi){(void)pi;*m=0;}
static int io_mutex_lock(io_mutex_t *m){assert(!*m);*m=1;return 0;}
static void io_mutex_unlock_required(io_mutex_t *m){assert(*m);*m=0;}
static int copy_from_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static int copy_to_user(void *d,const void *s,size_t n){memcpy(d,s,n);return 0;}
static bool versioned_header_valid(const os_versioned_header_t *h,size_t n){return h->size==n&&h->version==OS_SYSCALL_ABI_VERSION;}
static int vfs_open_kernel(const char *p,uint32_t flags,uint32_t mode,file_t **f){
 (void)mode;static int file;
 if (flags==OS_FILE_OPEN_READ && !has_saved)return -2;
 assert(strstr(p,"/EFI/OS/audio-settings"));*f=&file;return 0;
}
static int vfs_write_kernel(file_t *f,const void *b,size_t n,uint64_t *bytes){
 assert(f&&n==sizeof(saved));if(fail_save)return -5;memcpy(&temporary,b,n);*bytes=n;return 0;
}
static int vfs_read_kernel(file_t *f,void *b,size_t n,uint64_t *bytes){assert(f);memcpy(b,&saved,n);*bytes=n;return 0;}
static int vfs_fsync(file_t *f){assert(f);return 0;}
static void vfs_close(file_t *f){assert(f);}
static int vfs_rename_kernel(const char *a,const char *b){assert(strstr(a,".tmp")&&!strstr(b,".tmp"));saved=temporary;has_saved=true;return 0;}
static int vfs_remove_kernel(const char *p){assert(strstr(p,".tmp"));return 0;}
'''+source+r'''
static int request(os_audio_settings_t *s){return syscall_audio_settings((uintptr_t)s,0,0,0,0,0);}
int main(void){
 os_audio_settings_t s={.hdr={sizeof(s),OS_SYSCALL_ABI_VERSION}};
 assert(!audio_settings_ensure_loaded());
 assert(!request(&s)&&s.volume==100&&s.effective_backend==1&&s.available==14);
 s.operation=1;s.backend=3;s.volume=50;assert(!request(&s)&&s.backend==3&&s.effective_backend==3&&s.generation==1);
 s.backend=2;assert(!request(&s)&&s.backend==2&&s.generation==2);
 int16_t pcm[]={-32768,-200,0,200,32767};
 audio_settings_apply_gain(pcm,sizeof(pcm),OS_AUDIO_SAMPLE_S16_LE);
 assert(pcm[0]==-16384&&pcm[1]==-100&&pcm[2]==0&&pcm[3]==100&&pcm[4]==16383);
 int32_t wide[]={INT32_MIN,INT32_MAX};
 audio_settings_apply_gain(wide,sizeof(wide),OS_AUDIO_SAMPLE_S32_LE);
 assert(wide[0]==-1073741824&&wide[1]==1073741823);
 uint8_t packed[]={0,0,0x80,0xff,0xff,0x7f};
 audio_settings_apply_gain(packed,sizeof(packed),OS_AUDIO_SAMPLE_S24_LE);
 assert(packed[2]==0xc0&&packed[3]==0xff&&packed[4]==0xff&&packed[5]==0x3f);
 s.volume=101;assert(request(&s)==-22);s.volume=0;
 fail_save=true;assert(request(&s)==-5);fail_save=false;
 s.operation=0;assert(!request(&s)&&s.volume==50);
 s.operation=1;s.muted=1;assert(!request(&s));
 audio_settings_apply_gain(pcm,sizeof(pcm),OS_AUDIO_SAMPLE_S16_LE);
 for(unsigned i=0;i<5;i++)assert(!pcm[i]);
 g_audio_settings_loaded=false;atomic_store(&g_audio_settings_word,100U<<8U);
 s.operation=0;assert(!request(&s)&&s.volume==50&&s.muted&&s.backend==2);
 usb_present=false;s.operation=1;assert(request(&s)==-2);
 s.backend=0;assert(!request(&s)&&s.effective_backend==1);
 atomic_store(&hda.state,DEVICE_REMOVING);s.operation=0;
 assert(!request(&s)&&!s.available&&!s.effective_backend);
 puts("Audio settings: device routing, 16/24/32-bit master gain, mute, persistence and failed-write isolation passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=str(Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),'-x','c','-','-o',exe],input=code,text=True,check=True)
 subprocess.run([exe],check=True)
