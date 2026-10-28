#include <kernel/audio.h>
#include <kernel/device.h>
#include <kernel/dma.h>
#include <kernel/hda.h>
#include <kernel/xhci.h>
#include <kernel/kmem.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>
#include <kernel/iommu.h>
#include <kernel/string.h>
#include <kernel/sched.h>

#define AUDIO_MAX_PERIODS 32U
#define AUDIO_MAX_CHANNELS 8U
#define AUDIO_MIN_RATE 8000U
#define AUDIO_MAX_RATE 192000U
#define AUDIO_MIN_PERIOD_FRAMES 64U
#define AUDIO_MAX_PERIOD_FRAMES 4096U
#define AUDIO_PERIOD_FREE 0U
#define AUDIO_PERIOD_QUEUED 1U
#define AUDIO_PERIOD_ACTIVE 2U
#define AUDIO_PERIOD_INVALID UINT32_MAX

typedef struct audio_period {
    page_t **pages;
    uint32_t page_count;
    dma_mapping_t dma;
    uint32_t state;
    uint32_t frames;
    uint64_t submission;
} audio_period_t;

struct audio_stream {
    object_header_t object;
    spinlock_t lock;
    struct device *device;
    audio_direction_t direction;
    audio_stream_state_t state;
    audio_format_t format;
    uint32_t bytes_per_sample;
    uint32_t frame_bytes;
    uint64_t period_bytes;
    audio_period_t periods[AUDIO_MAX_PERIODS];
    uint32_t active_period;
    uint32_t queued_count;
    uint64_t queued_frames;
    uint64_t queued_total;
    uint64_t submission_sequence;
    uint64_t completed_total;
    uint64_t completed_frames;
    uint64_t underruns;
    uint64_t overruns;
    uint64_t controller_resets;
    uint64_t device_generation;
    struct audio_stream *retired_next;
};

static atomic_uint g_audio_init_state;
static spinlock_t g_audio_retired_lock;
static audio_stream_t *g_audio_retired;

static bool audio_stream_try_destroy(audio_stream_t *stream);

/* 失败对象由回收链表持有；只在打开新流时重试，避免后台忙轮询。 */
static void audio_stream_retire(audio_stream_t *stream) {
    sched_preempt_disable();
    spinlock_lock(&g_audio_retired_lock);
    stream->retired_next = g_audio_retired;
    g_audio_retired = stream;
    spinlock_unlock(&g_audio_retired_lock);
    sched_preempt_enable();
}

static void audio_stream_reap_retired(void) {
    sched_preempt_disable();
    spinlock_lock(&g_audio_retired_lock);
    audio_stream_t *streams = g_audio_retired;
    g_audio_retired = 0;
    spinlock_unlock(&g_audio_retired_lock);
    sched_preempt_enable();
    /* 先摘下快照，在锁外停止设备和释放映射；本轮失败的对象留到下次打开。 */
    while (streams != 0) {
        audio_stream_t *next = streams->retired_next;
        streams->retired_next = 0;
        if (!audio_stream_try_destroy(streams)) audio_stream_retire(streams);
        streams = next;
    }
}

static void audio_stream_object_destroy(void *object);

static const object_ops_t g_audio_stream_ops = {
    .destroy = audio_stream_object_destroy,
    .type_name = "AudioStream",
    .is_signaled = 0,
    .wait_value = 0,
};

static void audio_object_init(object_header_t *object) {
    refcount_init(&object->refs, 1U);
    object->type = KOBJECT_TYPE_AUDIO_STREAM;
    object->flags = 0U;
    object->ops = &g_audio_stream_ops;
}

static void audio_lock(audio_stream_t *stream) {
    sched_preempt_disable();
    spinlock_lock(&stream->lock);
}

static void audio_unlock(audio_stream_t *stream) {
    spinlock_unlock(&stream->lock);
    sched_preempt_enable();
}

