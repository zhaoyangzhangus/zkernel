#pragma once

/* 控制面允许休眠和持久化；播放热路径只读取一个原子快照。 */
static io_mutex_t g_audio_settings_lock;
static atomic_uint g_audio_settings_init;
static atomic_uint_fast64_t g_audio_settings_word = (100U << 8U);
static atomic_bool g_audio_settings_loaded;

static uint32_t audio_settings_available(void) {
    uint32_t result = 0U;
    device_t *hda = hda_audio_device(), *usb = xhci_audio_device();
    if (hda && atomic_load_explicit(&hda->state, memory_order_acquire) < DEVICE_REMOVING)
        result |= OS_AUDIO_DEVICE_HDA;
    if (usb && atomic_load_explicit(&usb->state, memory_order_acquire) < DEVICE_REMOVING)
        result |= OS_AUDIO_DEVICE_USB;
    if (xhci_audio_device_count()>1U) result |= OS_AUDIO_DEVICE_USB_HEADSET;
    return result;
}

static bool audio_settings_valid(const os_audio_settings_t *settings) {
    return settings->backend <= OS_AUDIO_BACKEND_USB_HEADSET &&
        settings->volume <= 100U && settings->muted <= 1U;
}

static void audio_settings_snapshot(os_audio_settings_t *settings) {
    uint64_t word = atomic_load_explicit(&g_audio_settings_word, memory_order_acquire);
    settings->backend = word & 0xFFU;
    settings->volume = (word >> 8U) & 0xFFU;
    settings->muted = (word >> 16U) & 1U;
    settings->generation = word >> 32U;
    settings->available = audio_settings_available();
    settings->effective_backend = settings->backend;
    if (settings->backend == OS_AUDIO_BACKEND_AUTO)
        settings->effective_backend = settings->available & OS_AUDIO_DEVICE_HDA ?
            OS_AUDIO_BACKEND_HDA : settings->available & OS_AUDIO_DEVICE_USB ?
            OS_AUDIO_BACKEND_USB : OS_AUDIO_BACKEND_AUTO;
}

static void audio_settings_store(const os_audio_settings_t *settings) {
    uint64_t previous = atomic_load_explicit(&g_audio_settings_word, memory_order_relaxed);
    uint64_t word = (((previous >> 32U) + 1U) << 32U) |
        settings->backend | ((uint64_t)settings->volume << 8U) |
        ((uint64_t)settings->muted << 16U);
    atomic_store_explicit(&g_audio_settings_word, word, memory_order_release);
}

/* 不在持有自旋锁时访问磁盘；临时文件完整落盘后才替换旧配置。 */
static kstatus_t audio_settings_save(const os_audio_settings_t *settings) {
    file_t *file = 0;
    uint64_t bytes = 0;
    kstatus_t status = vfs_open_kernel("/EFI/OS/audio-settings.tmp",
        OS_FILE_OPEN_WRITE | OS_FILE_OPEN_CREATE | OS_FILE_OPEN_TRUNCATE, 0644U, &file);
    if (status != K_OK) return status;
    status = vfs_write_kernel(file, settings, sizeof(*settings), &bytes);
    if (status == K_OK && bytes != sizeof(*settings)) status = K_EIO;
    if (status == K_OK) status = vfs_fsync(file);
    vfs_close(file);
    if (status == K_OK) status = vfs_rename_kernel("/EFI/OS/audio-settings.tmp", "/EFI/OS/audio-settings");
    if (status != K_OK) (void)vfs_remove_kernel("/EFI/OS/audio-settings.tmp");
    return status;
}

