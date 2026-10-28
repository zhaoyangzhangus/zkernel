#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <uapi/audio.h>
#include <uapi/syscall.h>

static int64_t call(uint64_t number, uint64_t a, uint64_t b) {
    register uint64_t ax __asm__("rax")=number;
    register uint64_t di __asm__("rdi")=a;
    register uint64_t si __asm__("rsi")=b;
    __asm__ volatile("syscall":"+a"(ax),"+D"(di),"+S"(si)::"rcx","r11","memory");
    return (int64_t)ax;
}
static int64_t control(os_handle_t handle, uint32_t code, uint32_t period,
                       const void *data, uint32_t bytes) {
    os_audio_control_t request={.hdr={.size=sizeof(request),.version=OS_SYSCALL_ABI_VERSION},
        .code=code,.period=period,.frames=960U,.buffer=(uintptr_t)data,.buffer_size=bytes};
    return call(OS_SYS_AUDIO_CONTROL,handle,(uintptr_t)&request);
}
int main(int argc, char **argv) {
    uint32_t backend=OS_AUDIO_BACKEND_HDA;
    if (argc>1 && !strcmp(argv[1],"usb")) backend=OS_AUDIO_BACKEND_USB;
    else if (argc>1 && strcmp(argv[1],"hda")) {puts("audio-test hda|usb");return 2;}
    os_audio_open_t open={.hdr={.size=sizeof(open),.version=OS_SYSCALL_ABI_VERSION},
        .config={.hdr={.size=sizeof(os_audio_stream_config_t),.version=OS_SYSCALL_ABI_VERSION},
        .direction=OS_AUDIO_PLAYBACK,.sample_rate=48000U,.channels=2U,
        .sample_format=OS_AUDIO_SAMPLE_S16_LE,.period_frames=960U,.period_count=2U},.backend=backend};
    int64_t status=call(OS_SYS_AUDIO_OPEN,(uintptr_t)&open,0);
    if (status<0) {printf("Audio open failed: %lld\n",(long long)status);return 1;}
    int16_t samples[960U*2U];
    /* 低音量三角波，600 Hz；周期长度为整数波长，避免缓冲区边界跳变。 */
    for (unsigned i=0;i<960U;++i) {
        int phase=(int)(i%80U);
        int16_t value=(int16_t)((phase<40?phase:80-phase)*75-1500);
        samples[i*2U]=value;samples[i*2U+1U]=value;
    }
    uint32_t periods=backend==OS_AUDIO_BACKEND_HDA?2U:1U;
    const char *stage="QUEUE";
    for (uint32_t i=0;i<periods;++i)
        if ((status=control(open.handle,OS_AUDIO_CONTROL_QUEUE,i,samples,sizeof(samples)))<0) goto done;
    stage="START";
    if ((status=control(open.handle,OS_AUDIO_CONTROL_START,0,0,0))<0) goto done;
    puts(backend==OS_AUDIO_BACKEND_HDA?"5070 HDMI/DP: 600 Hz, 2 seconds":"USB audio: 600 Hz, 2 seconds");
    for (uint32_t i=0;i<100U;++i) {
        stage="COMPLETE";
        /* 完成依据硬件 DMA，不按睡眠时间假装已经播放。 */
        struct timespec start,now;
        clock_gettime(CLOCK_MONOTONIC,&start);
        for (;;) {
            status=control(open.handle,OS_AUDIO_CONTROL_COMPLETE,i%periods,0,0);
            if (status!=-11) break;
            usleep(500U);
            clock_gettime(CLOCK_MONOTONIC,&now);
            if ((now.tv_sec-start.tv_sec)*1000000000LL+now.tv_nsec-start.tv_nsec>1000000000LL) {
                status=-110;break;
            }
        }
        if (status<0) goto done;
        stage="QUEUE";
        if ((status=control(open.handle,OS_AUDIO_CONTROL_QUEUE,i%periods,samples,sizeof(samples)))<0) goto done;
        if (periods==1U) {
            stage="RECOVER";
            if ((status=control(open.handle,OS_AUDIO_CONTROL_RECOVER,0,0,0))<0) goto done;
        }
    }
    puts("Audio DMA completed: 96000 frames");
done:
    control(open.handle,OS_AUDIO_CONTROL_STOP,0,0,0);
    call(OS_SYS_HANDLE_CLOSE,open.handle,0);
    if (status<0) printf("Audio %s failed: %lld\n",stage,(long long)status);
    return status<0?1:0;
}
