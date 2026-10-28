#pragma once

#include <kernel/audio.h>
#include <kernel/bluetooth.h>
#include <kernel/dma.h>
#include <kernel/pci.h>
#include <kernel/spinlock.h>

#include <stdbool.h>
#include <stdint.h>

struct pci_device;

#define XHCI_DEFERRED_EVENT_COUNT 32U

const struct pci_device *xhci_pci_find_controller(void);
uint32_t xhci_pci_controller_count(void);
const struct pci_device *xhci_pci_controller_at(uint32_t index);

typedef struct __attribute__((packed)) xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16U, "xHCI TRB ABI");

/* xHCI 核心和协议所有者共用的私有控制器模型。
 * 这些类型不属于内核公共 API；协议模块只依赖规范 Slot 上下文，绝不另建设备模型。 */
typedef struct xhci_dma_page {
    page_t *page;
    dma_mapping_t mapping;
    void *cpu;
} xhci_dma_page_t;

typedef struct xhci_dma_region {
    page_t *head;
    page_t **pages;
    uint32_t page_count;
    uint8_t allocation_order;
    dma_mapping_t mapping;
    void *cpu;
} xhci_dma_region_t;

/* 复合 USB 设备可能公开相互独立的 Boot HID 接口（例如鼠标和键盘）。
 * 第二个接口的环和报告状态仍保存在同一规范 Slot 上下文中。 */