static kstatus_t audio_settings_lock_and_load(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_audio_settings_init, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        io_mutex_init(&g_audio_settings_lock, true);
        atomic_store_explicit(&g_audio_settings_init, 2U, memory_order_release);
    }
    while (atomic_load_explicit(&g_audio_settings_init, memory_order_acquire) != 2U)
        __asm__ volatile("pause");
    kstatus_t status = io_mutex_lock(&g_audio_settings_lock);
    if (status != K_OK) return status;
    if (!atomic_load_explicit(&g_audio_settings_loaded, memory_order_acquire)) {
        file_t *file = 0;
        os_audio_settings_t saved = {0};
        uint64_t bytes = 0U;
        if (vfs_open_kernel("/EFI/OS/audio-settings", OS_FILE_OPEN_READ, 0U, &file) == K_OK) {
            if (vfs_read_kernel(file, &saved, sizeof(saved), &bytes) == K_OK &&
                bytes == sizeof(saved) && versioned_header_valid(&saved.hdr, sizeof(saved)) &&
                audio_settings_valid(&saved)) audio_settings_store(&saved);
            vfs_close(file);
        }
        atomic_store_explicit(&g_audio_settings_loaded, true, memory_order_release);
    }
    return K_OK;
}

static kstatus_t audio_settings_ensure_loaded(void) {
    if (atomic_load_explicit(&g_audio_settings_loaded, memory_order_acquire)) return K_OK;
    kstatus_t status = audio_settings_lock_and_load();
    if (status == K_OK) io_mutex_unlock_required(&g_audio_settings_lock);
    return status;
}

int64_t syscall_audio_settings(uint64_t pointer, uint64_t unused1, uint64_t unused2,
        uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    os_audio_settings_t settings;
    kstatus_t status = copy_from_user(&settings, (const void __user *)(uintptr_t)pointer, sizeof(settings));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&settings.hdr, sizeof(settings)) ||
        settings.operation > OS_AUDIO_SETTINGS_WRITE ||
        (settings.operation == OS_AUDIO_SETTINGS_WRITE && !audio_settings_valid(&settings))) return K_EINVAL;
    if (settings.operation == OS_AUDIO_SETTINGS_READ &&
        atomic_load_explicit(&g_audio_settings_loaded, memory_order_acquire)) {
        audio_settings_snapshot(&settings);
        return copy_to_user((void __user *)(uintptr_t)pointer, &settings, sizeof(settings));
    }
    status = audio_settings_lock_and_load();
    if (status != K_OK) return status;
    if (settings.operation == OS_AUDIO_SETTINGS_WRITE) {
        uint32_t available = audio_settings_available();
        if (settings.backend && !(available & (1U << settings.backend))) status = K_ENOENT;
        else {
            status = audio_settings_save(&settings);
            if (status == K_OK) audio_settings_store(&settings);
        }
    }
    audio_settings_snapshot(&settings);
    io_mutex_unlock_required(&g_audio_settings_lock);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)pointer, &settings, sizeof(settings));
}

/* 当前物理输出支持有符号整数 PCM，主音量同时作用于所有原生播放流。 */
static void audio_settings_apply_gain(void *buffer, uint64_t bytes, uint16_t format) {
    uint64_t word = atomic_load_explicit(&g_audio_settings_word, memory_order_acquire);
    uint32_t gain = word & (1U << 16U) ? 0U : (uint32_t)((word >> 8U) & 0xFFU);
    if (gain == 100U) return;
    uint32_t width = format == OS_AUDIO_SAMPLE_S16_LE ? 2U :
        format == OS_AUDIO_SAMPLE_S24_LE ? 3U : format == OS_AUDIO_SAMPLE_S32_LE ? 4U : 0U;
    if (!width) return;
    uint8_t *data = buffer;
    for (uint64_t offset = 0U; offset + width <= bytes; offset += width) {
        uint32_t raw = 0U;
        for (uint32_t i = 0; i < width; ++i) raw |= (uint32_t)data[offset+i] << (i*8U);
        int64_t sample = raw;
        if (raw & (1U << (width*8U-1U))) sample -= (1LL << (width*8U));
        uint32_t scaled = (uint32_t)(sample * gain / 100);
        for (uint32_t i = 0; i < width; ++i) data[offset+i] = (uint8_t)(scaled >> (i*8U));
    }
}