static void audio_copy_bytes(void *destination, const void *source, size_t size) {
    if (size == 0U) return;
    uintptr_t out_address = (uintptr_t)destination;
    uintptr_t in_address = (uintptr_t)source;
    uintptr_t distance = out_address >= in_address ?
                         out_address - in_address : in_address - out_address;
    if (distance >= size) {
        memcpy(destination, source, size);
        return;
    }
    /* Preserve the original forward-copy behavior for overlapping ranges. */
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0U) *out++ = *in++;
}

static uint32_t audio_sample_bytes(uint16_t sample_format) {
    switch ((audio_sample_format_t)sample_format) {
        case AUDIO_SAMPLE_S16_LE: return 2U;
        case AUDIO_SAMPLE_S24_LE: return 3U;
        case AUDIO_SAMPLE_S32_LE:
        case AUDIO_SAMPLE_F32_LE: return 4U;
        default: return 0U;
    }
}

static bool audio_validate_format(const audio_format_t *format,
                                  uint32_t *sample_bytes,
                                  uint32_t *frame_bytes,
                                  uint64_t *period_bytes,
                                  uint32_t *page_count) {
    uint32_t bytes;
    uint64_t frames_bytes;
    uint64_t total_bytes;
    if (format == 0 || sample_bytes == 0 || frame_bytes == 0 ||
        period_bytes == 0 || page_count == 0 ||
        format->sample_rate < AUDIO_MIN_RATE ||
        format->sample_rate > AUDIO_MAX_RATE || format->channels == 0U ||
        format->channels > AUDIO_MAX_CHANNELS ||
        format->period_frames < AUDIO_MIN_PERIOD_FRAMES ||
        format->period_frames > AUDIO_MAX_PERIOD_FRAMES ||
        format->period_count < 2U || format->period_count > AUDIO_MAX_PERIODS) {
        return false;
    }
    bytes = audio_sample_bytes(format->sample_format);
    if (bytes == 0U || (uint64_t)bytes * format->channels > UINT32_MAX) {
        return false;
    }
    *sample_bytes = bytes;
    *frame_bytes = bytes * format->channels;
    if (format->period_frames > UINT64_MAX / *frame_bytes) return false;
    frames_bytes = (uint64_t)format->period_frames * *frame_bytes;
    if (frames_bytes == 0U || frames_bytes > UINT64_MAX - (PAGE_SIZE - 1U)) {
        return false;
    }
    total_bytes = (frames_bytes + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
    if (total_bytes == 0U || total_bytes / PAGE_SIZE > UINT32_MAX) return false;
    *period_bytes = frames_bytes;
    *page_count = (uint32_t)(total_bytes / PAGE_SIZE);
    return *page_count != 0U;
}

static bool audio_release_period(audio_period_t *period) {
    if (period == 0) return false;
    if (period->dma.device != 0 &&
        dma_unmap_checked(&period->dma) != K_OK) return false;
    if (period->pages != 0) {
        for (uint32_t i = 0; i < period->page_count; ++i) {
            if (period->pages[i] != 0) page_free(period->pages[i]);
        }
        kfree(period->pages);
    }
    *period = (audio_period_t){0};
    return true;
}

static bool audio_release_periods_locked(audio_stream_t *stream) {
    bool success = true;
    if (stream == 0) return false;
    for (uint32_t i = 0; i < AUDIO_MAX_PERIODS; ++i) {
        if (!audio_release_period(&stream->periods[i])) success = false;
    }
    if (!success) return false;
    stream->active_period = AUDIO_PERIOD_INVALID;
    stream->queued_count = 0U;
    stream->queued_frames = 0U;
    return true;
}

static void audio_activate_next_locked(audio_stream_t *stream) {
    if (stream->active_period != AUDIO_PERIOD_INVALID) return;
    for (uint32_t i = 0; i < stream->format.period_count; ++i) {
        audio_period_t *period = &stream->periods[i];
        if (period->state != AUDIO_PERIOD_QUEUED) continue;
        period->state = AUDIO_PERIOD_ACTIVE;
        stream->active_period = i;
        return;
    }
}

static kstatus_t audio_allocate_periods_locked(audio_stream_t *stream) {
    uint32_t page_count = (uint32_t)((stream->period_bytes + PAGE_SIZE - 1U) /
                                     PAGE_SIZE);
    for (uint32_t i = 0; i < stream->format.period_count; ++i) {
        audio_period_t *period = &stream->periods[i];
        period->pages = (page_t **)kzalloc((size_t)page_count * sizeof(page_t *), 0);
        if (period->pages == 0) return K_ENOMEM;
        period->page_count = page_count;
        for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
            period->pages[page_index] = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
            if (period->pages[page_index] == 0) return K_ENOMEM;
        }
        if (dma_map_pages(stream->device, period->pages, page_count,
                          stream->direction == AUDIO_PLAYBACK ? DMA_TO_DEVICE :
                                                                 DMA_FROM_DEVICE,
                          &period->dma) != K_OK) {
            return K_EIO;
        }
    }
    return K_OK;
}

