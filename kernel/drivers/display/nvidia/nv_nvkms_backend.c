/*
 * System display backend for the official NVIDIA 595.84 NVKMS KAPI.
 *
 * The KAPI object set owns display memory, GPU mappings, display programming,
 * and flip completion.  This adapter owns only the System display-core
 * contract and a two-surface staging policy for the current CPU-readback
 * HBUFFER path.  It deliberately does not implement PMA, GPUVA, GMMU, or
 * GSP/RM operations.
 */

#include <kernel/display.h>
#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/nvidia/nvkms.h>
#include <kernel/realtest.h>

#include <nvkms-api-types.h>
#include <nvkms-kapi.h>

#include <nv_port.h>

#include <stdint.h>
#include <string.h>

/* Opt-in only: successful frame submission must not wait on COM1 or FAT. */
#ifndef OS_NVKMS_FRAME_TRACE
#define OS_NVKMS_FRAME_TRACE 0
#endif

typedef struct nvkms_scanout_surface {
    struct NvKmsKapiMemory *memory;
    struct NvKmsKapiSurface *surface;
    void *mapping;
    uint64_t size;
    uint32_t pitch;
} nvkms_scanout_surface_t;

typedef struct nvkms_gpu_pick {
    bool found;
    NvU32 gpu_id;
    MIGDeviceId mig_device;
} nvkms_gpu_pick_t;

typedef struct nvkms_display_state {
    nv_port_mutex_t lock;
    bool lock_initialized;
    bool initialized;
    bool connected;
    bool owns_device;
    bool pending;
    bool pending_timed_out;
    uint64_t pending_started_ns;
    uint64_t submitted_flips;
    uint64_t completed_flips;
    uint64_t recovered_flips;
    uint64_t timed_out_flips;
    uint64_t pending_query_failures;
    nv_port_work_t progress_work;

    const struct NvKmsKapiFunctionsTable *functions;
    struct NvKmsKapiDevice *device;

    NvU32 gpu_id;
    NvU32 head;
    NvKmsKapiDisplay display;
    struct NvKmsKapiDisplayMode mode;

    NvU32 width;
    NvU32 height;
    NvU32 pitch;
    NvU32 has_video_memory;

    nvkms_scanout_surface_t current;
    nvkms_scanout_surface_t spare;
    nvkms_scanout_surface_t pending_surface;
    display_scanout_complete_fn pending_complete;
    void *pending_context;

    display_scanout_backend_t backend;
} nvkms_display_state_t;

static nvkms_display_state_t g_nvkms_display;

/*
 * Keep the real-test boundary visible even when an official KAPI call blocks
 * before returning.  Capture the marker in memory here; the bootstrap poller
 * can persist it while this worker is in a KAPI call, and the terminal result
 * persists the complete trace.  A FAT sync in every trace point would make
 * storage I/O part of the vendor control path and can mask the actual KAPI
 * boundary being diagnosed.
 */
static void nvkms_trace(const char *text)
{
    if (text == NULL)
        return;
    kernel_serial_write_serial_only(text);
}

static inline void nvkms_frame_trace(const char *text)
{
#if OS_NVKMS_FRAME_TRACE
    nvkms_trace(text);
#else
    (void)text;
#endif
}

static void nvkms_gpu_pick_callback(const struct NvKmsKapiGpuInfo *info)
{
    if (info == NULL || g_nvkms_display.functions == NULL)
        return;
    /* System currently has one physical GPU and does not expose MIG. */
    if (!((nvkms_gpu_pick_t *)g_nvkms_display.backend.context)->found) {
        nvkms_gpu_pick_t *pick =
            (nvkms_gpu_pick_t *)g_nvkms_display.backend.context;
        pick->found = true;
        pick->gpu_id = info->gpuInfo.gpu_id;
        pick->mig_device = info->migDevice;
    }
}

static void nvkms_surface_release(
    nvkms_display_state_t *state,
    nvkms_scanout_surface_t *surface)
{
    const struct NvKmsKapiFunctionsTable *f;

    if (state == NULL || surface == NULL)
        return;
    f = state->functions;
    if (f == NULL) {
        *surface = (nvkms_scanout_surface_t){0};
        return;
    }
    if (surface->surface != NULL && f->destroySurface != NULL)
        f->destroySurface(state->device, surface->surface);
    if (surface->mapping != NULL && f->unmapMemory != NULL)
        f->unmapMemory(state->device, surface->memory,
                       NVKMS_KAPI_MAPPING_TYPE_KERNEL, surface->mapping);
    if (surface->memory != NULL && f->freeMemory != NULL)
        f->freeMemory(state->device, surface->memory);
    *surface = (nvkms_scanout_surface_t){0};
}