typedef struct xhci_hid_endpoint {
    xhci_dma_page_t ring;
    xhci_dma_page_t report;
    uint8_t interface_number;
    uint8_t protocol;
    uint8_t endpoint;
    uint16_t max_packet;
    uint8_t interval;
    uint32_t enqueue;
    uint8_t cycle;
    bool transfer_pending;
    uint8_t previous_modifier;
    uint8_t previous_keys[6];
    uint8_t previous_buttons;
} xhci_hid_endpoint_t;
#define XHCI_DEVICE_FIELDS \
    xhci_dma_page_t input_context; \
    xhci_dma_page_t output_context; \
    xhci_dma_page_t ep0_ring; \
    xhci_dma_page_t descriptor_buffer; \
    xhci_dma_page_t hid_ring; \
    xhci_dma_page_t hid_report; \
    xhci_dma_page_t audio_ring; \
    xhci_dma_page_t audio_buffer; \
    xhci_dma_page_t bt_event_ring; \
    xhci_dma_page_t bt_event_buffer; \
    xhci_dma_page_t bt_acl_in_ring; \
    xhci_dma_page_t bt_acl_in_buffer; \
    xhci_dma_page_t bt_acl_out_ring; \
    xhci_dma_page_t bt_acl_out_buffer; \
    xhci_dma_page_t hub_ring; \
    xhci_dma_page_t hub_report; \
    uint8_t device_slot; \
    uint8_t device_port; \
    uint8_t device_speed; \
    uint8_t root_port; \
    uint8_t parent_slot; \
    uint8_t parent_port; \
    uint32_t route_string; \
    uint32_t ep0_enqueue; \
    uint8_t configuration_value; \
    uint8_t hid_interface; \
    uint8_t hid_protocol; \
    uint8_t hid_endpoint; \
    uint16_t hid_max_packet; \
    uint8_t hid_interval; \
    uint32_t hid_enqueue; \
    uint8_t hid_cycle; \
    bool hid_transfer_pending; \
    uint8_t hid_previous_modifier; \
    uint8_t hid_previous_keys[6]; \
    uint8_t hid_previous_buttons; \
    xhci_hid_endpoint_t hid_secondary; \
    uint8_t audio_protocol, audio_control_interface, audio_clock; \
    uint8_t audio_mute_unit, audio_volume_unit, audio_volume_channels; \
    bool audio_transfer_failed; \
    uint8_t audio_interface; \
    uint8_t audio_alt_setting; \
    uint8_t audio_endpoint; \
    uint8_t audio_interval; \
    uint16_t audio_max_packet; \
    uint8_t audio_channels; \
    uint8_t audio_bit_resolution; \
    uint32_t audio_sample_rate; \
    bool audio_endpoint_in; \
    uint32_t audio_enqueue; \
    uint8_t audio_cycle; \
    bool audio_transfer_pending; \
    uint32_t audio_completed; \
    uint32_t audio_stream_generation; \
    uint32_t audio_transfer_generation; \
    uint16_t audio_transfer_first; \
    uint16_t audio_transfer_packets; \
    audio_stream_t *audio_stream; \
    uint32_t audio_period; \
    uint32_t audio_frames; \
    bool audio_stream_running; \
    bool audio_stream_queued; \
    uint8_t hub_interface; \
    bool hub_interface_present; \
    uint8_t hub_port_count; \
    uint8_t hub_protocol; \
    uint8_t hub_tt_think_time; \
    bool hub_configured; \
    uint8_t hub_endpoint; \
    uint16_t hub_max_packet; \
    uint8_t hub_interval; \
    uint32_t hub_enqueue; \
    uint8_t hub_cycle; \
    bool hub_transfer_pending; \
    uint32_t hub_change_bitmap; \
    uint8_t msc_interface; \
    uint8_t msc_bulk_in_endpoint; \
    uint8_t msc_bulk_out_endpoint; \
    uint8_t msc_bulk_in_max_burst; \
    uint8_t msc_bulk_out_max_burst; \
    uint16_t msc_bulk_in_max_packet; \
    uint16_t msc_bulk_out_max_packet; \
    bool msc_configured; \
    bool msc_uas_present; \
    uint8_t msc_uas_interface; \
    uint8_t msc_uas_alternate; \
    uint8_t msc_uas_command_endpoint; \
    uint8_t msc_uas_status_endpoint; \
    uint8_t msc_uas_data_in_endpoint; \
    uint8_t msc_uas_data_out_endpoint; \
    uint8_t msc_uas_command_address; \
    uint8_t msc_uas_status_address; \
    uint8_t msc_uas_data_in_address; \
    uint8_t msc_uas_data_out_address; \
    uint8_t msc_uas_command_max_burst; \
    uint8_t msc_uas_status_max_burst; \
    uint8_t msc_uas_data_in_max_burst; \
    uint8_t msc_uas_data_out_max_burst; \
    uint8_t msc_uas_status_max_streams; \
    uint8_t msc_uas_data_in_max_streams; \
    uint8_t msc_uas_data_out_max_streams; \
    uint16_t msc_uas_command_max_packet; \
    uint16_t msc_uas_status_max_packet; \
    uint16_t msc_uas_data_in_max_packet; \
    uint16_t msc_uas_data_out_max_packet; \
    uint8_t bt_event_endpoint; \
    uint8_t bt_acl_in_endpoint; \
    uint8_t bt_acl_out_endpoint; \
    uint8_t bt_event_interval; \
    uint16_t bt_event_max_packet; \
    uint16_t bt_acl_in_max_packet; \
    uint16_t bt_acl_out_max_packet; \
    uint32_t bt_event_enqueue; \
    uint8_t bt_event_cycle; \
    bool bt_event_transfer_pending; \
    uint32_t bt_acl_in_enqueue; \
    uint8_t bt_acl_in_cycle; \
    bool bt_acl_in_transfer_pending; \
    uint32_t bt_acl_out_enqueue; \
    uint8_t bt_acl_out_cycle; \
    bool bt_acl_out_transfer_pending; \
    bt_controller_t *bt_controller;

typedef struct xhci_device_context {
    XHCI_DEVICE_FIELDS
} xhci_device_context_t;

#define XHCI_MAX_SLOT_TABLE 256U

/* 控制器分发器和协议所有者共用的 xHCI 事件字段。 */
#define XHCI_TRANSFER_EVENT_TYPE 32U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_COMPLETION_SHIFT 24U
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMMAND_RING_TYPE 9U
#define XHCI_DISABLE_SLOT_TYPE 10U

typedef struct xhci_slot_device {
    bool used;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t device_class;
    uint8_t device_protocol;
    uint8_t parent_slot;
    uint8_t parent_port;
    uint8_t root_port;
    uint32_t route_string;
    uint8_t tt_slot;
    uint8_t tt_port;
    bool tt_multi;
    bool is_hub;
    uint8_t hub_port_count;
    uint8_t hub_protocol;
    uint8_t child_slots[16];
    xhci_device_context_t context;
} xhci_slot_device_t;

