#include <usb/storage.h>

#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>

#include <stdatomic.h>

#define USB_MSC_MAX_SLOT          256U
#define USB_MSC_CBW_SIGNATURE     0x43425355U
#define USB_MSC_CSW_SIGNATURE     0x53425355U
#define USB_MSC_CBW_FLAG_IN       0x80U

#define SCSI_TEST_UNIT_READY      0x00U
#define SCSI_INQUIRY              0x12U
#define SCSI_READ_CAPACITY_10     0x25U
#define SCSI_READ_10              0x28U
#define SCSI_WRITE_10             0x2AU
#define SCSI_SYNCHRONIZE_CACHE_10 0x35U
#define USB_MSC_LOCK_TIMEOUT_NS   1000000000ULL

typedef struct __attribute__((packed)) usb_msc_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t command_length;
    uint8_t command[16];
} usb_msc_cbw_t;

typedef struct __attribute__((packed)) usb_msc_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} usb_msc_csw_t;

_Static_assert(sizeof(usb_msc_cbw_t) == 31U, "USB MSC CBW ABI");
_Static_assert(sizeof(usb_msc_csw_t) == 13U, "USB MSC CSW ABI");

typedef struct usb_msc_device {
    atomic_bool busy;
    atomic_bool online;
    atomic_uint busy_owner_cpu;
    uint8_t slot;
    uint8_t interface_number;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t lun;
    uint32_t next_tag;
    uint32_t block_size;
    uint64_t block_count;
    OS_BLOCK_DEVICE *block_device;
} usb_msc_device_t;

static usb_msc_device_t g_usb_msc[USB_MSC_MAX_SLOT];
static OS_BLOCK_MANAGER g_usb_msc_block_manager;
static atomic_uint g_usb_msc_manager_state;

static void usb_msc_record_status(const char *stage, kstatus_t status) {
    uint32_t value = status < 0 ? (uint32_t)(-status) : (uint32_t)status;
    realtest_mark_number(stage, value);
}

static uint32_t msc_be32(const uint8_t value[4]) {
    return ((uint32_t)value[0] << 24) |
           ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) |
           (uint32_t)value[3];
}

static void msc_store_be16(uint8_t value[2], uint16_t data) {
    value[0] = (uint8_t)(data >> 8);
    value[1] = (uint8_t)data;
}

static void msc_store_be32(uint8_t value[4], uint32_t data) {
    value[0] = (uint8_t)(data >> 24);
    value[1] = (uint8_t)(data >> 16);
    value[2] = (uint8_t)(data >> 8);
    value[3] = (uint8_t)data;
}

static bool usb_msc_manager_init_once(void) {
    unsigned state = atomic_load_explicit(&g_usb_msc_manager_state,
                                          memory_order_acquire);
    if (state == 2U) return true;

    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &g_usb_msc_manager_state, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        bool ok = block_manager_init(&g_usb_msc_block_manager) != 0;
        atomic_store_explicit(&g_usb_msc_manager_state, ok ? 2U : 3U,
                              memory_order_release);
        return ok;
    }

    while ((state = atomic_load_explicit(&g_usb_msc_manager_state,
                                         memory_order_acquire)) == 1U) {
        __asm__ volatile ("pause");
    }
    return state == 2U;
}

/*
 * BOT is strictly single-command-at-a-time for one interface.  `busy` used to
 * behave like a try-lock, so a second filesystem request racing the current
 * request was reported as an I/O error.  That is especially easy to hit while
 * the desktop loads files and writes state concurrently.
 *
 * Wait for the current BOT transaction instead.  Do not disable preemption
 * here: the owner may be running on this CPU and must be allowed to complete.
 */
