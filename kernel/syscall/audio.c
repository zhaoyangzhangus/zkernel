/* REFACTOR_SYSCALL_AUDIO_OWNER：音频 UAPI 和流控制处理程序。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/audio.h>
#include <kernel/device.h>
#include <kernel/hda.h>
#include <kernel/kmem.h>
#include <kernel/io_mutex.h>
#include <kernel/vfs.h>
#include <uapi/file.h>
#include <kernel/process.h>
#include <kernel/xhci.h>
#include <uapi/audio.h>
#include <uapi/syscall.h>

#include "internal.h"
#include "audio_settings.h"

static kstatus_t audio_uapi_to_kernel(const os_audio_stream_config_t *source,
                                      audio_format_t *destination) {
    if (source == 0 || destination == 0 ||
        !versioned_header_valid(&source->hdr, sizeof(*source)) ||
        source->direction > OS_AUDIO_CAPTURE) return K_EINVAL;
    destination->sample_rate = source->sample_rate;
    destination->channels = source->channels;
    destination->sample_format = source->sample_format;
    destination->period_frames = source->period_frames;
    destination->period_count = source->period_count;
    return K_OK;
}

static uint32_t audio_uapi_state(audio_stream_state_t state) {
    if (state == AUDIO_STREAM_RUNNING) return OS_AUDIO_STATE_RUNNING;
    if (state == AUDIO_STREAM_DISCONNECTED) return OS_AUDIO_STATE_DISCONNECTED;
    return OS_AUDIO_STATE_READY;
}

/* 音频对象只暴露统一 UAPI，实际控制交给存在的硬件后端。 */
static kstatus_t audio_backend_configure(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_configure(stream);
    return status == K_ENOENT ? xhci_audio_stream_configure(stream) : status;
}

static kstatus_t audio_backend_start(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_start(stream);
    return status == K_ENOENT ? xhci_audio_stream_start(stream) : status;
}

static kstatus_t audio_backend_stop(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_stop(stream);
    return status == K_ENOENT ? xhci_audio_stream_stop(stream) : status;
}

static kstatus_t audio_backend_reset(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_reset(stream);
    return status == K_ENOENT ? xhci_audio_stream_reset(stream) : status;
}

static kstatus_t audio_backend_disconnect(audio_stream_t *stream) {
    kstatus_t status = hda_audio_stream_disconnect(stream);
    return status == K_ENOENT ? xhci_audio_stream_disconnect(stream) : status;
}