bool audio_core_init(void) {
    if (atomic_load_explicit(&g_audio_init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_audio_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_store_explicit(&g_audio_init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_audio_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

bool audio_stream_is_valid(const audio_stream_t *stream) {
    return stream != 0 &&
           ((const object_header_t *)stream)->type == KOBJECT_TYPE_AUDIO_STREAM;
}

struct device *audio_stream_device(const audio_stream_t *stream) {
    struct device *device = 0;
    if (stream == 0) return 0;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    device = mutable_stream->device;
    audio_unlock(mutable_stream);
    return device;
}

kstatus_t audio_stream_create(struct device *device, audio_direction_t direction,
                              audio_stream_t **out) {
    audio_stream_t *stream;
    if (device == 0 || out == 0 || direction > AUDIO_CAPTURE ||
        !audio_core_init()) return K_EINVAL;
    audio_stream_reap_retired();
    stream = (audio_stream_t *)kzalloc(sizeof(*stream), 0);
    if (stream == 0) return K_ENOMEM;
    audio_object_init(&stream->object);
    spinlock_init(&stream->lock);
    stream->device = device;
    stream->direction = direction;
    stream->state = AUDIO_STREAM_NEW;
    stream->active_period = AUDIO_PERIOD_INVALID;
    stream->device_generation = 1U;
    object_get(device);
    *out = stream;
    return K_OK;
}

audio_direction_t audio_stream_direction(const audio_stream_t *stream) {
    return stream == 0 ? AUDIO_CAPTURE : stream->direction;
}

static bool audio_stream_try_destroy(audio_stream_t *stream) {
    struct device *device;
    device = stream->device;
    /* 先停止设备后端并释放控制器描述符，再释放 period 的 DMA 映射。 */
    kstatus_t usb_status = xhci_audio_stream_disconnect(stream);
    kstatus_t hda_status = hda_audio_stream_disconnect(stream);
    /* 后端未确认停止时，保留流、设备引用和周期映射，避免 DMA 访问已释放内存。 */
    if ((usb_status != K_OK && usb_status != K_ENOENT) ||
        (hda_status != K_OK && hda_status != K_ENOENT)) return false;
    if (audio_stream_disconnect(stream) != K_OK) return false;
    if (device != 0) object_put(device);
    kfree(stream);
    return true;
}

static void audio_stream_object_destroy(void *object) {
    audio_stream_t *stream = object;
    if (stream != 0 && !audio_stream_try_destroy(stream)) audio_stream_retire(stream);
}

void audio_stream_destroy(audio_stream_t *stream) {
    /* 释放创建者持有的引用；用户态句柄仍可独立持有该流。 */
    object_put(stream);
}

kstatus_t audio_stream_configure(audio_stream_t *stream,
                                 const audio_format_t *format) {
    uint32_t sample_bytes;
    uint32_t frame_bytes;
    uint32_t page_count;
    uint64_t period_bytes;
    kstatus_t status;
    if (stream == 0 || !audio_validate_format(format, &sample_bytes, &frame_bytes,
                                              &period_bytes, &page_count)) {
        return K_EINVAL;
    }
    (void)page_count;
    audio_lock(stream);
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        audio_unlock(stream);
        return K_EDEVREMOVED;
    }
    if (stream->state == AUDIO_STREAM_RUNNING) {
        audio_unlock(stream);
        return K_EBUSY;
    }
    if (!audio_release_periods_locked(stream)) {
        audio_unlock(stream);
        return K_EIO;
    }
    stream->format = *format;
    stream->bytes_per_sample = sample_bytes;
    stream->frame_bytes = frame_bytes;
    stream->period_bytes = period_bytes;
    status = audio_allocate_periods_locked(stream);
    if (status != K_OK) {
        if (audio_release_periods_locked(stream)) stream->state = AUDIO_STREAM_NEW;
        audio_unlock(stream);
        return status;
    }
    stream->state = AUDIO_STREAM_CONFIGURED;
    audio_unlock(stream);
    return K_OK;
}

kstatus_t audio_stream_start(audio_stream_t *stream) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        audio_unlock(stream);
        return K_EDEVREMOVED;
    }
    if (stream->state != AUDIO_STREAM_CONFIGURED &&
        stream->state != AUDIO_STREAM_XRUN &&
        stream->state != AUDIO_STREAM_RUNNING) {
        audio_unlock(stream);
        return K_EINVAL;
    }
    if (stream->state == AUDIO_STREAM_XRUN) {
        audio_unlock(stream);
        return K_EAGAIN;
    }
    audio_activate_next_locked(stream);
    stream->state = AUDIO_STREAM_RUNNING;
    audio_unlock(stream);
    return K_OK;
}

kstatus_t audio_stream_stop(audio_stream_t *stream) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        audio_unlock(stream);
        return K_EDEVREMOVED;
    }
    if (stream->state == AUDIO_STREAM_NEW) {
        audio_unlock(stream);
        return K_OK;
    }
    for (uint32_t i = 0; i < stream->format.period_count; ++i) {
        stream->periods[i].state = AUDIO_PERIOD_FREE;
        stream->periods[i].frames = 0U;
    }
    stream->active_period = AUDIO_PERIOD_INVALID;
    stream->queued_count = 0U;
    stream->queued_frames = 0U;
    stream->state = AUDIO_STREAM_CONFIGURED;
    audio_unlock(stream);
    return K_OK;
}

