#include <arch/x86_64/paging.h>
#include <kernel/audio.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_AUDIO_OWNER：USB 音频流策略和 UAPI 状态。 */

static uint8_t g_xhci_audio_slot;

static xhci_device_context_t *
xhci_audio_context(
    xhci_state_t *state)
{
    if(state == 0 ||
       !state->initialized)
    {
        return 0;
    }


    /*
    * O(1) 稳态路径。
     */
    if(g_xhci_audio_slot != 0U)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(g_xhci_audio_slot);


        if(slot_dev->used &&
           slot_dev->context.device_slot ==
               g_xhci_audio_slot &&
           slot_dev->context.audio_endpoint !=
               0U)
        {
            return
                &slot_dev->context;
        }


        /*
         * 设备已拔出，或 Slot 已被复用。
         */
        g_xhci_audio_slot =
            0U;
    }


    /*
    * 较慢的查找路径。
     */
    uint32_t limit =
        state->max_slots;


    if(limit >=
       XHCI_MAX_SLOT_TABLE)
    {
        limit =
            XHCI_MAX_SLOT_TABLE -
            1U;
    }


    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);


        if(!slot_dev->used ||
           slot_dev->context.device_slot !=
               (uint8_t)slot ||
           slot_dev->context.audio_endpoint ==
               0U)
        {
            continue;
        }


        g_xhci_audio_slot =
            (uint8_t)slot;


        return
            &slot_dev->context;
    }


    return 0;
}



/* 音频状态直接保存在设备 Slot 中，由统一音频流持有。 */

uint32_t
xhci_usb_audio_completed(void)
{
    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return 0U;
    }


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    return
        device != 0
        ? device->audio_completed
        : 0U;
}


struct device *
xhci_audio_device(void)
{
    return
        xhci_controller_state()->initialized &&
        xhci_topology_audio_configured() &&
        xhci_controller_state()->pci != 0
        ? (struct device *)
            &xhci_controller_state()->pci->device
        : 0;
}


static void
xhci_audio_lock(void)
{
    xhci_event_lock(xhci_controller_state());
}


static void
xhci_audio_unlock(void)
{
    xhci_event_unlock(xhci_controller_state());
}


/* 仅撤销逻辑周期；在途 DMA 仍由完成事件回收，不能在这里清除 pending。 */
static void xhci_audio_invalidate_stream(xhci_device_context_t *device) {
    ++device->audio_stream_generation;
    device->audio_stream_running = false;
    device->audio_stream_queued = false;
}

static bool
xhci_audio_format_supported(
    const xhci_device_context_t *device,
    const audio_format_t *format)
{
    uint32_t sample_bits;


    if(device == 0 ||
       format == 0 ||
       format->channels !=
           device->audio_channels ||
       format->sample_rate !=
           device->audio_sample_rate)
    {
        return false;
    }


    switch(format->sample_format)
    {
        case AUDIO_SAMPLE_S16_LE:
            sample_bits = 16U;
            break;

        case AUDIO_SAMPLE_S24_LE:
            sample_bits = 24U;
            break;

        case AUDIO_SAMPLE_S32_LE:
            sample_bits = 32U;
            break;

        default:
            return false;
    }


    return
        sample_bits ==
        device->
            audio_bit_resolution;
}


kstatus_t
xhci_audio_stream_configure(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    audio_format_t format;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured() ||
       audio_stream_device(audio) !=
           xhci_audio_device())
    {
        return K_ENOENT;
    }


    audio_stream_get_format(
        audio,
        &format);


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    if(audio_stream_direction(audio) !=
           (device->audio_endpoint_in
                ? AUDIO_CAPTURE
                : AUDIO_PLAYBACK) ||
       !xhci_audio_format_supported(
           device,
           &format) ||
       audio_stream_period_bytes(audio) >
           PAGE_SIZE)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    if(device->audio_stream != 0 &&
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_EBUSY;
    }


    /* 绑定新流时，使旧周期的完成事件失效。 */
    xhci_audio_invalidate_stream(device);
    device->audio_stream = audio;

    device->audio_period =
        0U;

    device->audio_frames =
        0U;




    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_start(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    bool success =
        true;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    device->audio_stream_running =
        true;


    if(device->audio_stream_queued &&
       !device->audio_transfer_pending)
    {
        success =
            xhci_queue_audio_transfer_device(
                xhci_controller_state(),
                device);


        if(!success)
        {
            device->
                audio_stream_running =
                    false;
        }
    }




    xhci_audio_unlock();


    return
        success
        ? K_OK
        : K_EIO;
}