/* AUDIO_OPEN(args)：打开当前可用的 HDA 或 USB Audio DMA 流。 */
int64_t syscall_audio_open(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    audio_stream_t *stream = 0;
    audio_format_t format;
    device_t *device;
    handle_t handle = OS_INVALID_HANDLE;
    os_audio_open_t arguments;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = audio_uapi_to_kernel(&arguments.config, &format);
    if (status != K_OK) return status;
    if (arguments.backend > OS_AUDIO_BACKEND_USB_HEADSET) return K_EINVAL;
    status = audio_settings_ensure_loaded();
    if (status != K_OK) return status;
    if (arguments.backend == OS_AUDIO_BACKEND_AUTO) {
        os_audio_settings_t settings = {0};
        audio_settings_snapshot(&settings);
        arguments.backend = settings.effective_backend;
    }
    /* 串行保护端点选择和首次绑定，防止另一个 OPEN 在两者之间切换 USB 设备。 */
    status = audio_settings_lock_and_load();
    if (status != K_OK) return status;
    if (arguments.backend >= OS_AUDIO_BACKEND_USB)
        status = xhci_audio_select_device(arguments.backend-OS_AUDIO_BACKEND_USB);
    device = arguments.backend >= OS_AUDIO_BACKEND_USB ? xhci_audio_device() : hda_audio_device();
    if (!device || atomic_load_explicit(&device->state, memory_order_acquire) >= DEVICE_REMOVING)
        status = K_ENOENT;
    if (status == K_OK) status = audio_stream_create(device,
        (audio_direction_t)arguments.config.direction, &stream);
    if (status == K_OK) status = audio_stream_configure(stream, &format);
    if (status == K_OK) status = audio_backend_configure(stream);
    io_mutex_unlock_required(&g_audio_settings_lock);
    if (status == K_OK) {
        status = handle_create(&process->handles, stream,
                               AUDIO_STREAM_RIGHT_ALL, &handle);
    }
    if (status == K_OK) {
        arguments.handle = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    audio_stream_destroy(stream);
    return status;
}

/* AUDIO_CONTROL(handle,args)：控制流状态，并负责周期数据的用户拷贝。 */
int64_t syscall_audio_control(uint64_t handle, uint64_t arguments_pointer,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_audio_control_t arguments;
    audio_stream_t *stream;
    void *object = 0;
    void *temporary = 0;
    kstatus_t status;
    uint32_t rights;
    uint64_t expected;
    uint64_t submission = 0U;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.buffer_size > (uint64_t)SIZE_MAX) {
        return K_EINVAL;
    }
    switch (arguments.code) {
    case OS_AUDIO_CONTROL_CONFIGURE:
    case OS_AUDIO_CONTROL_START:
    case OS_AUDIO_CONTROL_STOP:
    case OS_AUDIO_CONTROL_QUEUE:
    case OS_AUDIO_CONTROL_COMPLETE:
    case OS_AUDIO_CONTROL_RECOVER:
    case OS_AUDIO_CONTROL_RESET:
    case OS_AUDIO_CONTROL_DISCONNECT:
        rights = AUDIO_STREAM_RIGHT_CONTROL;
        break;
    case OS_AUDIO_CONTROL_GET_STATS:
        rights = AUDIO_STREAM_RIGHT_QUERY;
        break;
    default:
        return K_ENOSYS;
    }
    status = handle_lookup(&process->handles, (handle_t)handle, rights, &object);
    if (status != K_OK) return status;
    stream = (audio_stream_t *)object;
    if (!audio_stream_is_valid(stream)) {
        status = K_EINVAL;
        goto done;
    }
    arguments.bytes_returned = 0U;
    switch (arguments.code) {
    case OS_AUDIO_CONTROL_CONFIGURE: {
        os_audio_stream_config_t config;
        audio_format_t format;
        if (arguments.buffer == 0U || arguments.buffer_size < sizeof(config)) {
            status = K_EINVAL;
            break;
        }
        status = copy_from_user(&config,
            (const void __user *)(uintptr_t)arguments.buffer, sizeof(config));
        if (status == K_OK) status = audio_uapi_to_kernel(&config, &format);
        if (status == K_OK) {
            status = audio_stream_configure(stream, &format);
            if (status == K_OK) status = audio_backend_configure(stream);
            if (status == K_OK) arguments.bytes_returned = sizeof(config);
        }
        break;
    }
    case OS_AUDIO_CONTROL_START:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_start(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) {
            status = audio_stream_start(stream);
            if (status != K_OK) (void)audio_backend_stop(stream);
        }
        break;
    case OS_AUDIO_CONTROL_STOP:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_stop(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_stop(stream);
        break;
    case OS_AUDIO_CONTROL_QUEUE:
        expected = audio_stream_bytes_for_frames(stream, arguments.frames);
        if (expected == 0U || arguments.period == UINT32_MAX ||
            audio_stream_direction(stream) == AUDIO_CAPTURE) {
            status = K_EINVAL;
            break;
        }
        if (arguments.buffer == 0U || arguments.buffer_size != expected) {
            status = K_EINVAL;
            break;
        }
        /* 用户拷贝成功后才会提交整块数据，无需预先清零；拷贝失败直接释放。 */
        temporary = kmalloc((size_t)expected, 0);
        if (temporary == 0) {
            status = K_ENOMEM;
            break;
        }
        status = copy_from_user(temporary,
            (const void __user *)(uintptr_t)arguments.buffer, (size_t)expected);
        if (status == K_OK && audio_stream_direction(stream) == AUDIO_PLAYBACK) {
            audio_format_t format;
            audio_stream_get_format(stream, &format);
            audio_settings_apply_gain(temporary, expected, format.sample_format);
        }
        if (status == K_OK) status = audio_stream_submit(stream,
            arguments.period, temporary, expected, arguments.frames, &submission);
        if (status == K_OK) {
            kstatus_t backend = xhci_audio_stream_queue(stream, arguments.period,
                                                     arguments.frames);
            if (backend != K_OK && backend != K_ENOENT) {
                (void)audio_stream_cancel_submit(stream, arguments.period, submission);
                status = backend;
            }
        }
        kfree(temporary);
        temporary = 0;
        if (status == K_OK) arguments.bytes_returned = expected;
        break;
    case OS_AUDIO_CONTROL_COMPLETE:
        expected = audio_stream_bytes_for_frames(stream, arguments.frames);
        if (expected == 0U || arguments.period == UINT32_MAX) {
            status = K_EINVAL;
            break;
        }
        if (audio_stream_direction(stream) == AUDIO_CAPTURE) {
            if (arguments.buffer == 0U || arguments.buffer_size != expected) {
                status = K_EINVAL;
                break;
            }
            temporary = kzalloc((size_t)expected, 0);
            if (temporary == 0) {
                status = K_ENOMEM;
                break;
            }
        } else if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = hda_audio_stream_complete(stream, arguments.period, arguments.frames);
        if (status == K_ENOENT) status = xhci_audio_stream_complete(stream,
                                        arguments.period, arguments.frames);
        if (status == K_OK) status = audio_stream_complete(stream, arguments.period, arguments.frames);
        if (status == K_OK && temporary != 0) {
            status = audio_stream_period_read(stream, arguments.period,
                                              temporary, expected);
            if (status == K_OK) status = copy_to_user(
                (void __user *)(uintptr_t)arguments.buffer, temporary,
                (size_t)expected);
        }
        if (temporary != 0) kfree(temporary);
        temporary = 0;
        if (status == K_OK) arguments.bytes_returned = expected;
        break;
    case OS_AUDIO_CONTROL_RECOVER:
        status = arguments.buffer == 0U && arguments.buffer_size == 0U ?
                 audio_stream_recover(stream) : K_EINVAL;
        break;
    case OS_AUDIO_CONTROL_RESET:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_reset(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_controller_reset(stream);
        break;
    case OS_AUDIO_CONTROL_DISCONNECT:
        if (arguments.buffer != 0U || arguments.buffer_size != 0U) {
            status = K_EINVAL;
            break;
        }
        status = audio_backend_disconnect(stream);
        if (status == K_ENOENT) status = K_OK;
        if (status == K_OK) status = audio_stream_disconnect(stream);
        break;
    case OS_AUDIO_CONTROL_GET_STATS: {
        audio_stream_stats_t kernel_stats = {0};
        os_audio_stream_stats_t user_stats = {0};
        if (arguments.buffer == 0U ||
            arguments.buffer_size < sizeof(user_stats)) {
            status = K_EINVAL;
            break;
        }
        audio_stream_get_stats(stream, &kernel_stats);
        user_stats.hdr.size = sizeof(user_stats);
        user_stats.hdr.version = OS_SYSCALL_ABI_VERSION;
        user_stats.queued_frames = kernel_stats.queued_frames;
        user_stats.mixed_frames = kernel_stats.completed_frames;
        user_stats.underruns = kernel_stats.underruns;
        user_stats.overruns = kernel_stats.overruns;
        user_stats.device_generation = kernel_stats.device_generation;
        user_stats.state = audio_uapi_state(kernel_stats.state);
        status = copy_to_user((void __user *)(uintptr_t)arguments.buffer,
                              &user_stats, sizeof(user_stats));
        if (status == K_OK) arguments.bytes_returned = sizeof(user_stats);
        break;
    }
    default:
        status = K_ENOSYS;
        break;
    }
done:
    if (temporary != 0) kfree(temporary);
    object_put(object);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/* DEVICE_ENUMERATE(args)：按紧凑索引返回公开设备信息，不暴露内核地址。 */