static bool usb_msc_enter(usb_msc_device_t *device) {
    uint64_t deadline;

    if (device == 0) return false;
    deadline = x86_read_tsc() +
               x86_timeout_ns_to_tsc(USB_MSC_LOCK_TIMEOUT_NS);

    for (;;) {
        if (!atomic_load_explicit(&device->online, memory_order_acquire)) {
            return false;
        }

        bool expected = false;
        if (atomic_compare_exchange_weak_explicit(
                &device->busy, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&device->busy_owner_cpu,
                                  x86_current_cpu_index(),
                                  memory_order_release);
            break;
        }
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
            realtest_mark_number("USB_MSC_BUSY_TIMEOUT", device->slot);
            realtest_mark_number(
                "USB_MSC_BUSY_OWNER",
                atomic_load_explicit(&device->busy_owner_cpu,
                                     memory_order_acquire));
            realtest_mark_number("USB_MSC_BUSY_ONLINE",
                atomic_load_explicit(&device->online, memory_order_acquire) ?
                1U : 0U);
            return false;
        }
        __asm__ volatile ("pause");
    }

    /* Detach can race the acquisition.  Never start a new command offline. */
    if (!atomic_load_explicit(&device->online, memory_order_acquire)) {
        atomic_store_explicit(&device->busy_owner_cpu, UINT32_MAX,
                              memory_order_release);
        atomic_store_explicit(&device->busy, false, memory_order_release);
        return false;
    }
    return true;
}

static void usb_msc_leave(usb_msc_device_t *device) {
    atomic_store_explicit(&device->busy_owner_cpu, UINT32_MAX,
                          memory_order_release);
    atomic_store_explicit(&device->busy, false, memory_order_release);
}

static kstatus_t usb_msc_bot_locked(usb_msc_device_t *device,
                                    const uint8_t *command,
                                    uint8_t command_length,
                                    void *data,
                                    uint32_t data_length,
                                    bool direction_in,
                                    uint32_t *actual_data) {
    usb_msc_cbw_t cbw = {0};
    usb_msc_csw_t csw = {0};
    uint32_t actual = 0U;
    uint32_t data_actual = 0U;
    kstatus_t status;

    if (device == 0 || command == 0 || command_length == 0U ||
        command_length > sizeof(cbw.command) ||
        (data_length != 0U && data == 0)) {
        return K_EINVAL;
    }

    if (xhci_usb_msc_is_uas(device->slot)) {
        return xhci_usb_msc_scsi_command(
            device->slot, command, command_length, data, data_length,
            direction_in, actual_data);
    }

    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++device->next_tag;
    if (cbw.tag == 0U) cbw.tag = ++device->next_tag;
    cbw.transfer_length = data_length;
    cbw.flags = direction_in ? USB_MSC_CBW_FLAG_IN : 0U;
    cbw.lun = device->lun;
    cbw.command_length = command_length;
    for (uint32_t i = 0U; i < command_length; ++i) cbw.command[i] = command[i];

    /* Keep one Event Ring consumer for the complete CBW -> DATA -> CSW triplet. */
    status = xhci_usb_bulk_session_begin(device->slot);
    if (status != K_OK) return status;

    status = xhci_usb_bulk_transfer_locked(
        device->slot, device->bulk_out, false,
        &cbw, sizeof(cbw), &actual);
    if (status != K_OK || actual != sizeof(cbw)) {
        realtest_mark_number("USB_MSC_BOT_FAIL_STAGE", 1U);
        usb_msc_record_status("USB_MSC_BOT_FAIL_STATUS", status);
        usb_msc_record_status("USB_MSC_BOT_FAIL_ACTUAL", (kstatus_t)actual);
        if (status == K_OK) status = K_EIO;
        goto done;
    }

    if (data_length != 0U) {
        status = xhci_usb_bulk_transfer_locked(
            device->slot,
            direction_in ? device->bulk_in : device->bulk_out,
            direction_in, data, data_length, &data_actual);
        if (status != K_OK) {
            realtest_mark_number("USB_MSC_BOT_FAIL_STAGE", 2U);
            usb_msc_record_status("USB_MSC_BOT_FAIL_STATUS", status);
            goto done;
        }
    }

    actual = 0U;
    status = xhci_usb_bulk_transfer_locked(
        device->slot, device->bulk_in, true,
        &csw, sizeof(csw), &actual);
    if (status != K_OK || actual != sizeof(csw)) {
        realtest_mark_number("USB_MSC_BOT_FAIL_STAGE", 3U);
        usb_msc_record_status("USB_MSC_BOT_FAIL_STATUS", status);
        usb_msc_record_status("USB_MSC_BOT_FAIL_ACTUAL", (kstatus_t)actual);
        if (status == K_OK) status = K_EIO;
        goto done;
    }

    if (csw.signature != USB_MSC_CSW_SIGNATURE ||
        csw.tag != cbw.tag || csw.status != 0U ||
        csw.residue > data_length) {
        realtest_mark_number("USB_MSC_BOT_FAIL_STAGE", 4U);
        status = K_EIO;
        goto done;
    }

    if (actual_data != 0) *actual_data = data_actual;

done:
    xhci_usb_bulk_session_end(device->slot);
    return status;
}