typedef struct xhci_state {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    uint64_t mmio_span;
    uint32_t operational_offset;
    uint32_t runtime_offset;
    uint32_t doorbell_offset;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t context_size;
    /* 将 HCCPARAMS1.MaxPsaSize 解码为主流上下文条目数（2^(field + 1)）。
     * 与 Linux xHCI 一样，仅提供两个条目的主机视为不支持可用的 bulk stream。 */
    uint32_t max_primary_stream_array_size;
    uint16_t hci_version;
    uint32_t command_index;
    uint8_t command_cycle;
    uint32_t event_index;
    uint8_t event_cycle;
    xhci_dma_page_t dcbaa;
    xhci_dma_page_t command_ring;
    xhci_dma_page_t event_ring;
    xhci_dma_page_t erst;
    uint32_t scratchpad_count;
    xhci_dma_region_t scratchpad_array;
    xhci_dma_region_t scratchpad_buffers;
    union {
        xhci_device_context_t device;
        struct { XHCI_DEVICE_FIELDS };
    };
    xhci_trb_t deferred_events[XHCI_DEFERRED_EVENT_COUNT];
    uint32_t deferred_event_head;
    uint32_t deferred_event_tail;
    uint32_t deferred_event_count;
    spinlock_t event_lock;
    /* RFLAGS.IF 由当前持有 event_lock 的 CPU 管理。 */
    uint64_t event_lock_saved_flags;
    uint32_t event_lock_owner;
    atomic_bool event_work_waiting;
    uint8_t irq_vector;
    bool irq_msi;
    bool initialized;
} xhci_state_t;

void xhci_runtime_reset_state(void);
void xhci_runtime_stop(void);
bool xhci_runtime_ready(void);
void xhci_runtime_resume_events(void);
void xhci_runtime_set_ready(bool ready);
void xhci_interrupt_reset_state(void);
bool xhci_interrupt_take_pending(void);
uint32_t xhci_msix_failure_stage(void);
bool xhci_core_initialize(xhci_state_t *state, const pci_device_t *pci);
bool xhci_core_destroy(xhci_state_t *state);
bool xhci_controller_halt(xhci_state_t *state);
bool xhci_core_bind_msix(xhci_state_t *state);
void xhci_core_unbind_msix(xhci_state_t *state);

/* 同步传输路径和延迟事件工作线程共用一个事件消费者。锁所有权由控制器核心管理，避免某个 CPU 等待
 * 同一事件流时，持锁者被抢占。 */
void xhci_event_lock(xhci_state_t *state);
bool xhci_event_try_lock(xhci_state_t *state);
void xhci_event_unlock(xhci_state_t *state);

/*
 * Hub 运行时所有者包含拓扑状态机，但控制器命令、DMA 环及 Slot 上下文发布仍由 xHCI 核心负责。应显式
 * 保留该依赖，不要绕过边界调用私有静态函数，也不要建立第二套设备模型。
 */
typedef struct xhci_hub_runtime_ops {
    bool (*queue_status)(xhci_state_t *state,
                         xhci_device_context_t *hub);
    bool (*restart_status)(xhci_state_t *state,
                           xhci_device_context_t *hub);
    bool (*submit_command)(xhci_state_t *state, uint32_t type,
                           uint8_t slot, uint64_t parameter,
                           uint8_t *result_slot);
    bool (*enumerate_device)(xhci_state_t *state, uint8_t slot,
                             uint8_t port, uint8_t speed,
                             uint8_t root_port, uint8_t parent_slot,
                             uint8_t parent_port, uint32_t route_string);
    bool (*free_device_resources)(xhci_state_t *state);
    bool (*publish_working_device)(xhci_state_t *state);
    bool (*remove_device_subtree)(xhci_state_t *state, uint8_t slot);
    bool (*find_child)(uint8_t parent_slot, uint8_t parent_port,
                       uint8_t *child_slot);
    bool (*child_route)(const xhci_device_context_t *hub, uint8_t port,
                        uint32_t *route_out);
    void (*zero_device_context)(xhci_device_context_t *context);
    void (*clear_device_flags)(void);
} xhci_hub_runtime_ops_t;

/* 事件分发从控制器所有者处获取拓扑边界，不会直接访问根端口发布的内部实现。 */
typedef struct xhci_event_dispatch_ops {
    bool (*handle_root_port_event)(xhci_state_t *state, uint8_t port,
                                   uint32_t portsc);
    const xhci_hub_runtime_ops_t *hub_runtime;
} xhci_event_dispatch_ops_t;