static kstatus_t nvkms_surface_create(
    nvkms_display_state_t *state,
    nvkms_scanout_surface_t *surface)
{
    const struct NvKmsKapiFunctionsTable *f;
    struct NvKmsKapiAllocateMemoryParams alloc_params;
    struct NvKmsKapiCreateSurfaceParams surface_params;
    NvU8 compressible = 0U;

    if (state == NULL || surface == NULL || state->functions == NULL ||
        state->device == NULL || state->pitch == 0U || state->height == 0U)
        return K_EINVAL;

    f = state->functions;
    if (f->allocateMemory == NULL || f->freeMemory == NULL ||
        f->mapMemory == NULL || f->unmapMemory == NULL ||
        f->createSurface == NULL || f->destroySurface == NULL)
        return K_ENOSYS;

    if ((uint64_t)state->pitch * state->height > UINT64_MAX)
        return K_EOVERFLOW;
    surface->size = (uint64_t)state->pitch * state->height;
    if (surface->size == 0U)
        return K_EOVERFLOW;

    memset(&alloc_params, 0, sizeof(alloc_params));
    alloc_params.layout = NvKmsSurfaceMemoryLayoutPitch;
    alloc_params.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
    alloc_params.size = surface->size;
    alloc_params.noDisplayCaching = NV_TRUE;
    alloc_params.useVideoMemory = state->has_video_memory != 0U ?
                                  NV_TRUE : NV_FALSE;
    alloc_params.compressible = &compressible;
    surface->memory = f->allocateMemory(state->device, &alloc_params);
    if (surface->memory == NULL)
        return K_ENOMEM;

    if (!f->mapMemory(state->device, surface->memory,
                      NVKMS_KAPI_MAPPING_TYPE_KERNEL, &surface->mapping) ||
        surface->mapping == NULL) {
        nvkms_surface_release(state, surface);
        return K_EIO;
    }

    memset(&surface_params, 0, sizeof(surface_params));
    surface_params.planes[0].memory = surface->memory;
    surface_params.planes[0].offset = 0U;
    surface_params.planes[0].pitch = state->pitch;
    surface_params.width = state->width;
    surface_params.height = state->height;
    surface_params.format = NvKmsSurfaceMemoryFormatX8R8G8B8;
    surface_params.explicit_layout = NV_TRUE;
    surface_params.layout = NvKmsSurfaceMemoryLayoutPitch;
    surface->surface = f->createSurface(state->device, &surface_params);
    if (surface->surface == NULL) {
        nvkms_surface_release(state, surface);
        return K_EIO;
    }
    surface->pitch = state->pitch;
    return K_OK;
}

static kstatus_t nvkms_copy_gop_to_surface(
    const nvkms_display_state_t *state,
    nvkms_scanout_surface_t *surface,
    uint32_t gop_stride)
{
    if (state == NULL || surface == NULL || surface->surface == NULL ||
        gop_stride < state->width)
        return K_EINVAL;
    /* Initial modeset only needs a valid KAPI scanout surface.  Reading the
     * firmware GOP alias and writing the VRAM alias here is not required to
     * program the head, and is unsafe on this GB205 BAR1 mapping.  The
     * compositor's later upload path owns surface contents after modeset. */
    return K_OK;
}

static kstatus_t nvkms_copy_request_to_surface(
    const nvkms_display_state_t *state,
    const display_scanout_request_t *request,
    nvkms_scanout_surface_t *surface)
{
    uint64_t row_bytes;
    uint64_t map_length;
    void *source_mapping = NULL;
    kstatus_t status;

    if (state == NULL || request == NULL || surface == NULL ||
        surface->mapping == NULL || request->buffer == NULL ||
        request->format != 1U || request->width != state->width ||
        request->height != state->height ||
        request->stride < (uint64_t)request->width * sizeof(uint32_t))
        return K_EINVAL;

    nvkms_frame_trace("OS_NVKMS_NATIVE_COPY_BEGIN\r\n");

    row_bytes = (uint64_t)request->width * sizeof(uint32_t);
    if ((uint64_t)(request->height - 1U) > UINT64_MAX / request->stride)
        return K_EOVERFLOW;
    map_length = (uint64_t)(request->height - 1U) * request->stride;
    if (map_length > UINT64_MAX - row_bytes)
        return K_EOVERFLOW;
    map_length += row_bytes;

    status = gpu_allocation_cpu_map(request->buffer, request->offset,
                                    map_length, &source_mapping);
    if (status != K_OK || source_mapping == NULL) {
        nvkms_trace("OS_NVKMS_NATIVE_COPY_MAP_FAIL\r\n");
        return status == K_OK ? K_EIO : status;
    }

    nvkms_frame_trace("OS_NVKMS_NATIVE_COPY_MAP_OK\r\n");

    for (uint32_t y = 0U; y < request->height; ++y) {
        const volatile uint32_t *source =
            (const volatile uint32_t *)((const uint8_t *)source_mapping +
                                        (uint64_t)y * request->stride);
        volatile uint32_t *destination =
            (volatile uint32_t *)((uint8_t *)surface->mapping +
                                  (uint64_t)y * surface->pitch);
        for (uint32_t x = 0U; x < request->width; ++x)
            destination[x] = source[x];
    }
    __asm__ volatile ("sfence" : : : "memory");
    gpu_allocation_cpu_unmap(request->buffer, source_mapping, map_length);
    nvkms_frame_trace("OS_NVKMS_NATIVE_COPY_DONE\r\n");
    return K_OK;
}