static kstatus_t usb_msc_test_unit_ready(usb_msc_device_t *device) {
    uint8_t command[6] = {SCSI_TEST_UNIT_READY, 0U, 0U, 0U, 0U, 0U};
    return usb_msc_bot_locked(device, command, sizeof(command),
                              0, 0U, false, 0);
}

static kstatus_t usb_msc_inquiry(usb_msc_device_t *device) {
    uint8_t command[6] = {SCSI_INQUIRY, 0U, 0U, 0U, 36U, 0U};
    uint8_t response[36] = {0};
    uint32_t actual = 0U;
    kstatus_t status = usb_msc_bot_locked(
        device, command, sizeof(command),
        response, sizeof(response), true, &actual);
    if (status != K_OK) return status;
    if (actual < 5U || (response[0] & 0x1FU) != 0U) return K_EIO;
    return K_OK;
}

static kstatus_t usb_msc_read_capacity(usb_msc_device_t *device) {
    uint8_t command[10] = {SCSI_READ_CAPACITY_10, 0U, 0U, 0U, 0U,
                           0U, 0U, 0U, 0U, 0U};
    uint8_t response[8] = {0};
    uint32_t actual = 0U;
    kstatus_t status = usb_msc_bot_locked(
        device, command, sizeof(command),
        response, sizeof(response), true, &actual);
    if (status != K_OK || actual != sizeof(response))
        return status == K_OK ? K_EIO : status;

    uint32_t last_lba = msc_be32(response);
    uint32_t block_size = msc_be32(response + 4U);
    if (last_lba == UINT32_MAX || block_size < 512U ||
        block_size > PAGE_SIZE ||
        (block_size & (block_size - 1U)) != 0U) {
        return K_EOVERFLOW;
    }

    device->block_size = block_size;
    device->block_count = (uint64_t)last_lba + 1ULL;
    return K_OK;
}

static kstatus_t usb_msc_rw_blocks(usb_msc_device_t *device,
                                  uint64_t lba,
                                  uint32_t count,
                                  void *buffer,
                                  bool write) {
    uint8_t command[10] = {0};
    uint32_t actual = 0U;

    if (device == 0 || buffer == 0 || device->block_size == 0U ||
        device->block_size > PAGE_SIZE || count == 0U ||
        count > XHCI_MSC_MAX_TRANSFER / device->block_size || count > UINT16_MAX ||
        lba >= device->block_count || count > device->block_count - lba ||
        lba > UINT32_MAX || (uint64_t)count - 1U > UINT32_MAX - lba)
        return K_EINVAL;
    uint32_t length = count * device->block_size;

    command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    msc_store_be32(command + 2U, (uint32_t)lba);
    msc_store_be16(command + 7U, (uint16_t)count);

    kstatus_t status = usb_msc_bot_locked(
        device, command, sizeof(command),
        buffer, length, !write, &actual);
    if (status != K_OK) return status;
    if (actual != length) return K_EIO;
    return K_OK;
}

static BOOLEAN usb_msc_block_io(VOID *context, UINT64 lba,
                                UINT32 count, VOID *buffer, bool write) {
    usb_msc_device_t *device = (usb_msc_device_t *)context;
    if (device == 0 || buffer == 0 || count == 0U ||
        device->block_size == 0U || device->block_size > PAGE_SIZE ||
        lba >= device->block_count || count > device->block_count - lba ||
        lba > UINT32_MAX || (uint64_t)count - 1U > UINT32_MAX - lba ||
        !usb_msc_enter(device)) return 0;

    bool ok = true;
    uint64_t requested = (uint64_t)count * device->block_size;
    if (requested > XHCI_MSC_MAX_TRANSFER) requested = XHCI_MSC_MAX_TRANSFER;
    uint32_t capacity = xhci_usb_msc_prepare_buffer(device->slot, (uint32_t)requested);
    uint32_t maximum = capacity / device->block_size;
    if (maximum > UINT16_MAX) maximum = UINT16_MAX;
    if (maximum == 0U) { usb_msc_leave(device); return 0; }
    /* One SCSI request for the largest contiguous run the transport can
     * accept. xHCI chains DMA segments without copying eligible buffers.
     * Each command releases the event-ring lock for HID progress. */
    for (uint32_t i = 0U; ok && i < count;) {
        uint32_t chunk = count - i;
        if (chunk > maximum) chunk = maximum;
        ok = usb_msc_rw_blocks(device, lba + i, chunk,
                 (uint8_t *)buffer + (uint64_t)i * device->block_size,
                 write) == K_OK;
        i += chunk;
    }
    usb_msc_leave(device);
    return ok ? 1 : 0;
}