extern const xhci_event_dispatch_ops_t g_xhci_event_dispatch_ops;
/* topology.c 管理 Slot 表存储；其他所有者通过此索引视图访问。 */
xhci_slot_device_t *xhci_topology_slot(uint32_t slot);
/* 核心所有者将单例控制器存储保持为私有。 */
xhci_state_t *xhci_controller_state(void);
/* 存储设备连接到另一独立 xHCI 功能时，第二个控制器仍可为 HID 保持运行。它有意不拥有全局 USB 拓扑。 */
xhci_state_t *xhci_hid_controller_state(void);
bool xhci_hid_controller_active(void);
void xhci_hid_controller_set_active(bool active);
/* REFACTOR_P8_XHCI_TOPOLOGY_OWNER：派生状态通过只读查询发布；快照存储仍由 topology.c 私有管理。 */
bool xhci_topology_device_enumerated(void);
uint32_t xhci_topology_device_count(void);
bool xhci_topology_hid_configured(void);
bool xhci_topology_keyboard_configured(void);
bool xhci_topology_mouse_configured(void);
bool xhci_topology_audio_configured(void);
bool xhci_topology_hub_configured(void);
uint8_t xhci_topology_hub_port_count(void);
bool xhci_topology_hub_downstream_configured(void);
bool xhci_topology_bluetooth_configured(void);
extern const xhci_hub_runtime_ops_t g_xhci_hub_runtime_ops;

void xhci_set_hardware_present(bool present);
void xhci_clear_error(void);
void xhci_set_error(uint32_t error);
uint32_t xhci_last_error(void);
void xhci_recompute_topology(xhci_state_t *state);
void xhci_clear_topology(void);
bool xhci_topology_begin_slot(uint8_t slot, uint8_t speed,
                              uint8_t parent_slot, uint8_t parent_port,
                              uint8_t root_port, uint32_t route_string,
                              uint8_t tt_slot, uint8_t tt_port,
                              bool tt_multi);
void xhci_topology_publish_slot(uint8_t slot);
void xhci_topology_detach_slot(uint8_t slot, uint8_t parent_slot,
                               uint8_t parent_port);
void xhci_topology_clear_slot(uint8_t slot);
uint8_t xhci_context_kind(const xhci_device_context_t *context);
bool xhci_topology_find_child(uint8_t parent_slot, uint8_t parent_port,
                              uint8_t *child_slot);
bool xhci_topology_child_route(const xhci_device_context_t *hub,
                               uint8_t port, uint32_t *route_out);

bool xhci_hub_runtime_handle_transfer_event(
    xhci_state_t *state,
    const xhci_trb_t *event);
void xhci_hub_runtime_rearm(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops);
bool xhci_hub_runtime_start(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops);
bool xhci_hub_runtime_probe_downstream(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops);
bool xhci_hub_runtime_reconcile(
    xhci_state_t *state,
    const xhci_hub_runtime_ops_t *ops);

bool xhci_queue_audio_transfer_device(xhci_state_t *state,
                                       xhci_device_context_t *device);
bool xhci_configure_audio_endpoint(xhci_state_t *state);
bool xhci_handle_audio_transfer_event(xhci_state_t *state,
                                      const xhci_trb_t *event);
bool xhci_enumerate_device(xhci_state_t *state, uint8_t slot,
                           uint8_t port, uint8_t speed,
                           uint8_t root_port, uint8_t parent_slot,
                           uint8_t parent_port, uint32_t route_string);
bool xhci_enumerate_hid_device(xhci_state_t *state, uint8_t slot,
                               uint8_t port, uint8_t speed,
                               uint8_t root_port);
bool xhci_queue_hub_status_device(xhci_state_t *state,
                                  xhci_device_context_t *device);
bool xhci_configure_hub_endpoint(xhci_state_t *state);
bool xhci_restart_hub_status_endpoint(xhci_state_t *state,
                                      xhci_device_context_t *hub);
bool xhci_drop_endpoint_transfer_events(xhci_state_t *state,
                                        uint8_t slot,
                                        uint8_t endpoint_id);

/* MSC 协议所有者使用的控制器原语。 */
uint64_t xhci_dma_address(const dma_mapping_t *mapping);
uint32_t xhci_controller_read32(const xhci_state_t *state, uint32_t offset);
bool xhci_controller_write32(const xhci_state_t *state, uint32_t offset,
                             uint32_t value);