static void nvkms_fill_layer(
    const nvkms_display_state_t *state,
    struct NvKmsKapiLayerRequestedConfig *layer,
    struct NvKmsKapiSurface *surface,
    bool initial)
{
    memset(layer, 0, sizeof(*layer));
    layer->config.surface = surface;
    layer->config.compParams.compMode =
        NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
    layer->config.compParams.surfaceAlpha = 255U;
    /* Match the official non-async KAPI client defaults. */
    layer->config.tearing = NV_FALSE;
    layer->config.minPresentInterval = 1U;
    layer->config.csc = NVKMS_IDENTITY_CSC_MATRIX;
    layer->config.inputTf = NVKMS_INPUT_TF_LINEAR;
    layer->config.outputTf = NVKMS_OUTPUT_TF_NONE;
    layer->config.inputColorSpace = NVKMS_INPUT_COLOR_SPACE_NONE;
    layer->config.inputColorRange = NVKMS_INPUT_COLOR_RANGE_DEFAULT;
    layer->config.srcX = 0U;
    layer->config.srcY = 0U;
    layer->config.srcWidth = (NvU16)state->width;
    layer->config.srcHeight = (NvU16)state->height;
    layer->config.dstX = 0;
    layer->config.dstY = 0;
    layer->config.dstWidth = (NvU16)state->width;
    layer->config.dstHeight = (NvU16)state->height;
    layer->flags.surfaceChanged = NV_TRUE;
    if (initial) {
        layer->flags.srcXYChanged = NV_TRUE;
        layer->flags.srcWHChanged = NV_TRUE;
        layer->flags.dstXYChanged = NV_TRUE;
        layer->flags.dstWHChanged = NV_TRUE;
        layer->flags.cscChanged = NV_TRUE;
        layer->flags.inputTfChanged = NV_TRUE;
        layer->flags.outputTfChanged = NV_TRUE;
        layer->flags.inputColorSpaceChanged = NV_TRUE;
        layer->flags.inputColorRangeChanged = NV_TRUE;
        layer->flags.hdrMetadataChanged = NV_TRUE;
        layer->flags.matrixOverridesChanged = NV_TRUE;
        layer->flags.ilutChanged = NV_TRUE;
        layer->flags.tmoChanged = NV_TRUE;
    }
}

static void nvkms_fill_config(
    const nvkms_display_state_t *state,
    struct NvKmsKapiRequestedModeSetConfig *config,
    struct NvKmsKapiSurface *surface,
    bool initial)
{
    struct NvKmsKapiHeadRequestedConfig *head;

    memset(config, 0, sizeof(*config));
    config->headsMask = 1U << state->head;
    head = &config->headRequestedConfig[state->head];
    head->modeSetConfig.bActive = NV_TRUE;
    head->modeSetConfig.numDisplays = 1U;
    head->modeSetConfig.displays[0] = state->display;
    head->modeSetConfig.mode = state->mode;
    head->modeSetConfig.vrrEnabled = NV_FALSE;
    head->modeSetConfig.colorimetry = NVKMS_OUTPUT_COLORIMETRY_DEFAULT;
    /* Match the official Linux KAPI client's initial CRTC state. */
    head->modeSetConfig.olutFpNormScale =
        NVKMS_OLUT_FP_NORM_SCALE_DEFAULT;
    head->flags.activeChanged = initial ? NV_TRUE : NV_FALSE;
    head->flags.displaysChanged = initial ? NV_TRUE : NV_FALSE;
    head->flags.modeChanged = initial ? NV_TRUE : NV_FALSE;
    nvkms_fill_layer(state,
                     &head->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX],
                     surface, initial);
}