static BOOLEAN usb_msc_block_read(VOID *context, UINT64 lba,
                                  UINT32 count, VOID *buffer) {
    return usb_msc_block_io(context, lba, count, buffer, false);
}

static BOOLEAN usb_msc_block_write(VOID *context, UINT64 lba,
                                   UINT32 count, const VOID *buffer) {
    return usb_msc_block_io(context, lba, count, (void *)buffer, true);
}

static BOOLEAN usb_msc_block_flush(VOID *context) {
    usb_msc_device_t *device = (usb_msc_device_t *)context;
    uint8_t command[10] = {SCSI_SYNCHRONIZE_CACHE_10, 0U, 0U, 0U, 0U,
                           0U, 0U, 0U, 0U, 0U};
    if (device == 0 || !usb_msc_enter(device)) return 0;
    bool ok = usb_msc_bot_locked(device, command, sizeof(command),
                                 0, 0U, false, 0) == K_OK;
    usb_msc_leave(device);
    return ok ? 1 : 0;
}

static void usb_msc_make_name(uint8_t slot,
                              CHAR8 name[OS_BLOCK_NAME_LENGTH]) {
    uint32_t pos = 0U;
    name[pos++] = 'u'; name[pos++] = 's'; name[pos++] = 'b';
    uint32_t value = slot;
    char digits[3];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U && pos + 1U < OS_BLOCK_NAME_LENGTH)
        name[pos++] = (CHAR8)digits[--count];
    name[pos] = 0;
}