static kstatus_t audio_stream_queue_locked(audio_stream_t *stream, uint32_t period,
                             uint32_t frames) {
    audio_period_t *entry;
    if (stream == 0 || frames == 0U) return K_EINVAL;
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        return K_EDEVREMOVED;
    }
    if (stream->state == AUDIO_STREAM_NEW || period >= stream->format.period_count ||
        frames > stream->format.period_frames) {
        return K_EINVAL;
    }
    entry = &stream->periods[period];
    if (entry->state != AUDIO_PERIOD_FREE) {
        ++stream->overruns;
        return K_EBUSY;
    }
    if (stream->submission_sequence == UINT64_MAX) return K_EOVERFLOW;
    entry->submission = ++stream->submission_sequence;
    entry->frames = frames;
    dma_sync_for_device(&entry->dma);
    entry->state = AUDIO_PERIOD_QUEUED;
    ++stream->queued_count;
    stream->queued_frames += frames;
    ++stream->queued_total;
    if (stream->state != AUDIO_STREAM_XRUN) audio_activate_next_locked(stream);
    return K_OK;
}

kstatus_t audio_stream_queue(audio_stream_t *stream, uint32_t period,
                             uint32_t frames) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    kstatus_t status = audio_stream_queue_locked(stream, period, frames);
    audio_unlock(stream);
    return status;
}