static kstatus_t nvkms_apply_config(
    nvkms_display_state_t *state,
    struct NvKmsKapiRequestedModeSetConfig *config)
{
    struct NvKmsKapiModeSetReplyConfig reply;
    const struct NvKmsKapiFunctionsTable *f = state->functions;
    NvBool status;

    memset(&reply, 0, sizeof(reply));
    status = (f != NULL && f->applyModeSetConfig != NULL) ?
             f->applyModeSetConfig(state->device, config, &reply, NV_TRUE) :
             NV_FALSE;
    if (OS_NVKMS_FRAME_TRACE ||
        (reply.flipResult != NV_KMS_FLIP_RESULT_IN_PROGRESS &&
         (!status || reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS))) {
        kernel_serial_printf_serial_only(
            "OS_NVKMS_MODESET_RESULT ok=%u flip=%d\r\n",
            status ? 1U : 0U, (int)reply.flipResult);
    }
    if (!status) {
        return reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS ?
               K_EBUSY : K_EIO;
    }
    if (reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS)
        return K_EBUSY;
    if (reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS)
        return K_EIO;
    return K_OK;
}

static kstatus_t nvkms_atomic_commit(
    void *context,
    const display_scanout_request_t *request,
    display_scanout_complete_fn complete,
    void *completion_context)
{
    nvkms_display_state_t *state = (nvkms_display_state_t *)context;
    nvkms_scanout_surface_t selected;
    bool selected_from_spare;
    struct NvKmsKapiRequestedModeSetConfig config;
    kstatus_t status;

    if (state == NULL || request == NULL || complete == NULL) {
        nvkms_trace("OS_NVKMS_NATIVE_ATOMIC_ARGS_REJECT\r\n");
        return K_EINVAL;
    }
    if (!state->lock_initialized) {
        nvkms_trace("OS_NVKMS_NATIVE_ATOMIC_LOCK_UNINIT\r\n");
        return K_EBUSY;
    }
    if (nv_port_mutex_lock(&state->lock, UINT64_MAX) != K_OK) {
        nvkms_trace("OS_NVKMS_NATIVE_ATOMIC_LOCK_FAIL\r\n");
        return K_EBUSY;
    }
    nvkms_frame_trace("OS_NVKMS_NATIVE_ATOMIC_LOCK_OK\r\n");
    if (!state->initialized || !state->connected || state->pending) {
        nvkms_frame_trace("OS_NVKMS_NATIVE_ATOMIC_REJECT\r\n");
        (void)nv_port_mutex_unlock(&state->lock);
        return K_EBUSY;
    }

    selected_from_spare = state->spare.surface != NULL;
    if (selected_from_spare) {
        selected = state->spare;
        state->spare = (nvkms_scanout_surface_t){0};
    } else {
        selected = (nvkms_scanout_surface_t){0};
        status = nvkms_surface_create(state, &selected);
        if (status != K_OK) {
            (void)nv_port_mutex_unlock(&state->lock);
            return status;
        }
    }

    status = nvkms_copy_request_to_surface(state, request, &selected);
    if (status == K_OK) {
        nvkms_fill_config(state, &config, selected.surface, false);
        nvkms_frame_trace("OS_NVKMS_NATIVE_APPLY_BEGIN\r\n");
        status = nvkms_apply_config(state, &config);
        if (status == K_OK)
            nvkms_frame_trace("OS_NVKMS_NATIVE_APPLY_OK\r\n");
        else if (status != K_EBUSY)
            nvkms_trace("OS_NVKMS_NATIVE_APPLY_FAIL\r\n");
    }
    if (status != K_OK) {
        if (status != K_EBUSY)
            nvkms_trace("OS_NVKMS_NATIVE_ATOMIC_FAIL\r\n");
        if (selected_from_spare)
            state->spare = selected;
        else
            nvkms_surface_release(state, &selected);
        (void)nv_port_mutex_unlock(&state->lock);
        return status;
    }

    state->pending_surface = selected;
    state->pending_complete = complete;
    state->pending_context = completion_context;
    state->pending = true;
    state->pending_timed_out = false;
    state->pending_started_ns = nv_port_time_now_ns();
    ++state->submitted_flips;
    nvkms_frame_trace("OS_NVKMS_NATIVE_ATOMIC_ACCEPTED\r\n");
    (void)nv_port_mutex_unlock(&state->lock);
    return K_OK;
}