static void usb_msc_attach_work(void *argument) {
    uint8_t slot = (uint8_t)(uintptr_t)argument;
    uint8_t interface_number = 0U;
    uint8_t bulk_in = 0U;
    uint8_t bulk_out = 0U;
    CHAR8 name[OS_BLOCK_NAME_LENGTH] = {0};

    if (slot == 0U ||
        !xhci_usb_msc_query(slot, &interface_number, &bulk_in, &bulk_out))
        return;

    realtest_mark_number("USB_MSC_ATTACH_BEGIN", slot);

    usb_msc_device_t *device = &g_usb_msc[slot];
    if (atomic_exchange_explicit(&device->busy, true,
                                 memory_order_acq_rel)) return;
    atomic_store_explicit(&device->busy_owner_cpu, x86_current_cpu_index(),
                          memory_order_release);
    if (atomic_load_explicit(&device->online, memory_order_acquire)) {
        usb_msc_leave(device);
        return;
    }

    device->slot = slot;
    device->interface_number = interface_number;
    device->bulk_in = bulk_in;
    device->bulk_out = bulk_out;
    device->lun = 0U;
    device->next_tag = ((uint32_t)slot << 24) | 1U;
    device->block_size = 0U;
    device->block_count = 0U;
    device->block_device = 0;

    kstatus_t status = usb_msc_inquiry(device);
    usb_msc_record_status("USB_MSC_INQUIRY_STATUS", status);
    if (status != K_OK && xhci_usb_msc_is_uas(slot)) {
        for (uint32_t retry = 0U; retry < 2U && status != K_OK; ++retry) {
            if (!xhci_usb_msc_recover_uas(slot)) break;
            realtest_mark_number("USB_MSC_UAS_RETRY", retry + 1U);
            status = usb_msc_inquiry(device);
            usb_msc_record_status("USB_MSC_UAS_RETRY_STATUS", status);
        }
    }
    if (status != K_OK && xhci_usb_msc_is_uas(slot)) {
        realtest_mark_number("USB_MSC_UAS_FALLBACK_STATUS",
                                    (uint32_t)(-status));
        if (xhci_usb_msc_fallback_to_bot(slot) &&
            xhci_usb_msc_query(slot, &interface_number, &bulk_in, &bulk_out)) {
            device->interface_number = interface_number;
            device->bulk_in = bulk_in;
            device->bulk_out = bulk_out;
            status = usb_msc_inquiry(device);
            usb_msc_record_status("USB_MSC_BOT_INQUIRY_STATUS", status);
        }
    }
    if (status == K_OK) {
        status = K_EIO;
        for (uint32_t retry = 0U; retry < 8U; ++retry) {
            status = usb_msc_test_unit_ready(device);
            if (status == K_OK) break;
            __asm__ volatile ("pause");
        }
        usb_msc_record_status("USB_MSC_READY_STATUS", status);
    }
    if (status == K_OK) {
        status = usb_msc_read_capacity(device);
        usb_msc_record_status("USB_MSC_CAPACITY_STATUS", status);
    }

    if (status != K_OK) {
        realtest_mark_number("USB_MSC_SCSI_FAIL", (uint32_t)(-status));
        kernel_serial_write("OS_USB_MSC_SCSI_FAIL\r\n");
        usb_msc_leave(device);
        return;
    }
    if (!usb_msc_manager_init_once()) {
        realtest_mark("USB_MSC_MANAGER_FAIL");
        kernel_serial_write("OS_USB_MSC_SCSI_FAIL\r\n");
        usb_msc_leave(device);
        return;
    }

    usb_msc_make_name(slot, name);
    if (!block_register(&g_usb_msc_block_manager, name,
            device->block_size, device->block_count,
            usb_msc_block_read, usb_msc_block_write, usb_msc_block_flush,
            device, &device->block_device)) {
        kernel_serial_write("OS_USB_MSC_BLOCK_REGISTER_FAIL\r\n");
        usb_msc_leave(device);
        return;
    }

    realtest_mark("USB_MSC_BLOCK_REGISTER_OK");

    atomic_store_explicit(&device->online, true, memory_order_release);
    usb_msc_leave(device);

    kernel_serial_write("OS_USB_MSC_FOUND SLOT=");
    kernel_serial_write_u32(slot);
    kernel_serial_write(" BLOCK_SIZE=");
    kernel_serial_write_u32(device->block_size);
    kernel_serial_write(" BLOCK_COUNT_LOW=");
    kernel_serial_write_u32((uint32_t)device->block_count);
    kernel_serial_write("\r\nOS_USB_MSC_BLOCK_OK\r\n");
}

/*
 * OS_USB_ROOT_PATCH_V1
 *
 * Boot can need the same USB Mass Storage device that UEFI used to load
 * BOOTX64.EFI before the persistent deferred worker has started.
 *
 * The normal hotplug path remains deferred.  This helper reuses the exact
 * same SCSI/BOT attach routine synchronously during root discovery.
 */
bool usb_msc_attach(uint8_t slot) {
    if (slot == 0U) return false;
    usb_msc_attach_work((void *)(uintptr_t)slot);
    return usb_msc_present(slot);
}

bool usb_msc_schedule_attach(uint8_t slot) {
    if (slot == 0U) return false;
    return deferred_schedule(usb_msc_attach_work, (void *)(uintptr_t)slot);
}

void usb_msc_detach(uint8_t slot) {
    if (slot == 0U) return;
    usb_msc_device_t *device = &g_usb_msc[slot];
    bool was_online = atomic_exchange_explicit(&device->online, false,
                                               memory_order_acq_rel);
    if (device->block_device != 0) {
        (void)block_unregister(&g_usb_msc_block_manager,
                                      device->block_device);
        device->block_device = 0;
    }
    device->block_size = 0U;
    device->block_count = 0U;
    if (was_online) {
        kernel_serial_write("OS_USB_MSC_REMOVED SLOT=");
        kernel_serial_write_u32(slot);
        kernel_serial_write("\r\n");
    }
}

bool usb_msc_present(uint8_t slot) {
    return slot != 0U &&
           atomic_load_explicit(&g_usb_msc[slot].online,
                                memory_order_acquire);
}

OS_BLOCK_DEVICE *usb_msc_block_device(uint8_t slot) {
    return usb_msc_present(slot) ? g_usb_msc[slot].block_device : 0;
}