bool xhci_controller_write64(const xhci_state_t *state, uint32_t offset,
                             uint64_t value);
uint64_t xhci_controller_timeout_deadline(uint64_t timeout_ns);
bool xhci_controller_timeout_reached(uint64_t deadline);
void xhci_controller_delay_ns(uint64_t delay_ns);
bool xhci_controller_map_mmio(xhci_state_t *state);
void xhci_controller_unmap_mmio(xhci_state_t *state);
bool xhci_controller_reset(xhci_state_t *state);
bool xhci_controller_handoff_legacy(xhci_state_t *state,
                                     uint32_t hcc_params1);
bool xhci_controller_setup_rings(xhci_state_t *state);
bool xhci_controller_free_rings(xhci_state_t *state);
bool xhci_controller_free_scratchpads(xhci_state_t *state);
bool xhci_alloc_dma_region(xhci_state_t *state, xhci_dma_region_t *region,
                           uint32_t page_count);
bool xhci_free_dma_region(xhci_dma_region_t *region);
bool xhci_alloc_page(xhci_state_t *state, xhci_dma_page_t *page,
                     enum dma_direction direction);
bool xhci_free_page(xhci_dma_page_t *page);
void xhci_zero_device_context(xhci_device_context_t *context);
void xhci_clear_device_flags(void);
bool xhci_free_device_resources(xhci_state_t *state);
bool xhci_free_hid_device_resources(xhci_state_t *state);
bool xhci_release_working_device(xhci_state_t *state);
bool xhci_release_slot_device(xhci_state_t *state, uint8_t slot);
bool xhci_publish_working_device(xhci_state_t *state);
bool xhci_remove_device_subtree(xhci_state_t *state, uint8_t slot);
bool xhci_handle_root_port_event(xhci_state_t *state, uint8_t port,
                                 uint32_t portsc);
bool xhci_reenumerate_self_test(xhci_state_t *state,
                                xhci_device_context_t *device);
bool xhci_probe_connected_ports(xhci_state_t *state, uint8_t *selected_slot);
bool xhci_probe_hid_controller(xhci_state_t *state);
bool xhci_free_slot_resources(xhci_state_t *state);
void xhci_unpublish_slot_device(uint8_t slot, uint8_t parent_slot,
                                uint8_t parent_port);
bool xhci_submit_command(xhci_state_t *state, uint32_t type,
                         uint8_t slot, uint64_t parameter,
                         uint8_t *result_slot);
bool xhci_submit_command_ex(xhci_state_t *state, uint32_t type,
                            uint8_t slot, uint8_t endpoint,
                            uint64_t parameter, uint8_t *result_slot);
bool xhci_submit_address_device(xhci_state_t *state, uint8_t slot,
                                uint64_t input_context,
                                bool context_only);
void xhci_init_endpoint_context(xhci_state_t *state, uint32_t *endpoint,
                                uint8_t interval, uint8_t type,
                                uint16_t max_packet,
                                const dma_mapping_t *ring);
bool xhci_next_ring_event(xhci_state_t *state, xhci_trb_t *event);
bool xhci_defer_event(xhci_state_t *state, const xhci_trb_t *event);
bool xhci_next_event(xhci_state_t *state, xhci_trb_t *event);
bool xhci_event_pending(const xhci_state_t *state);
void xhci_event_handler_complete(xhci_state_t *state);
bool xhci_process_event_ring(xhci_state_t *state, uint32_t budget,
                             const xhci_event_dispatch_ops_t *ops);
bool xhci_drain_startup_events(xhci_state_t *state,
                               const xhci_event_dispatch_ops_t *ops);
void xhci_msc_transport_release(uint8_t slot);
bool xhci_configure_msc_endpoints(xhci_state_t *state);
bool xhci_submit_control_transfer_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    const uint8_t setup[8],
    uint32_t length,
    bool direction_in);
bool xhci_hub_get_port_status_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint32_t *status_out);
bool xhci_hub_set_port_feature_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint16_t feature);
bool xhci_hub_clear_port_feature_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint16_t feature);
bool xhci_hub_ack_port_changes_device(
    xhci_state_t *state,
    xhci_device_context_t *device,
    uint8_t port,
    uint32_t status);
bool xhci_hub_ack_all_port_changes_device(
    xhci_state_t *state,
    xhci_device_context_t *device);