/* A missing event is not proof of a missing flip. Query the hardware before
 * retiring surfaces. If the engine really remains busy, fail the client once
 * but quarantine both surfaces until a later query proves retirement safe. */
static void nvkms_pending_progress(nvkms_display_state_t *state, bool event_seen)
{
    display_scanout_complete_fn complete = NULL;
    void *completion_context = NULL;
    kstatus_t completion_status = K_OK;
    bool report_timeout = false;
    uint64_t timeout_age_ns = 0U;
    NvBool hardware_pending = NV_TRUE;
    bool query_ok = false;

    if (state == NULL || !state->lock_initialized ||
        nv_port_mutex_lock(&state->lock, 0U) != K_OK) return;
    if (state->pending) {
        uint64_t age = nv_port_time_now_ns() - state->pending_started_ns;
        if (state->functions != NULL &&
            state->functions->getFlipPendingStatus != NULL) {
            query_ok = state->functions->getFlipPendingStatus(
                state->device, state->head, NVKMS_KAPI_LAYER_PRIMARY_IDX,
                &hardware_pending) != NV_FALSE;
            if (!query_ok) ++state->pending_query_failures;
        }
        if (query_ok && !hardware_pending) {
            nvkms_scanout_surface_t old = state->current;
            state->current = state->pending_surface;
            state->pending_surface = (nvkms_scanout_surface_t){0};
            state->spare = old;
            state->pending = false;
            ++state->completed_flips;
            if (!event_seen) ++state->recovered_flips;
            complete = state->pending_complete;
            completion_context = state->pending_context;
            state->pending_complete = NULL;
            state->pending_context = NULL;
            state->pending_timed_out = false;
        } else if (age >= 250000000ULL && !state->pending_timed_out) {
            state->pending_timed_out = true;
            ++state->timed_out_flips;
            complete = state->pending_complete;
            completion_context = state->pending_context;
            state->pending_complete = NULL;
            state->pending_context = NULL;
            completion_status = K_ETIMEDOUT;
            report_timeout = true;
            timeout_age_ns = age;
        }
    }
    (void)nv_port_mutex_unlock(&state->lock);
    if (report_timeout) {
        nvkms_trace("OS_NVKMS_FLIP_TIMEOUT_SURFACES_QUARANTINED\r\n");
        kernel_serial_printf_serial_only(
            "OS_NVKMS_FLIP_TIMEOUT age_ms=%llu query_ok=%u hw_pending=%u\r\n",
            (unsigned long long)(timeout_age_ns / 1000000ULL),
            query_ok ? 1U : 0U, hardware_pending ? 1U : 0U);
    }
    if (complete != NULL) complete(completion_context, completion_status);
}

static void nvkms_poll(void *context)
{
    nvkms_pending_progress((nvkms_display_state_t *)context, false);
}

static void nvkms_event_progress(void *context)
{
    nvkms_pending_progress((nvkms_display_state_t *)context, true);
}

static void nvkms_event_callback(const struct NvKmsKapiEvent *event)
{
    nvkms_display_state_t *state;

    if (event == NULL)
        return;
    state = (nvkms_display_state_t *)event->privateData;
    if (state == NULL)
        state = &g_nvkms_display;
    if (!state->lock_initialized)
        return;

    if (event->type == NVKMS_EVENT_TYPE_DPY_CHANGED ||
        event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED ||
        event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED) {
        if (nv_port_mutex_lock(&state->lock, UINT64_MAX) == K_OK) {
            if (event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED)
                state->connected = false;
            (void)nv_port_mutex_unlock(&state->lock);
        }
        return;
    }

    if (event->type != NVKMS_EVENT_TYPE_FLIP_OCCURRED ||
        event->u.flipOccurred.head != state->head ||
        event->u.flipOccurred.layer != NVKMS_KAPI_LAYER_PRIMARY_IDX)
        return;

    /* Vendor event dispatch may still own its global lock. Query hardware
     * only after returning from that dispatch, never recursively here. */
    (void)nv_port_work_queue_irq(&state->progress_work);
}

