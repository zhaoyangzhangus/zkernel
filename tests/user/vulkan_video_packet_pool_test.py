"""用实际 FFmpeg 缓冲池验证桥接输入的复用、引用隔离和扩容生命周期。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
ffmpeg = root / 'build/nvidia/vulkan/ffmpeg-7.1.1'
source = (root / 'tools/nvidia/vulkan_video_decoder.c').read_text()
send = source[source.index('int os_video_decoder_send('):
              source.index('int os_video_decoder_receive(')]
program = r'''
#include <uapi/video_decoder.h>
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
struct os_video_decoder {
 AVCodecContext *codec;
 AVPacket *packet;
 AVBufferPool *packet_pool;
 size_t packet_capacity;
};
static AVBufferRef *held;
static uint8_t *last_data;
static unsigned sends;
static int hold, status;
static const uint8_t *expected;
static int expected_size;
int avcodec_send_packet(AVCodecContext *codec, const AVPacket *packet) {
 assert(codec);++sends;
 if (!packet) return status;
 assert(packet->size == expected_size && packet->pts == 123);
 assert(packet->dts == AV_NOPTS_VALUE);
 assert(!memcmp(packet->data, expected, expected_size));
 for (unsigned i=0;i<AV_INPUT_BUFFER_PADDING_SIZE;++i)
  assert(!packet->data[expected_size+i]);
 last_data=packet->data;
 if(hold){assert(!held);held=av_buffer_ref(packet->buf);assert(held);}
 return status;
}
''' + send + r'''
int main(void) {
 AVCodecContext codec={0};
 os_video_decoder_t decoder={.codec=&codec,.packet=av_packet_alloc()};
 assert(decoder.packet);
 uint8_t payload[4096];memset(payload,0xa5,sizeof(payload));expected=payload;
 expected_size=1024;
 uint8_t *first=NULL;
 for(unsigned i=0;i<10000;++i){
  assert(!os_video_decoder_send(&decoder,payload,expected_size,123));
  if(!first)first=last_data;else assert(first==last_data);
  assert(!decoder.packet->buf);
 }
 /* 引用未归还时，下一包即便尺寸相同也必须使用另一份存储。 */
 hold=1;assert(!os_video_decoder_send(&decoder,payload,1024,123));hold=0;
 uint8_t *borrowed=held->data;assert(borrowed==first);
 memset(payload,0x5a,sizeof(payload));
 assert(!os_video_decoder_send(&decoder,payload,1024,123));assert(last_data!=borrowed);
 for(unsigned i=0;i<1024;++i)assert(borrowed[i]==0xa5);
 /* 扩容销毁旧池后，解码器仍持有的旧引用继续有效。 */
 expected_size=4096;assert(!os_video_decoder_send(&decoder,payload,4096,123));
 for(unsigned i=0;i<1024;++i)assert(borrowed[i]==0xa5);
 av_buffer_unref(&held);
 /* 缩短后的包尾必须清零，不能暴露上一大包留下的数据。 */
 expected_size=3;status=OS_VIDEO_AGAIN;
 assert(os_video_decoder_send(&decoder,payload,3,123)==OS_VIDEO_AGAIN);
 status=-EIO;assert(os_video_decoder_send(&decoder,payload,3,123)==-EIO);
 status=0;assert(!os_video_decoder_send(&decoder,payload,3,123));
 unsigned before=sends;
 assert(os_video_decoder_send(&decoder,payload,INT_MAX-64,123)==-EINVAL);
 assert(os_video_decoder_send(&decoder,NULL,3,123)==-EINVAL&&sends==before);
 assert(!os_video_decoder_send(&decoder,NULL,0,123));
 av_packet_free(&decoder.packet);av_buffer_pool_uninit(&decoder.packet_pool);
}
'''
with tempfile.TemporaryDirectory(prefix='video-packet-pool-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(ffmpeg),
                    str(path / 'test.c'), str(ffmpeg / 'libavcodec/libavcodec.a'),
                    str(ffmpeg / 'libavutil/libavutil.a'), '-lm', '-pthread',
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
print('实际 FFmpeg 输入缓冲池：10000 包复用、保留引用隔离、扩容、包尾补零及错误恢复通过')
