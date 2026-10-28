#define _GNU_SOURCE
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <uapi/linux_abi.h>
#include <uapi/window.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "video_pacing.h"

/* 严格使用硬件解码。初始呈现路径会下载 NV12 并在 CPU 上转换为 BGRA；不能将其描述为零拷贝播放。 */
static enum AVPixelFormat hardware_format(AVCodecContext *ctx,const enum AVPixelFormat *formats) {
    (void)ctx;
    for(;*formats!=AV_PIX_FMT_NONE;++formats)if(*formats==AV_PIX_FMT_VULKAN)return *formats;
    fprintf(stderr,"VIDEO_FAIL Vulkan decode format unavailable; refusing software fallback\n");
    return AV_PIX_FMT_NONE;
}
int video_device_create(AVBufferRef **);
static int download_frame(AVFrame *cpu, const AVFrame *frame) {
    /* 仅当 dst->buf[0] 为空时，av_hwframe_transfer_data 才会分配内存。
     * 播放器拒绝分辨率变化，且没有消费者保留 cpu 指针，因此可在播放和 seek 循环中复用可写存储。 */
    if (cpu->buf[0] != NULL &&
        (cpu->width != frame->width || cpu->height != frame->height))
        return AVERROR(EINVAL);
    return av_hwframe_transfer_data(cpu, frame, 0);
}
static int handle_window_event(const os_window_event_t *event);
static int window_closed(int fd,int timeout) {
    struct pollfd p={.fd=fd,.events=POLLIN};
    int result=poll(&p,1,timeout);
    if(result<0)return errno==EINTR ? 0 : -1;
    if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return 1;
    if(p.revents&POLLIN) {
        os_window_event_t event;
        ssize_t n=read(fd,&event,sizeof(event));
        if(n==0)return 1;
        if(n>0&&n!=(ssize_t)sizeof(event))return -1;
        if(n==(ssize_t)sizeof(event)&&event.type==OS_WINDOW_EVENT_CLOSE_REQUEST)return 1;
        if(n==(ssize_t)sizeof(event)&&event.type==OS_WINDOW_EVENT_RESIZE)
            return handle_window_event(&event);
        if(n<0&&errno!=EAGAIN&&errno!=EINTR)return -1;
    }
    return 0;
}
static int monotonic_ns(uint64_t *now) {
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time))return -1;
    *now=(uint64_t)time.tv_sec*UINT64_C(1000000000)+(uint64_t)time.tv_nsec;
    return 0;
}
static int wait_next_frame(int fd,video_pacing_t *pacing) {
    uint64_t now;
    if(monotonic_ns(&now))return -1;
    uint64_t deadline=video_pacing_advance(pacing,now);
    for(;;) {
        if(monotonic_ns(&now))return -1;
        int timeout=video_pacing_timeout(deadline,now);
        if(!timeout)return 0;
        int closed=window_closed(fd,timeout);
        if(closed)return closed;
        /* 鼠标/键盘事件和 EINTR 不会改变呈现截止时间。 */
    }
}
static linux_window_create_t *video_window;
static linux_window_command_t *video_present;
static size_t *video_bytes;
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    int log=open(argv[2],O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(log<0)return 2;
    if(dup2(log,1)<0||dup2(log,2)<0){close(log);return 2;}
    if(log>2)close(log);
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    if(setenv("VK_DRIVER_FILES","/usr/apps/vulkan-video/vulkan/nvidia_icd.json",1))return 1;
    if(setenv("__EGL_VENDOR_LIBRARY_FILENAMES","/usr/apps/vulkan-video/egl/10_nvidia.json",1))return 1;
    puts("VIDEO_START backend=Vulkan no_software_fallback=1");
    AVFormatContext *input=NULL;
    AVCodecContext *decoder=NULL;
    AVBufferRef *hw=NULL;
    AVFrame *frame=av_frame_alloc(),*cpu=av_frame_alloc();
    AVPacket *packet=av_packet_alloc();
    video_pacing_t pacing;
    struct SwsContext *scale=NULL;
    int status=1,fd=-1,stream=-1,err=0;
    uint64_t frames=0,loops=0;
    uint64_t download_buffers=0;
    uint8_t *download_storage=NULL;
    linux_window_create_t w={.x=160,.y=140,.width=256,.height=256,
        .flags=OS_WINDOW_RESIZABLE|OS_WINDOW_STANDARD_CAPTION_CONTROLS,
        .address=UINT64_C(0x08000000),.title="视频播放 - Vulkan"};
    if(!frame||!cpu||!packet)goto cleanup;
    if((err=avformat_open_input(&input,argv[1],NULL,NULL))<0)goto cleanup;
    if((err=avformat_find_stream_info(input,NULL))<0)goto cleanup;
    const AVCodec *codec=NULL;
    if((stream=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,&codec,0))<0){err=stream;goto cleanup;}
    if(codec->id!=AV_CODEC_ID_H264){fprintf(stderr,"VIDEO_FAIL currently requires H264\n");goto cleanup;}
    AVRational rate=av_guess_frame_rate(input,input->streams[stream],NULL);
    video_pacing_init(&pacing,rate.num,rate.den);
    printf("VIDEO_PACING rate=%d/%d period_ns=%llu\n",rate.num,rate.den,
           (unsigned long long)pacing.step);
    if((err=video_device_create(&hw))<0)goto cleanup;
    AVHWDeviceContext *hc=(AVHWDeviceContext*)hw->data;
    AVVulkanDeviceContext *vk=hc->hwctx;
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)
        vk->get_proc_addr(vk->inst,"vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties gpu;
    if(!properties)goto cleanup;
    properties(vk->phys_dev,&gpu);
    if(gpu.vendorID!=0x10de||gpu.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        fprintf(stderr,"VIDEO_FAIL not NVIDIA discrete hardware\n");goto cleanup;
    }
    printf("VIDEO_GPU name=%s\n",gpu.deviceName);
    decoder=avcodec_alloc_context3(codec);
    if(!decoder)goto cleanup;
    if((err=avcodec_parameters_to_context(decoder,input->streams[stream]->codecpar))<0)goto cleanup;
    decoder->get_format=hardware_format;
    decoder->thread_count=1;
    decoder->hw_device_ctx=av_buffer_ref(hw);
    if(!decoder->hw_device_ctx)goto cleanup;
    if((err=avcodec_open2(decoder,codec,NULL))<0)goto cleanup;
    if(decoder->width<1||decoder->height<1||decoder->width>1920||decoder->height>1080) {
        fprintf(stderr,"VIDEO_FAIL unsupported video dimensions\n");goto cleanup;
    }
    int zoom=decoder->width<=640&&decoder->height<=360 ? 2 : 1;
    w.width=(uint32_t)decoder->width*zoom;
    w.height=(uint32_t)decoder->height*zoom;
    size_t bytes=(size_t)w.width*w.height*4;
    long created=syscall((long)LINUX_SYS_WINDOW_CREATE,&w);
    if(created<0||created>INT32_MAX)goto cleanup;
    fd=(int)created;
    if(w.buffer_size<bytes||!w.address)goto cleanup;
    linux_window_command_t present={.operation=LINUX_WINDOW_OPERATION_PRESENT,
        .width=w.width,.height=w.height,.stride=w.width*4,.address=w.address,.length=bytes};
    linux_window_command_t visible={.operation=LINUX_WINDOW_OPERATION_VISIBILITY,.width=1};
    video_window=&w;video_present=&present;video_bytes=&bytes;
    for(;;) {
        int closed=window_closed(fd,0);
        if(closed){status=closed>0&&frames ? 0 : 1;break;}
        err=av_read_frame(input,packet);
        int eof=err==AVERROR_EOF;
        if(err<0&&!eof)goto cleanup;
        if(!eof&&packet->stream_index!=stream){av_packet_unref(packet);continue;}
        err=avcodec_send_packet(decoder,eof ? NULL : packet);
        av_packet_unref(packet);
        if(err<0)goto cleanup;
        while((err=avcodec_receive_frame(decoder,frame))>=0) {
            if(frame->format!=AV_PIX_FMT_VULKAN) {
                fprintf(stderr,"VIDEO_FAIL software frame rejected\n");goto cleanup;
            }
            if(frame->width!=decoder->width||frame->height!=decoder->height)goto cleanup;
            if((err=download_frame(cpu,frame))<0)goto cleanup;
            if(cpu->data[0]!=download_storage) {
                download_storage=cpu->data[0];
                ++download_buffers;
            }
            scale=sws_getCachedContext(scale,cpu->width,cpu->height,cpu->format,w.width,w.height,AV_PIX_FMT_BGRA,
                SWS_POINT,NULL,NULL,NULL);
            if(!scale)goto cleanup;
            uint8_t *dest[4]={(uint8_t*)(uintptr_t)w.address,NULL,NULL,NULL};
            int stride[4]={(int)w.width*4,0,0,0};
            if(sws_scale(scale,(const uint8_t *const*)cpu->data,cpu->linesize,0,cpu->height,dest,stride)!=(int)w.height)goto cleanup;
            if(!frames) {
                printf("VIDEO_DOWNLOAD format=%s stride_y=%d stride_uv=%d y0=%u y16=%u uv0=%u uv1=%u\n",
                    av_get_pix_fmt_name(cpu->format),cpu->linesize[0],cpu->linesize[1],
                    cpu->data[0][0],cpu->data[0][16],cpu->data[1][0],cpu->data[1][1]);
                int dump=open("/video-first.bgra",O_WRONLY|O_CREAT|O_TRUNC,0644);
                if(dump>=0){ssize_t written=write(dump,dest[0],bytes);if(written!=(ssize_t)bytes||fsync(dump))fprintf(stderr,"VIDEO_DUMP_FAIL\n");close(dump);}
            }
            if(ioctl(fd,(unsigned long)LINUX_WINDOW_IOCTL_COMMIT,&present))goto cleanup;
            if(!frames&&ioctl(fd,(unsigned long)LINUX_WINDOW_IOCTL_COMMIT,&visible))goto cleanup;
            ++frames;
            if(frames==1){puts("VIDEO_FIRST_FRAME hardware_format=Vulkan download=NV12 color_conversion=CPU");fsync(1);}
            av_frame_unref(frame);
            closed=wait_next_frame(fd,&pacing);
            if(closed){status=closed>0 ? 0 : 1;goto cleanup;}
        }
        if(err!=AVERROR(EAGAIN)&&err!=AVERROR_EOF)goto cleanup;
        if(eof) {
            if(!frames)goto cleanup;
            printf("VIDEO_LOOP_PASS loops=%llu frames=%llu decoder=Vulkan download_buffers=%llu\n",
                   (unsigned long long)++loops,(unsigned long long)frames,
                   (unsigned long long)download_buffers);
            fsync(1);
            if((err=av_seek_frame(input,-1,0,AVSEEK_FLAG_BYTE))<0)goto cleanup;
            avcodec_flush_buffers(decoder);
        }
    }
cleanup:
    if(status&&err<0){char message[AV_ERROR_MAX_STRING_SIZE];av_strerror(err,message,sizeof(message));fprintf(stderr,"VIDEO_FAIL error=%s (%d)\n",message,err);}
    if(fd>=0)close(fd);
    sws_freeContext(scale);av_packet_free(&packet);av_frame_free(&frame);av_frame_free(&cpu);
    avcodec_free_context(&decoder);av_buffer_unref(&hw);avformat_close_input(&input);
    printf("VIDEO_EXIT status=%d frames=%llu\n",status,(unsigned long long)frames);
    if(fsync(1))status=1;
    return status;
}

static int handle_window_event(const os_window_event_t *event) {
    uint64_t pixels=(uint64_t)event->resize.width*event->resize.height;
    if(!event->resize.width||!event->resize.height||event->resize.width>INT_MAX/4U||
       pixels>video_window->buffer_size/4U||pixels>event->resize.buffer_size/4U||pixels>SIZE_MAX/4U)
        return -1;
    uint64_t bytes=pixels*4U;
    video_window->width=event->resize.width;
    video_window->height=event->resize.height;
    *video_bytes=(size_t)bytes;
    video_present->width=event->resize.width;
    video_present->height=event->resize.height;
    video_present->stride=event->resize.width*4U;
    video_present->length=bytes;
    printf("VIDEO_WINDOW_RESIZE width=%u height=%u\n",event->resize.width,event->resize.height);
    return 0;
}