static bool nvkms_select_display(
    nvkms_display_state_t *state,
    const struct NvKmsKapiDeviceResourcesInfo *resources)
{
    const struct NvKmsKapiFunctionsTable *f = state->functions;
    NvKmsKapiDisplay displays[NVKMS_KAPI_MAX_CLONE_DISPLAYS];
    NvU32 display_count = NVKMS_KAPI_MAX_CLONE_DISPLAYS;
    NvU32 max_heads;

    if (f == NULL || resources == NULL || f->getDisplays == NULL ||
        f->getStaticDisplayInfo == NULL || f->getDynamicDisplayInfo == NULL ||
        f->getDisplayMode == NULL ||
        !f->getDisplays(state->device, &display_count, displays))
        return false;
    if (display_count > NVKMS_KAPI_MAX_CLONE_DISPLAYS)
        display_count = NVKMS_KAPI_MAX_CLONE_DISPLAYS;
    max_heads = resources->numHeads > NVKMS_KAPI_MAX_HEADS ?
                NVKMS_KAPI_MAX_HEADS : resources->numHeads;

    for (NvU32 display_index = 0U; display_index < display_count;
         ++display_index) {
        struct NvKmsKapiStaticDisplayInfo static_info;
        struct NvKmsKapiDynamicDisplayParams dynamic_info;

        memset(&static_info, 0, sizeof(static_info));
        if (!f->getStaticDisplayInfo(state->device, displays[display_index],
                                     &static_info))
            continue;
        memset(&dynamic_info, 0, sizeof(dynamic_info));
        dynamic_info.handle = displays[display_index];
        if (!f->getDynamicDisplayInfo(state->device, &dynamic_info) ||
            dynamic_info.connected == 0U)
            continue;

        for (NvU32 head = 0U; head < max_heads; ++head) {
            struct NvKmsKapiDisplayMode selected_mode;
            bool found_mode = false;
            bool found_preferred = false;

            if ((static_info.headMask & (1U << head)) == 0U ||
                resources->numLayers[head] == 0U)
                continue;

            for (NvU32 mode_index = 0U; mode_index < 256U; ++mode_index) {
                struct NvKmsKapiDisplayMode mode;
                NvBool valid = NV_FALSE;
                NvBool preferred = NV_FALSE;
                int more;

                memset(&mode, 0, sizeof(mode));
                more = f->getDisplayMode(state->device,
                                         displays[display_index], mode_index,
                                         &mode, &valid, &preferred);
                if (more < 0)
                    break;
                if (valid != NV_FALSE &&
                    mode.timings.hVisible == state->width &&
                    mode.timings.vVisible == state->height &&
                    (!found_mode || (preferred != NV_FALSE &&
                                     !found_preferred))) {
                    selected_mode = mode;
                    found_mode = true;
                    found_preferred = preferred != NV_FALSE;
                }
                if (found_preferred || more == 0)
                    break;
            }
            if (!found_mode)
                continue;
            if (f->validateDisplayMode != NULL &&
                !f->validateDisplayMode(state->device,
                                        displays[display_index],
                                        &selected_mode))
                continue;
            state->display = displays[display_index];
            state->head = head;
            state->mode = selected_mode;
            state->connected = true;
            return true;
        }
    }
    return false;
}

static void nvkms_display_cleanup(nvkms_display_state_t *state)
{
    const struct NvKmsKapiFunctionsTable *f;

    if (state == NULL)
        return;
    f = state->functions;
    nvkms_surface_release(state, &state->pending_surface);
    nvkms_surface_release(state, &state->spare);
    nvkms_surface_release(state, &state->current);
    if (state->device != NULL && state->owns_device &&
        f != NULL && f->releaseOwnership != NULL)
        f->releaseOwnership(state->device);
    if (state->device != NULL && f != NULL && f->freeDevice != NULL)
        f->freeDevice(state->device);
    state->device = NULL;
    state->owns_device = false;
    state->initialized = false;
    state->connected = false;
    state->pending = false;
}