uint8_t xhci_hub_port_speed(uint32_t status);
void xhci_bt_transport_release(uint8_t slot);
bool xhci_configure_bt_endpoints(xhci_state_t *state);
bool xhci_handle_bt_transfer_event(xhci_state_t *state,
                                   const xhci_trb_t *event);

/* xHCI 控制器与环形队列单元共用的私有环原语。 */
void xhci_ring_init_link(void *cpu, uint64_t dma_address,
                         uint32_t trb_count);
void xhci_command_encode(void *raw_trb, uint32_t type, uint8_t slot,
                         uint8_t endpoint, uint64_t parameter,
                         uint32_t cycle);

bool xhci_event_queue_push(xhci_trb_t *entries, uint32_t *tail,
                           uint32_t *count, const xhci_trb_t *event);
bool xhci_event_queue_pop(const xhci_trb_t *entries, uint32_t *head,
                          uint32_t *count, xhci_trb_t *event);
bool xhci_event_queue_self_test(void);

bool xhci_transfer_encode_setup(void *raw_trb, const uint8_t setup[8],
                                uint32_t length, bool direction_in,
                                uint32_t cycle);
bool xhci_transfer_encode_data(void *raw_trb, uint64_t dma_address,
                               uint32_t length, bool direction_in,
                               uint32_t cycle);
bool xhci_transfer_encode_normal(void *raw_trb, uint64_t dma_address,
                                  uint32_t length, uint32_t flags,
                                  uint32_t cycle);
bool xhci_transfer_encode_isoch(void *raw_trb, uint64_t dma_address,
                                 uint32_t length, uint32_t flags,
                                 uint32_t cycle);
bool xhci_transfer_encode_status(void *raw_trb, bool direction_in,
                                 uint32_t cycle);
bool xhci_transfer_encode_self_test(void);

/* 设备/端点数据布局原语独立于控制器 MMIO、环所有权和传输调度。 */
void xhci_device_context_clear(void *context, uint64_t size);
bool xhci_device_context_self_test(void);
void xhci_endpoint_context_encode(uint32_t endpoint[5], uint8_t interval,
                                  uint8_t type, uint16_t max_packet,
                                  uint64_t ring_address);
void xhci_endpoint_context_set_max_burst(uint32_t endpoint[5],
                                         uint8_t max_burst);
void xhci_endpoint_context_set_average_trb_length(uint32_t endpoint[5],
                                                  uint16_t length);
void xhci_endpoint_context_set_max_esit_payload(uint32_t endpoint[5],
                                                uint32_t length);
void xhci_endpoint_context_set_streams(uint32_t endpoint[5],
                                       uint64_t stream_array,
                                       uint8_t max_primary_streams);
bool xhci_endpoint_context_self_test(void);

/* HID 报告解码独立于控制器 MMIO 和传输环所有权。核心提供精简快照，并负责写回状态。 */
typedef struct xhci_hid_report_context {
    const uint8_t *report;
    uint8_t protocol;
    uint8_t device_slot;
    uint8_t previous_modifier;
    uint8_t previous_keys[6];
    uint8_t previous_buttons;
} xhci_hid_report_context_t;

void xhci_hid_init(void);
void xhci_hid_consume(xhci_hid_report_context_t *context,
                      uint32_t report_length);
void xhci_hid_consume_report(xhci_device_context_t *device,
                             uint32_t report_length);
void xhci_hid_consume_report_secondary(xhci_device_context_t *device,
                                       uint32_t report_length);
bool xhci_queue_hid_report(xhci_state_t *state,
                           xhci_device_context_t *device);
bool xhci_queue_hid_report_secondary(xhci_state_t *state,
                                     xhci_device_context_t *device);
bool xhci_restart_hid_endpoint(xhci_state_t *state,
                               xhci_device_context_t *device);
bool xhci_restart_hid_endpoint_secondary(xhci_state_t *state,
                                          xhci_device_context_t *device);
bool xhci_handle_hid_transfer_event(xhci_state_t *state,
                                    const xhci_trb_t *event);
bool xhci_handle_hid_io_event(xhci_state_t *state,
                              const xhci_trb_t *event);
void xhci_hid_runtime_initialize(void);
void xhci_hid_runtime_reset_completion_counters(void);
void xhci_report_hid_completion_milestones(void);
