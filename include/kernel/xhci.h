#pragma once

#include <kernel/base.h>

struct audio_stream;
struct device;

/* xHCI 控制器后端：控制器没有出现时允许无 USB 的 QEMU/物理机继续启动。 */
bool xhci_hardware_present(void);
bool xhci_hardware_self_test(void);
bool xhci_event_queue_self_test(void);
bool xhci_transfer_encode_self_test(void);
bool xhci_device_context_self_test(void);
bool xhci_endpoint_context_self_test(void);
bool xhci_usb_device_enumerated(void);
uint32_t xhci_usb_device_count(void);
bool xhci_usb_hid_configured(void);
bool xhci_usb_keyboard_configured(void);
bool xhci_usb_mouse_configured(void);
bool xhci_usb_audio_configured(void);
bool xhci_usb_hub_configured(void);
uint8_t xhci_usb_hub_port_count(void);
bool xhci_usb_hub_downstream_configured(void);
bool xhci_usb_bluetooth_configured(void);
bool xhci_usb_mass_storage_configured(void);
bool xhci_usb_msc_query(uint8_t slot, uint8_t *interface_number,
                        uint8_t *bulk_in, uint8_t *bulk_out);
bool xhci_usb_msc_is_uas(uint8_t slot);
/* Grow/reuse the DMA staging area; return usable bytes, or zero on failure.
 * Caller serializes commands for this storage interface. */
uint32_t xhci_usb_msc_prepare_buffer(uint8_t slot, uint32_t requested);
#define XHCI_MSC_MAX_TRANSFER (1024U * 1024U)
bool xhci_usb_msc_recover_uas(uint8_t slot);
bool xhci_usb_msc_fallback_to_bot(uint8_t slot);
kstatus_t xhci_usb_msc_scsi_command(uint8_t slot, const uint8_t *command,
                                    uint8_t command_length, void *buffer,
                                    uint32_t length, bool direction_in,
                                    uint32_t *actual);

/* BOT keeps the Event Ring consumer across CBW/DATA/CSW. */
kstatus_t xhci_usb_bulk_session_begin(uint8_t slot);
void xhci_usb_bulk_session_end(uint8_t slot);
kstatus_t xhci_usb_bulk_transfer_locked(uint8_t slot, uint8_t endpoint,
                                        bool direction_in, void *buffer,
                                        uint32_t length, uint32_t *actual);
uint32_t xhci_usb_audio_completed(void);
struct device *xhci_audio_device(void);
kstatus_t xhci_audio_stream_configure(struct audio_stream *stream);
kstatus_t xhci_audio_stream_start(struct audio_stream *stream);
kstatus_t xhci_audio_stream_stop(struct audio_stream *stream);
kstatus_t xhci_audio_stream_queue(struct audio_stream *stream, uint32_t period,
                                  uint32_t frames);
kstatus_t xhci_audio_stream_reset(struct audio_stream *stream);
kstatus_t xhci_audio_stream_disconnect(struct audio_stream *stream);
bool xhci_process_events(uint32_t budget);
bool xhci_schedule_deferred_work(void);
void xhci_deferred_work(void *argument);
uint32_t xhci_last_error(void);

kstatus_t xhci_audio_stream_complete(struct audio_stream *stream, uint32_t period, uint32_t frames);

/* USB 音频端点按枚举顺序选择；已有流绑定时禁止切换上下文。 */
uint32_t xhci_audio_device_count(void);
kstatus_t xhci_audio_select_device(uint32_t ordinal);