kstatus_t audio_stream_complete(audio_stream_t *stream, uint32_t period,
                                uint32_t frames) {
    audio_period_t *entry;
    uint32_t completed_frames;
    uint32_t queued_frames;
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state != AUDIO_STREAM_RUNNING ||
        period >= stream->format.period_count ||
        stream->active_period != period) {
        audio_unlock(stream);
        return stream->state == AUDIO_STREAM_DISCONNECTED ? K_EDEVREMOVED : K_EBUSY;
    }
    entry = &stream->periods[period];
    completed_frames = frames == 0U ? entry->frames : frames;
    queued_frames = entry->frames;
    if (entry->state != AUDIO_PERIOD_ACTIVE || completed_frames == 0U ||
        completed_frames > entry->frames) {
        audio_unlock(stream);
        return K_EINVAL;
    }
    dma_sync_for_cpu(&entry->dma);
    entry->state = AUDIO_PERIOD_FREE;
    if (stream->queued_frames >= queued_frames) {
        stream->queued_frames -= queued_frames;
    } else {
        stream->queued_frames = 0U;
    }
    entry->frames = 0U;
    stream->active_period = AUDIO_PERIOD_INVALID;
    if (stream->queued_count != 0U) --stream->queued_count;
    ++stream->completed_total;
    stream->completed_frames += completed_frames;
    audio_activate_next_locked(stream);
    if (stream->active_period == AUDIO_PERIOD_INVALID) {
        ++stream->underruns;
        stream->state = AUDIO_STREAM_XRUN;
    }
    audio_unlock(stream);
    return K_OK;
}

kstatus_t audio_stream_recover(audio_stream_t *stream) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        audio_unlock(stream);
        return K_EDEVREMOVED;
    }
    if (stream->state != AUDIO_STREAM_XRUN) {
        audio_unlock(stream);
        return K_OK;
    }
    audio_activate_next_locked(stream);
    if (stream->active_period == AUDIO_PERIOD_INVALID) {
        audio_unlock(stream);
        return K_EAGAIN;
    }
    stream->state = AUDIO_STREAM_RUNNING;
    audio_unlock(stream);
    return K_OK;
}

kstatus_t audio_stream_controller_reset(audio_stream_t *stream) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        audio_unlock(stream);
        return K_EDEVREMOVED;
    }
    for (uint32_t i = 0; i < stream->format.period_count; ++i) {
        stream->periods[i].state = AUDIO_PERIOD_FREE;
        stream->periods[i].frames = 0U;
    }
    stream->active_period = AUDIO_PERIOD_INVALID;
    stream->queued_count = 0U;
    stream->queued_frames = 0U;
    ++stream->controller_resets;
    stream->state = AUDIO_STREAM_CONFIGURED;
    audio_unlock(stream);
    return K_OK;
}

kstatus_t audio_stream_disconnect(audio_stream_t *stream) {
    if (stream == 0) return K_EINVAL;
    audio_lock(stream);
    if (stream->state != AUDIO_STREAM_DISCONNECTED) {
        if (audio_release_periods_locked(stream)) {
            stream->state = AUDIO_STREAM_DISCONNECTED;
            ++stream->device_generation;
        } else {
            audio_unlock(stream);
            return K_EIO;
        }
    }
    audio_unlock(stream);
    return K_OK;
}

void *audio_stream_period_address(audio_stream_t *stream, uint32_t period) {
    void *address = 0;
    if (stream == 0) return 0;
    audio_lock(stream);
    if (stream->state != AUDIO_STREAM_NEW &&
        stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < stream->format.period_count && stream->periods[period].pages != 0) {
        address = phys_to_direct(page_to_phys(stream->periods[period].pages[0]));
    }
    audio_unlock(stream);
    return address;
}

uint32_t audio_stream_period_page_count(audio_stream_t *stream, uint32_t period) {
    uint32_t page_count = 0U;
    if (stream == 0) return 0U;
    audio_lock(stream);
    if (stream->state != AUDIO_STREAM_NEW &&
        stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < stream->format.period_count) {
        page_count = stream->periods[period].page_count;
    }
    audio_unlock(stream);
    return page_count;
}