kstatus_t nv_nvkms_display_init(void)
{
    const struct NvKmsKapiFunctionsTable *f;
    struct NvKmsKapiAllocateDeviceParams device_params;
    struct NvKmsKapiDeviceResourcesInfo resources;
    nvkms_gpu_pick_t pick = {0};
    uint32_t gop_width;
    uint32_t gop_height;
    uint32_t gop_stride;
    uint32_t gop_format;
    uint64_t row_bytes;
    uint64_t aligned_pitch;
    uint32_t alignment;
    kstatus_t status = K_EIO;

    if (!nv_nvkms_ready())
        return K_EIO;
    if (!g_nvkms_display.lock_initialized) {
        nv_port_mutex_init(&g_nvkms_display.lock);
        nv_port_work_init(&g_nvkms_display.progress_work,
                         nvkms_event_progress, &g_nvkms_display);
        g_nvkms_display.lock_initialized = true;
    }
    if (g_nvkms_display.initialized)
        return K_OK;
    if (!display_core_query(0U, &gop_width, &gop_height, &gop_stride,
                            &gop_format) || gop_format != 1U ||
        gop_width == 0U || gop_height == 0U || gop_stride < gop_width ||
        gop_width > UINT16_MAX || gop_height > UINT16_MAX)
        return K_EINVAL;

    f = (const struct NvKmsKapiFunctionsTable *)nv_nvkms_functions();
    if (f == NULL || f->enumerateGpus == NULL || f->allocateDevice == NULL ||
        f->freeDevice == NULL || f->grabOwnership == NULL ||
        f->releaseOwnership == NULL || f->getDeviceResourcesInfo == NULL ||
        f->getFlipPendingStatus == NULL)
        return K_ENOSYS;
    g_nvkms_display.functions = f;
    g_nvkms_display.width = gop_width;
    g_nvkms_display.height = gop_height;
    g_nvkms_display.backend.context = &pick;
    nvkms_trace("OS_NVKMS_ENUM_BEGIN\r\n");
    NvU32 enumerated = f->enumerateGpus(nvkms_gpu_pick_callback);
    kernel_serial_printf_serial_only(
        "OS_NVKMS_ENUM_DONE count=%u found=%u\r\n",
        enumerated, pick.found ? 1U : 0U);
    (void)realtest_flush();
    if (enumerated == 0U || !pick.found)
        goto fail;

    memset(&device_params, 0, sizeof(device_params));
    device_params.gpuId = pick.gpu_id;
    device_params.migDevice = pick.mig_device;
    device_params.privateData = &g_nvkms_display;
    device_params.eventCallback = nvkms_event_callback;
    nvkms_trace("OS_NVKMS_ALLOCATE_DEVICE_BEGIN\r\n");
    g_nvkms_display.device = f->allocateDevice(&device_params);
    nvkms_trace(g_nvkms_display.device != NULL ?
                "OS_NVKMS_ALLOCATE_DEVICE_DONE\r\n" :
                "OS_NVKMS_ALLOCATE_DEVICE_FAIL\r\n");
    if (g_nvkms_display.device == NULL)
        goto fail;
    nvkms_trace("OS_NVKMS_GRAB_OWNERSHIP_BEGIN\r\n");
    if (!f->grabOwnership(g_nvkms_display.device))
        goto fail;
    nvkms_trace("OS_NVKMS_GRAB_OWNERSHIP_DONE\r\n");
    g_nvkms_display.owns_device = true;

    memset(&resources, 0, sizeof(resources));
    nvkms_trace("OS_NVKMS_RESOURCES_BEGIN\r\n");
    if (!f->getDeviceResourcesInfo(g_nvkms_display.device, &resources) ||
        resources.numHeads == 0U || resources.numHeads > NVKMS_KAPI_MAX_HEADS ||
        resources.numLayers[0] == 0U)
        goto fail;
    nvkms_trace("OS_NVKMS_RESOURCES_DONE\r\n");
    if ((resources.supportedSurfaceMemoryFormats[0] &
         (1ULL << NvKmsSurfaceMemoryFormatX8R8G8B8)) == 0U ||
        (resources.caps.layer[NVKMS_KAPI_LAYER_PRIMARY_IDX].validCompositionModes &
         (1U << NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE)) == 0U)
        goto fail;
    if (!nvkms_select_display(&g_nvkms_display, &resources))
        goto fail;

    alignment = resources.caps.pitchAlignment;
    if (alignment < sizeof(uint32_t))
        alignment = sizeof(uint32_t);
    row_bytes = (uint64_t)g_nvkms_display.width * sizeof(uint32_t);
    aligned_pitch = row_bytes;
    if (aligned_pitch % alignment != 0U)
        aligned_pitch += alignment - (aligned_pitch % alignment);
    if (aligned_pitch > UINT32_MAX ||
        aligned_pitch > UINT64_MAX / g_nvkms_display.height)
        goto fail;
    g_nvkms_display.pitch = (uint32_t)aligned_pitch;
    g_nvkms_display.has_video_memory = resources.caps.hasVideoMemory;

    nvkms_trace("OS_NVKMS_SURFACE_CURRENT_BEGIN\r\n");
    status = nvkms_surface_create(&g_nvkms_display,
                                  &g_nvkms_display.current);
    if (status != K_OK)
        goto fail;
    nvkms_trace("OS_NVKMS_SURFACE_CURRENT_DONE\r\n");
    nvkms_trace("OS_NVKMS_SURFACE_SPARE_BEGIN\r\n");
    status = nvkms_surface_create(&g_nvkms_display,
                                  &g_nvkms_display.spare);
    if (status != K_OK)
        goto fail;
    nvkms_trace("OS_NVKMS_SURFACE_SPARE_DONE\r\n");
    nvkms_trace("OS_NVKMS_GOP_COPY_BEGIN\r\n");
    status = nvkms_copy_gop_to_surface(&g_nvkms_display,
                                       &g_nvkms_display.current, gop_stride);
    if (status != K_OK)
        goto fail;
    nvkms_trace("OS_NVKMS_GOP_COPY_DONE\r\n");

    {
        struct NvKmsKapiRequestedModeSetConfig config;
        nvkms_fill_config(&g_nvkms_display, &config,
                          g_nvkms_display.current.surface, true);
        kernel_serial_printf_serial_only(
            "OS_NVKMS_MODESET_CONFIG head=%u display=%u active=%u "
            "displays=%u surface=%u size=%ux%u pitch=%u clock=%u "
            "refresh=%u h=%u/%u/%u/%u v=%u/%u/%u/%u olut=%u\r\n",
            g_nvkms_display.head, g_nvkms_display.display,
            config.headRequestedConfig[g_nvkms_display.head]
                .modeSetConfig.bActive ? 1U : 0U,
            config.headRequestedConfig[g_nvkms_display.head]
                .modeSetConfig.numDisplays,
            g_nvkms_display.current.surface != NULL ?
                1U : 0U,
            g_nvkms_display.width, g_nvkms_display.height,
            g_nvkms_display.pitch,
            g_nvkms_display.mode.timings.pixelClockHz,
            g_nvkms_display.mode.timings.refreshRate,
            g_nvkms_display.mode.timings.hVisible,
            g_nvkms_display.mode.timings.hSyncStart,
            g_nvkms_display.mode.timings.hSyncEnd,
            g_nvkms_display.mode.timings.hTotal,
            g_nvkms_display.mode.timings.vVisible,
            g_nvkms_display.mode.timings.vSyncStart,
            g_nvkms_display.mode.timings.vSyncEnd,
            g_nvkms_display.mode.timings.vTotal,
            config.headRequestedConfig[g_nvkms_display.head]
                .modeSetConfig.olutFpNormScale);
        (void)realtest_flush();
        nvkms_trace("OS_NVKMS_MODESET_BEGIN\r\n");
        status = nvkms_apply_config(&g_nvkms_display, &config);
        kernel_serial_printf_serial_only(
            "OS_NVKMS_MODESET_DONE status=%d\r\n", (int)status);
        (void)realtest_flush();
        if (status != K_OK)
            goto fail;
    }
    nvkms_trace("OS_NVKMS_EVENTS_BEGIN\r\n");
    if (f->declareEventInterest != NULL &&
        !f->declareEventInterest(
            g_nvkms_display.device,
            (1U << NVKMS_EVENT_TYPE_FLIP_OCCURRED) |
            (1U << NVKMS_EVENT_TYPE_DPY_CHANGED) |
            (1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED)))
        goto fail;
    nvkms_trace("OS_NVKMS_EVENTS_DONE\r\n");
    if (f->framebufferConsoleDisabled != NULL)
        f->framebufferConsoleDisabled(g_nvkms_display.device);

    g_nvkms_display.backend.context = &g_nvkms_display;
    g_nvkms_display.backend.poll = nvkms_poll;
    g_nvkms_display.backend.atomic_commit = nvkms_atomic_commit;
    nvkms_trace("OS_NVKMS_BACKEND_REGISTER_BEGIN\r\n");
    status = display_core_register_scanout_backend(0U,
                                                   &g_nvkms_display.backend);
    kernel_serial_printf_serial_only(
        "OS_NVKMS_BACKEND_REGISTER_DONE status=%d\r\n", (int)status);
    (void)realtest_flush();
    if (status != K_OK)
        goto fail;
    g_nvkms_display.gpu_id = pick.gpu_id;
    g_nvkms_display.initialized = true;
    kernel_serial_printf_serial_only(
        "OS_NVKMS_NATIVE_MODESET_OK gpu=%u head=%u width=%u height=%u pitch=%u\r\n",
        g_nvkms_display.gpu_id, g_nvkms_display.head,
        g_nvkms_display.width, g_nvkms_display.height,
        g_nvkms_display.pitch);
    return K_OK;

fail:
    nvkms_display_cleanup(&g_nvkms_display);
    if (status == K_OK)
        status = K_EIO;
    kernel_serial_printf_serial_only(
        "OS_NVKMS_NATIVE_MODESET_FAIL status=%d\r\n", (int)status);
    return status;
}

bool nv_nvkms_display_active(void)
{
    return g_nvkms_display.initialized;
}