kstatus_t
xhci_audio_stream_stop(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    xhci_audio_invalidate_stream(device);

    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_queue(
    struct audio_stream *stream,
    uint32_t period,
    uint32_t frames)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;

    audio_format_t format;

    bool success =
        true;


    if(audio == 0 ||
       frames == 0U)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    audio_stream_get_format(
        audio,
        &format);


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    if(device->audio_stream_queued ||
       (device->audio_transfer_pending &&
        device->audio_transfer_generation == device->audio_stream_generation))
    {
        xhci_audio_unlock();
        return K_EBUSY;
    }


    if(period >=
           format.period_count ||
       frames >
           format.period_frames)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    uint64_t bytes =
        audio_stream_bytes_for_frames(
            audio,
            frames);


    if(bytes == 0U ||
       bytes > PAGE_SIZE)
    {
        xhci_audio_unlock();
        return K_EINVAL;
    }


    device->audio_transfer_failed = false;
    device->audio_period =
        period;

    device->audio_frames =
        frames;

    device->audio_stream_queued =
        true;


    if(device->audio_stream_running && !device->audio_transfer_pending)
    {
        success =
            xhci_queue_audio_transfer_device(
                xhci_controller_state(),
                device);


        if(!success)
        {
            device->
                audio_stream_queued =
                    false;
        }
    }




    xhci_audio_unlock();


    return
        success
        ? K_OK
        : K_EIO;
}


kstatus_t
xhci_audio_stream_reset(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    xhci_audio_invalidate_stream(device);

    device->audio_period =
        0U;

    device->audio_frames =
        0U;




    xhci_audio_unlock();

    return K_OK;
}


kstatus_t
xhci_audio_stream_disconnect(
    struct audio_stream *stream)
{
    audio_stream_t *audio =
        (audio_stream_t *)stream;


    if(audio == 0)
    {
        return K_EINVAL;
    }


    if(!xhci_controller_state()->initialized ||
       !xhci_topology_audio_configured())
    {
        return K_ENOENT;
    }


    xhci_audio_lock();


    xhci_device_context_t *device =
        xhci_audio_context(
            xhci_controller_state());


    if(device == 0 ||
       device->audio_stream != audio)
    {
        xhci_audio_unlock();
        return K_ENOENT;
    }


    xhci_audio_invalidate_stream(device);
    device->audio_stream = 0;

    xhci_audio_unlock();

    return K_OK;
}

/* 只有控制器确认整个周期完成，用户态才能回收或覆盖对应缓冲区。 */
kstatus_t xhci_audio_stream_complete(struct audio_stream *stream,
                                    uint32_t period, uint32_t frames) {
    (void)frames;
    if (!stream || audio_stream_device(stream) != xhci_audio_device()) return K_ENOENT;
    xhci_audio_lock();
    xhci_device_context_t *device = xhci_audio_context(xhci_controller_state());
    kstatus_t status = K_ENOENT;
    if (device && device->audio_stream == stream) {
        status = period != device->audio_period ? K_EINVAL :
                 device->audio_transfer_failed ? K_EIO :
                 device->audio_stream_queued || device->audio_transfer_pending ? K_EAGAIN : K_OK;
    }
    xhci_audio_unlock();
    return status;
}

uint32_t xhci_audio_device_count(void) {
    if (!xhci_controller_state()->initialized) return 0U;
    uint32_t count=0U;
    for (uint32_t i=1;i<XHCI_MAX_SLOT_TABLE;++i) {
        xhci_slot_device_t *slot=xhci_topology_slot(i);
        if (slot && slot->used && slot->context.audio_endpoint) ++count;
    }
    return count;
}

kstatus_t xhci_audio_select_device(uint32_t ordinal) {
    uint32_t count=0U;
    uint8_t selected=0U;
    xhci_audio_lock();
    for (uint32_t i=1;i<XHCI_MAX_SLOT_TABLE;++i) {
        xhci_slot_device_t *slot=xhci_topology_slot(i);
        if (!slot || !slot->used || !slot->context.audio_endpoint) continue;
        if (count++==ordinal) selected=(uint8_t)i;
    }
    if (!selected) {xhci_audio_unlock();return K_ENOENT;}
    if (selected!=g_xhci_audio_slot) {
        for (uint32_t i=1;i<XHCI_MAX_SLOT_TABLE;++i) {
            xhci_slot_device_t *slot=xhci_topology_slot(i);
            /* 实际流和在途 DMA 才占用设备，关闭历史不影响切换。 */
            if (slot && slot->used &&
                (slot->context.audio_stream || slot->context.audio_transfer_pending)) {
                xhci_audio_unlock();return K_EBUSY;
            }
        }
    }
    g_xhci_audio_slot=selected;
    xhci_audio_unlock();return K_OK;
}