void *audio_stream_period_page_address(audio_stream_t *stream, uint32_t period,
                                       uint32_t page) {
    void *address = 0;
    if (stream == 0) return 0;
    audio_lock(stream);
    if (stream->state != AUDIO_STREAM_NEW &&
        stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < stream->format.period_count &&
        page < stream->periods[period].page_count &&
        stream->periods[period].pages != 0 &&
        stream->periods[period].pages[page] != 0) {
        address = phys_to_direct(page_to_phys(stream->periods[period].pages[page]));
    }
    audio_unlock(stream);
    return address;
}

uint64_t audio_stream_period_bytes(const audio_stream_t *stream) {
    return stream == 0 ? 0U : stream->period_bytes;
}

uint64_t audio_stream_bytes_for_frames(const audio_stream_t *stream,
                                       uint32_t frames) {
    uint64_t bytes = 0U;
    if (stream == 0) return 0U;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    if (mutable_stream->state != AUDIO_STREAM_NEW &&
        mutable_stream->state != AUDIO_STREAM_DISCONNECTED &&
        frames != 0U && frames <= mutable_stream->format.period_frames &&
        mutable_stream->frame_bytes != 0U) {
        bytes = (uint64_t)frames * mutable_stream->frame_bytes;
    }
    audio_unlock(mutable_stream);
    return bytes;
}

void audio_stream_get_format(const audio_stream_t *stream, audio_format_t *format) {
    if (stream == 0 || format == 0) return;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    *format = mutable_stream->format;
    audio_unlock(mutable_stream);
}

uint32_t audio_stream_period_dma_segment_count(const audio_stream_t *stream,
                                               uint32_t period) {
    uint32_t count = 0U;
    if (stream == 0) return 0U;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    if (mutable_stream->state != AUDIO_STREAM_NEW &&
        mutable_stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < mutable_stream->format.period_count) {
        count = mutable_stream->periods[period].dma.segment_count;
    }
    audio_unlock(mutable_stream);
    return count;
}

uint64_t audio_stream_period_dma_address(const audio_stream_t *stream,
                                         uint32_t period, uint32_t segment) {
    uint64_t address = 0U;
    if (stream == 0) return 0U;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    if (mutable_stream->state != AUDIO_STREAM_NEW &&
        mutable_stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < mutable_stream->format.period_count &&
        segment < mutable_stream->periods[period].dma.segment_count) {
        address = mutable_stream->periods[period].dma.segments[segment].addr.value;
    }
    audio_unlock(mutable_stream);
    return address;
}

uint64_t audio_stream_period_dma_length(const audio_stream_t *stream,
                                        uint32_t period, uint32_t segment) {
    uint64_t length = 0U;
    if (stream == 0) return 0U;
    audio_stream_t *mutable_stream = (audio_stream_t *)stream;
    audio_lock(mutable_stream);
    if (mutable_stream->state != AUDIO_STREAM_NEW &&
        mutable_stream->state != AUDIO_STREAM_DISCONNECTED &&
        period < mutable_stream->format.period_count &&
        segment < mutable_stream->periods[period].dma.segment_count) {
        length = mutable_stream->periods[period].dma.segments[segment].length;
    }
    audio_unlock(mutable_stream);
    return length;
}

