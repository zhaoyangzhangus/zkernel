"""执行真实设备池，验证并存解码器共享、最终释放和并发失败恢复。"""
from pathlib import Path
import os
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('VIDEO_DEVICE_SOURCE',
                             root / 'tools/nvidia/vulkan_video_device.c')).read_text()
body = source[source.index('typedef struct video_decode_device_entry'):
              source.index('int video_encode_device_create(')]
program = r'''
#include <uapi/video_decoder.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define AVERROR(e) (-(e))
typedef struct { atomic_uint refs; int mode; } Device;
typedef struct { unsigned char *data; } AVBufferRef;
static atomic_uint creations, destructions, references;
static atomic_bool fail_create, fail_ref;
static bool check_destroy_lock;
static void destructor_probe(int mode);
static AVBufferRef *av_buffer_ref(AVBufferRef *ref) {
    assert(ref && ref->data);
    if (atomic_exchange(&fail_ref, false)) return NULL;
    AVBufferRef *copy = malloc(sizeof(*copy)); assert(copy);
    *copy = *ref; atomic_fetch_add(&((Device *)copy->data)->refs, 1);
    atomic_fetch_add(&references, 1); return copy;
}
static void av_buffer_unref(AVBufferRef **ref) {
    if (!ref || !*ref) return;
    Device *device = (Device *)(*ref)->data;
    assert(atomic_load(&device->refs) > 0);
    if (atomic_fetch_sub(&device->refs, 1) == 1) {
        if (check_destroy_lock) destructor_probe(device->mode);
        atomic_fetch_add(&destructions, 1); free(device);
    }
    assert(atomic_fetch_sub(&references, 1) > 0);
    free(*ref); *ref = NULL;
}
static int video_device_create_mode(AVBufferRef **ref, int mode) {
    assert(ref && !*ref && (mode == 0 || mode == 2 || mode == 3));
    if (atomic_exchange(&fail_create, false)) return AVERROR(EIO);
    Device *device = malloc(sizeof(*device)); assert(device);
    atomic_init(&device->refs, 1); device->mode = mode;
    *ref = malloc(sizeof(**ref)); assert(*ref);
    (*ref)->data = (void *)device;
    atomic_fetch_add(&creations, 1); atomic_fetch_add(&references, 1);
    return 0;
}
''' + body + r'''
static void destructor_probe(int mode) {
 unsigned index=mode==0?0:(unsigned)mode-1U;
 assert(!pthread_mutex_trylock(&decode_devices[index].lock));
 assert(!pthread_mutex_unlock(&decode_devices[index].lock));
}
enum { THREADS = 24, ROUNDS = 20 };
static pthread_barrier_t barrier;
static AVBufferRef *held[THREADS];
static void sync_threads(void) {
    int status = pthread_barrier_wait(&barrier);
    assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *worker(void *argument) {
    unsigned index = (unsigned)(uintptr_t)argument;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        sync_threads();
        assert(!video_decode_device_create(&held[index], 1 + index % 3));
        sync_threads();
        for (unsigned peer = 0; peer < THREADS; ++peer) {
            assert(held[peer]);
            assert((held[index]->data == held[peer]->data) == (index % 3 == peer % 3));
        }
        sync_threads();
        video_decode_device_release(&held[index],1+index%3); assert(!held[index]);
        sync_threads();
    }
    return NULL;
}
static void empty(void) {
    assert(!atomic_load(&references));
    assert(atomic_load(&creations) == atomic_load(&destructions));
    for (unsigned i = 0; i < 3; ++i)
        assert(!decode_devices[i].device && !decode_devices[i].users);
}
int main(void) {
    AVBufferRef *a = (void *)1, *b = NULL;
    assert(video_decode_device_create(NULL, 1) == -EINVAL);
    assert(video_decode_device_create(&a, 0) == -EINVAL && !a);
    assert(video_decode_device_create(&a, 4) == -EINVAL && !a);
    video_decode_device_release(NULL,0); video_decode_device_release(&a,0); empty();
    for (unsigned codec = 1; codec <= 3; ++codec) {
        unsigned before = atomic_load(&creations);
        assert(!video_decode_device_create(&a, codec));
        assert(!video_decode_device_create(&b, codec));
        assert(a != b && a->data == b->data);
        assert(atomic_load(&creations) == before + 1);
        video_decode_device_release(&a,codec); assert(!a && b);
        assert(atomic_load(&((Device *)b->data)->refs) == 2);
        check_destroy_lock=true;
        video_decode_device_release(&b,codec); video_decode_device_release(&b,codec); empty();
        check_destroy_lock=false;
    }
    atomic_store(&fail_create, true);
    assert(video_decode_device_create(&a, 1) == -EIO && !a); empty();
    /* 首次创建后引用分配失败，也不能在设备池锁内等待驱动销毁。 */
    for (unsigned codec = 1; codec <= 3; ++codec) {
        atomic_store(&fail_ref, true);
        check_destroy_lock = true;
        assert(video_decode_device_create(&a, codec) == -ENOMEM && !a); empty();
        check_destroy_lock = false;
    }
    assert(!video_decode_device_create(&a, 1));
    atomic_store(&fail_ref, true);
    assert(video_decode_device_create(&b, 1) == -ENOMEM && !b);
    assert(decode_devices[0].users == 1 && atomic_load(&references) == 2);
    decode_devices[0].users = UINT_MAX;
    assert(video_decode_device_create(&b, 1) == -EOVERFLOW && !b);
    decode_devices[0].users = 1;
    video_decode_device_release(&a,1); empty();
    /* H264 初始化锁被占用时，AV1 释放仍必须独立完成。 */
    assert(!video_decode_device_create(&a,3));
    assert(!pthread_mutex_lock(&decode_devices[0].lock));
    check_destroy_lock=true;video_decode_device_release(&a,3);check_destroy_lock=false;
    assert(!pthread_mutex_unlock(&decode_devices[0].lock));empty();
    unsigned before = atomic_load(&creations);
    assert(!pthread_barrier_init(&barrier, NULL, THREADS));
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
        assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < THREADS; ++i) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&barrier));
    assert(atomic_load(&creations) == before + 3 * ROUNDS); empty();
    puts("Vulkan 视频设备共享：24 线程、20 轮、三种编码锁隔离、锁外销毁、失败恢复和末次释放通过");
}
'''
with tempfile.TemporaryDirectory(prefix='video-device-sharing-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
                    '-I' + str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    subprocess.run([str(path / 'test')], check=True, timeout=30, preexec_fn=no_core)