static kstatus_t audio_stream_period_copy_locked(audio_stream_t *stream,
                                          uint32_t period, void *buffer,
                                          uint64_t bytes, bool to_stream) {
    audio_period_t *entry;
    uint64_t offset = 0U;
    if (stream == 0 || buffer == 0 || bytes == 0U) return K_EINVAL;
    if (stream->state == AUDIO_STREAM_NEW ||
        stream->state == AUDIO_STREAM_DISCONNECTED ||
        period >= stream->format.period_count ||
        bytes > stream->period_bytes || stream->periods[period].pages == 0) {
        return stream->state == AUDIO_STREAM_DISCONNECTED ? K_EDEVREMOVED : K_EINVAL;
    }
    entry = &stream->periods[period];
    /* 播放期由 DMA 持有；采集后端仍可写入自己的活动周期。 */
    if (to_stream && stream->direction == AUDIO_PLAYBACK &&
        entry->state != AUDIO_PERIOD_FREE) return K_EBUSY;
    while (offset < bytes) {
        uint32_t page_index = (uint32_t)(offset / PAGE_SIZE);
        uint32_t page_offset = (uint32_t)(offset & (PAGE_SIZE - 1U));
        uint64_t remaining_in_page = PAGE_SIZE - page_offset;
        uint64_t remaining = bytes - offset;
        size_t chunk = (size_t)(remaining < remaining_in_page ?
                                remaining : remaining_in_page);
        void *page_address;
        if (page_index >= entry->page_count || entry->pages[page_index] == 0) {
            return K_EIO;
        }
        page_address = (uint8_t *)phys_to_direct(
            page_to_phys(entry->pages[page_index])) + page_offset;
        if (to_stream) {
            audio_copy_bytes(page_address, (const uint8_t *)buffer + offset, chunk);
        } else {
            audio_copy_bytes((uint8_t *)buffer + offset, page_address, chunk);
        }
        offset += chunk;
    }
    return K_OK;
}

static kstatus_t audio_stream_period_copy(audio_stream_t *stream,
                                          uint32_t period, void *buffer,
                                          uint64_t bytes, bool to_stream) {
    if (stream == 0 || buffer == 0 || bytes == 0U) return K_EINVAL;
    audio_lock(stream);
    kstatus_t status = audio_stream_period_copy_locked(stream, period, buffer,
                                                       bytes, to_stream);
    audio_unlock(stream);
    return status;
}

kstatus_t audio_stream_period_write(audio_stream_t *stream, uint32_t period,
                                    const void *buffer, uint64_t bytes) {
    return audio_stream_period_copy(stream, period, (void *)buffer, bytes, true);
}

kstatus_t audio_stream_period_read(audio_stream_t *stream, uint32_t period,
                                   void *buffer, uint64_t bytes) {
    return audio_stream_period_copy(stream, period, buffer, bytes, false);
}

/* 在同一临界区检查、复制并发布，避免两个提交者互相覆盖尚未入队的数据。 */
kstatus_t audio_stream_submit(audio_stream_t *stream, uint32_t period,
                               const void *buffer, uint64_t bytes,
                               uint32_t frames, uint64_t *submission) {
    if (stream == 0 || buffer == 0 || frames == 0U) return K_EINVAL;
    audio_lock(stream);
    kstatus_t status;
    if (stream->state == AUDIO_STREAM_DISCONNECTED) {
        status = K_EDEVREMOVED;
    } else if (stream->direction != AUDIO_PLAYBACK ||
               stream->state == AUDIO_STREAM_NEW ||
               frames > stream->format.period_frames ||
               bytes != (uint64_t)frames * stream->frame_bytes) {
        status = K_EINVAL;
    } else if (stream->submission_sequence == UINT64_MAX) {
        status = K_EOVERFLOW;
    } else {
        status = audio_stream_period_copy_locked(stream, period,
                                                   (void *)buffer, bytes, true);
        if (status == K_OK)
            status = audio_stream_queue_locked(stream, period, frames);
        if (status == K_OK && submission != 0)
            *submission = stream->periods[period].submission;
    }
    audio_unlock(stream);
    return status;
}

/* 后端拒绝后仅撤销对应提交，旧请求不能取消重置后的新周期。 */
kstatus_t audio_stream_cancel_submit(audio_stream_t *stream, uint32_t period,
                                     uint64_t submission) {
    if (stream == 0 || submission == 0U) return K_EINVAL;
    audio_lock(stream);
    kstatus_t status = K_OK;
    if (stream->state == AUDIO_STREAM_DISCONNECTED) status = K_EDEVREMOVED;
    else if (period >= stream->format.period_count) status = K_EINVAL;
    else {
        audio_period_t *entry = &stream->periods[period];
        if (entry->state == AUDIO_PERIOD_FREE || entry->submission != submission) {
            status = K_EAGAIN;
        } else {
            stream->queued_frames -= entry->frames;
            --stream->queued_count;
            --stream->queued_total;
            entry->frames = 0U;
            entry->state = AUDIO_PERIOD_FREE;
            if (stream->active_period == period) {
                stream->active_period = AUDIO_PERIOD_INVALID;
                if (stream->state != AUDIO_STREAM_XRUN) audio_activate_next_locked(stream);
                if (stream->state == AUDIO_STREAM_RUNNING &&
                    stream->active_period == AUDIO_PERIOD_INVALID)
                    stream->state = AUDIO_STREAM_XRUN;
            }
        }
    }
    audio_unlock(stream);
    return status;
}

void audio_stream_get_stats(audio_stream_t *stream, audio_stream_stats_t *stats) {
    if (stream == 0 || stats == 0) return;
    audio_lock(stream);
    stats->queued_periods = stream->queued_total;
    stats->queued_frames = stream->queued_frames;
    stats->completed_periods = stream->completed_total;
    stats->completed_frames = stream->completed_frames;
    stats->underruns = stream->underruns;
    stats->overruns = stream->overruns;
    stats->controller_resets = stream->controller_resets;
    stats->active_period = stream->active_period;
    stats->queued_count = stream->queued_count;
    stats->state = stream->state;
    stats->device_generation = stream->device_generation;
    audio_unlock(stream);
}

bool audio_core_self_test(void) {
    static device_t device;
    audio_stream_t *stream = 0;
    audio_stream_stats_t stats = {0};
    audio_format_t format = {
        .sample_rate = 48000U,
        .channels = 2U,
        .sample_format = AUDIO_SAMPLE_S16_LE,
        .period_frames = 128U,
        .period_count = 4U,
    };
    bool success = false;
    device_object_init(&device, 0xA001ULL, 0x0401U, 0, 0);
    /* IOMMU 开启时，合成音频设备也必须拥有独立 domain 才能验证真实 DMA 路径。 */
    if (iommu_hardware_enabled() &&
        iommu_attach_pci_device(&device, 0, 0, 30U, 7U) != K_OK) goto done;
    if (audio_stream_create(&device, AUDIO_PLAYBACK, &stream) != K_OK ||
        stream == 0 || audio_stream_configure(stream, &format) != K_OK ||
        audio_stream_period_bytes(stream) != 512U ||
        audio_stream_period_address(stream, 0U) == 0 ||
        audio_stream_queue(stream, 0U, 128U) != K_OK ||
        audio_stream_queue(stream, 1U, 128U) != K_OK ||
        audio_stream_start(stream) != K_OK ||
        audio_stream_complete(stream, 0U, 0U) != K_OK ||
        audio_stream_complete(stream, 1U, 128U) != K_OK) {
        goto done;
    }
    audio_stream_get_stats(stream, &stats);
    if (stats.state != AUDIO_STREAM_XRUN || stats.underruns != 1U ||
        stats.completed_frames != 256U || audio_stream_recover(stream) != K_EAGAIN ||
        audio_stream_queue(stream, 2U, 64U) != K_OK ||
        audio_stream_recover(stream) != K_OK ||
        audio_stream_complete(stream, 2U, 64U) != K_OK ||
        audio_stream_controller_reset(stream) != K_OK) {
        goto done;
    }
    audio_stream_get_stats(stream, &stats);
    success = stats.state == AUDIO_STREAM_CONFIGURED &&
              stats.controller_resets == 1U &&
              audio_stream_disconnect(stream) == K_OK &&
              audio_stream_queue(stream, 0U, 1U) == K_EDEVREMOVED;
done:
    if (stream != 0) audio_stream_destroy(stream);
    return success;
}
