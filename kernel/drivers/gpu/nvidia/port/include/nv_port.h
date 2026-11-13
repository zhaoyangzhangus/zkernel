#pragma once

/*
 * NVIDIA OpenRM 集成使用的 System 移植层约定。
 *
 * 此头文件特意位于厂商 ABI 边界之下。不得在此添加 NVIDIA 符号名；这些名称会由 vendor_abi/
 * 转换。移植层只负责操作系统原语，不负责 VRAM、GPU VA、GMMU 页表或 GSP RPC 消息。
 */

#include <kernel/base.h>
#include <kernel/dma.h>
#include <kernel/firmware.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <kernel/pci.h>
#include <kernel/process.h>
#include <kernel/wait.h>

#ifdef NV_PORT_HOST_TEST
#include <threads.h>
#endif

enum nv_port_cache_type {
    NV_PORT_CACHE_WB = 0,
    NV_PORT_CACHE_WC = 1,
    NV_PORT_CACHE_UC = 2,
};

enum nv_port_mem_flags {
    NV_PORT_MEM_ZERO       = 1U << 0,
    NV_PORT_MEM_CONTIGUOUS = 1U << 1,
    NV_PORT_MEM_DMA32      = 1U << 2,
    NV_PORT_MEM_ATOMIC     = 1U << 3,
};

/*
 * CPU 内存描述符。只有在请求的缓存模式和物理布局均可直接映射到 CPU 时，cpu_va 才有效。尤其是，
 * 物理地址绝不能从 cpu_va 推导，CPU 虚拟地址也绝不能传递给 DMA API。
 */
typedef struct nv_port_mem {
    void *cpu_va;
    uint64_t size;

    uint64_t *phys_pages;
    uint32_t page_count;
    uint32_t page_shift;

    uint32_t cache_type;
    uint32_t pin_count;

    bool contiguous;

    /* 移植层私有的生命周期状态；调用方必须将这些字段视为不透明数据。 */
    page_t **page_objects;
    uint32_t allocation_order;
    uint32_t allocation_block_pages;
    bool owns_pages;
    void *backend_data;
} nv_port_mem_t;

kstatus_t nv_port_mem_alloc(size_t size, size_t alignment, uint32_t flags,
                            uint32_t cache_type, nv_port_mem_t *out);
kstatus_t nv_port_mem_alloc_ordered(size_t size, size_t page_size,
                                    size_t alignment, uint32_t flags,
                                    uint32_t cache_type, nv_port_mem_t *out);
kstatus_t nv_port_mem_wrap_pages(page_t **pages, uint32_t page_count,
                                 size_t size, uint32_t cache_type,
                                 nv_port_mem_t *out);
kstatus_t nv_port_mem_pin(nv_port_mem_t *memory);
kstatus_t nv_port_mem_unpin(nv_port_mem_t *memory);
void *nv_port_mem_cpu_ptr(const nv_port_mem_t *memory, uint64_t offset,
                          size_t length);
void *nv_port_mem_page_cpu_ptr(const nv_port_mem_t *memory, uint32_t index);
void nv_port_mem_free(nv_port_mem_t *memory);
bool nv_port_memory_self_test(void);
kstatus_t nv_port_mem_map_kernel_pages(const nv_port_mem_t *memory,
                                       uint32_t page_index,
                                       uint32_t page_count,
                                       uint32_t cache_type,
                                       bool unencrypted, void **out);
void nv_port_mem_unmap_kernel_pages(void *address, uint32_t page_count);

typedef struct nv_port_atomic32 {
    atomic_uint value;
} nv_port_atomic32_t;

typedef struct nv_port_atomic64 {
    atomic_uint_fast64_t value;
} nv_port_atomic64_t;

void nv_port_atomic32_init(nv_port_atomic32_t *value, uint32_t initial);
uint32_t nv_port_atomic32_load(const nv_port_atomic32_t *value);
void nv_port_atomic32_store(nv_port_atomic32_t *value, uint32_t next);
uint32_t nv_port_atomic32_fetch_add(nv_port_atomic32_t *value, uint32_t delta);
uint32_t nv_port_atomic32_fetch_sub(nv_port_atomic32_t *value, uint32_t delta);
bool nv_port_atomic32_compare_exchange(nv_port_atomic32_t *value,
                                       uint32_t *expected, uint32_t desired);

void nv_port_atomic64_init(nv_port_atomic64_t *value, uint64_t initial);
uint64_t nv_port_atomic64_load(const nv_port_atomic64_t *value);
void nv_port_atomic64_store(nv_port_atomic64_t *value, uint64_t next);
uint64_t nv_port_atomic64_fetch_add(nv_port_atomic64_t *value, uint64_t delta);
uint64_t nv_port_atomic64_fetch_sub(nv_port_atomic64_t *value, uint64_t delta);
bool nv_port_atomic64_compare_exchange(nv_port_atomic64_t *value,
                                       uint64_t *expected, uint64_t desired);
bool nv_port_atomic_self_test(void);

void nv_port_compiler_barrier(void);
void nv_port_mb(void);
void nv_port_rmb(void);
void nv_port_wmb(void);
void nv_port_dma_rmb(void);
void nv_port_dma_wmb(void);
void nv_port_wc_drain(void);
void nv_port_cache_flush(const void *address, size_t length);
void nv_port_cache_invalidate(const void *address, size_t length);
bool nv_port_cache_self_test(void);

typedef struct nv_port_spinlock {
    spinlock_t core;
} nv_port_spinlock_t;

#ifdef NV_PORT_HOST_TEST
typedef struct nv_port_mutex {
    atomic_uint state;
} nv_port_mutex_t;
typedef struct nv_port_completion {
    atomic_uint count;
} nv_port_completion_t;
typedef struct nv_port_semaphore {
    atomic_uint count;
} nv_port_semaphore_t;
#else
typedef struct nv_port_mutex {
    kmutex_t core;
} nv_port_mutex_t;
typedef struct nv_port_completion {
    completion_t core;
} nv_port_completion_t;
typedef struct nv_port_semaphore {
    atomic_uint count;
    wait_queue_t waitq;
} nv_port_semaphore_t;
#endif

#define NV_PORT_RW_WRITER (1U << 31)
typedef struct nv_port_rwlock {
    atomic_uint state;
    atomic_uint writers_waiting;
#ifndef NV_PORT_HOST_TEST
    wait_queue_t readers_waitq;
    wait_queue_t writers_waitq;
#endif
} nv_port_rwlock_t;

typedef struct nv_port_once {
    atomic_uint state;
} nv_port_once_t;

typedef struct nv_port_refcount {
    atomic_uint value;
} nv_port_refcount_t;

void nv_port_spin_init(nv_port_spinlock_t *lock);
void nv_port_spin_lock(nv_port_spinlock_t *lock);
bool nv_port_spin_try_lock(nv_port_spinlock_t *lock);
void nv_port_spin_unlock(nv_port_spinlock_t *lock);
uint64_t nv_port_spin_lock_irqsave(nv_port_spinlock_t *lock);
void nv_port_spin_unlock_irqrestore(nv_port_spinlock_t *lock,
                                    uint64_t flags);

void nv_port_mutex_init(nv_port_mutex_t *mutex);
kstatus_t nv_port_mutex_try_lock(nv_port_mutex_t *mutex);
kstatus_t nv_port_mutex_lock(nv_port_mutex_t *mutex, uint64_t timeout_ns);
kstatus_t nv_port_mutex_unlock(nv_port_mutex_t *mutex);

void nv_port_rwlock_init(nv_port_rwlock_t *lock);
bool nv_port_rwlock_read_try_lock(nv_port_rwlock_t *lock);
void nv_port_rwlock_read_lock(nv_port_rwlock_t *lock);
void nv_port_rwlock_read_unlock(nv_port_rwlock_t *lock);
void nv_port_rwlock_write_lock(nv_port_rwlock_t *lock);
void nv_port_rwlock_write_unlock(nv_port_rwlock_t *lock);

void nv_port_semaphore_init(nv_port_semaphore_t *semaphore, uint32_t count);
kstatus_t nv_port_semaphore_down(nv_port_semaphore_t *semaphore,
                                 uint64_t timeout_ns);
void nv_port_semaphore_up(nv_port_semaphore_t *semaphore);

void nv_port_completion_init(nv_port_completion_t *completion);
void nv_port_completion_signal(nv_port_completion_t *completion);
kstatus_t nv_port_completion_wait(nv_port_completion_t *completion,
                                  uint64_t timeout_ns);

void nv_port_once_init(nv_port_once_t *once);
kstatus_t nv_port_once_run(nv_port_once_t *once, void (*function)(void *),
                           void *argument);

void nv_port_ref_init(nv_port_refcount_t *refcount, uint32_t initial);
bool nv_port_ref_get(nv_port_refcount_t *refcount);
bool nv_port_ref_put(nv_port_refcount_t *refcount);
uint32_t nv_port_ref_value(const nv_port_refcount_t *refcount);
bool nv_port_sync_self_test(void);

uint64_t nv_port_time_now_ns(void);
kstatus_t nv_port_ns_to_us(uint64_t nanoseconds, uint64_t *microseconds);
kstatus_t nv_port_us_to_ns(uint64_t microseconds, uint64_t *nanoseconds);
uint64_t nv_port_deadline_after(uint64_t timeout_ns);
bool nv_port_deadline_expired(uint64_t deadline_ns);
void nv_port_udelay(uint64_t microseconds);
void nv_port_mdelay(uint64_t milliseconds);
kstatus_t nv_port_sleep_ns(uint64_t nanoseconds);
bool nv_port_time_self_test(void);

typedef void (*nv_port_thread_fn_t)(void *argument);

typedef struct nv_port_thread {
    atomic_bool stop_requested;
    atomic_bool started;
    atomic_bool finished;
    nv_port_thread_fn_t function;
    void *argument;
    nv_port_completion_t finished_event;
#ifdef NV_PORT_HOST_TEST
    thrd_t host_thread;
    bool host_thread_valid;
#else
    thread_t thread;
    void *stack_base;
#endif
} nv_port_thread_t;

kstatus_t nv_port_thread_create(nv_port_thread_fn_t function, void *argument,
                                nv_port_thread_t **out);
kstatus_t nv_port_thread_start(nv_port_thread_t *thread);
bool nv_port_thread_should_stop(const nv_port_thread_t *thread);
kstatus_t nv_port_thread_stop(nv_port_thread_t *thread);
kstatus_t nv_port_thread_join(nv_port_thread_t *thread, uint64_t timeout_ns);
kstatus_t nv_port_thread_destroy(nv_port_thread_t *thread);
bool nv_port_thread_self_test(void);

typedef void (*nv_port_work_fn_t)(void *argument);

enum nv_port_work_state {
    NV_PORT_WORK_IDLE = 0,
    NV_PORT_WORK_QUEUED,
    NV_PORT_WORK_RUNNING,
    NV_PORT_WORK_CANCELLED,
};

typedef struct nv_port_work {
    atomic_uint state;
    atomic_bool cancel_requested;
    nv_port_work_fn_t function;
    void *argument;
#ifdef NV_PORT_HOST_TEST
    thrd_t host_thread;
    bool host_thread_valid;
#endif
} nv_port_work_t;

void nv_port_work_init(nv_port_work_t *work, nv_port_work_fn_t function,
                       void *argument);
kstatus_t nv_port_work_queue(nv_port_work_t *work);
/* 硬 IRQ 调用方只能使用非阻塞的延迟入队路径。 */
kstatus_t nv_port_work_queue_irq(nv_port_work_t *work);
kstatus_t nv_port_work_cancel(nv_port_work_t *work);
kstatus_t nv_port_work_drain(nv_port_work_t *work, uint64_t timeout_ns);
bool nv_port_worker_self_test(void);

typedef struct nv_port_vm_mapping {
    vm_space_t *space;
    vaddr_t address;
    uint64_t length;
    paddr_t physical;
    uint32_t cache_type;
    uint32_t prot;
    bool active;
#ifdef NV_PORT_HOST_TEST
    void *host_storage;
#endif
} nv_port_vm_mapping_t;

kstatus_t nv_port_vm_map_device(vm_space_t *space, paddr_t physical,
                                uint64_t length, vaddr_t requested_address,
                                uint32_t prot, uint32_t flags,
                                uint32_t cache_type,
                                nv_port_vm_mapping_t *out);
kstatus_t nv_port_vm_unmap(nv_port_vm_mapping_t *mapping);
kstatus_t nv_port_vm_protect(nv_port_vm_mapping_t *mapping, uint32_t prot);
bool nv_port_user_range_valid(const void __user *address, size_t length);
kstatus_t nv_port_copy_from_user(void *destination,
                                 const void __user *source, size_t length);
kstatus_t nv_port_copy_to_user(void __user *destination, const void *source,
                               size_t length);
kstatus_t nv_port_vm_pin_user_pages(vm_space_t *space, vaddr_t address,
                                    size_t length, page_t **pages,
                                    uint32_t page_capacity,
                                    uint32_t *page_count);
kstatus_t nv_port_vm_unpin_user_pages(page_t **pages, uint32_t page_count);
bool nv_port_vm_self_test(void);

typedef kstatus_t (*nv_port_pci_config_read_fn)(void *context,
                                                uint16_t offset,
                                                uint32_t *value);

kstatus_t nv_port_pci_find_capability_reader(
    void *context, nv_port_pci_config_read_fn read, uint8_t capability_id,
    uint8_t *offset_out);
kstatus_t nv_port_pci_find_extended_capability_reader(
    void *context, nv_port_pci_config_read_fn read, uint16_t capability_id,
    uint16_t *offset_out);
const pci_device_t *nv_port_pci_find(const pci_host_t *host,
                                     uint16_t vendor_id, uint16_t device_id);
kstatus_t nv_port_pci_read8(const pci_device_t *device, uint16_t offset,
                            uint8_t *value);
kstatus_t nv_port_pci_read16(const pci_device_t *device, uint16_t offset,
                             uint16_t *value);
kstatus_t nv_port_pci_read32(const pci_device_t *device, uint16_t offset,
                             uint32_t *value);
kstatus_t nv_port_pci_write8(pci_device_t *device, uint16_t offset,
                             uint8_t value);
kstatus_t nv_port_pci_write16(pci_device_t *device, uint16_t offset,
                              uint16_t value);
kstatus_t nv_port_pci_write32(pci_device_t *device, uint16_t offset,
                              uint32_t value);
kstatus_t nv_port_pci_find_capability(const pci_device_t *device,
                                      uint8_t capability_id,
                                      uint8_t *offset_out);
kstatus_t nv_port_pci_find_extended_capability(const pci_device_t *device,
                                               uint16_t capability_id,
                                               uint16_t *offset_out);
kstatus_t nv_port_pci_enable(pci_device_t *device);
bool nv_port_pci_self_test(void);

typedef struct nv_port_mmio_mapping {
    void *base;
    vaddr_t virtual_base;
    paddr_t physical;
    uint64_t length;
    uint64_t mapped_length;
    uint32_t cache_type;
    bool active;
#ifdef NV_PORT_HOST_TEST
    void *host_storage;
#else
    paddr_t root_table;
    uint32_t slot;
#endif
} nv_port_mmio_mapping_t;

kstatus_t nv_port_mmio_map(paddr_t physical, uint64_t length,
                           uint32_t cache_type,
                           nv_port_mmio_mapping_t *out);
kstatus_t nv_port_mmio_map_bar(const pci_device_t *device, uint32_t bar_index,
                               uint32_t cache_type,
                               nv_port_mmio_mapping_t *out);
kstatus_t nv_port_mmio_unmap(nv_port_mmio_mapping_t *mapping);
uint32_t nv_port_mmio_read32(void *base, uint64_t offset);
void nv_port_mmio_write32(void *base, uint64_t offset, uint32_t value);
uint64_t nv_port_mmio_read64(void *base, uint64_t offset);
void nv_port_mmio_write64(void *base, uint64_t offset, uint64_t value);
kstatus_t nv_port_mmio_read_bytes(const nv_port_mmio_mapping_t *mapping,
                                  uint64_t offset, void *destination,
                                  size_t length);
kstatus_t nv_port_mmio_write_bytes(const nv_port_mmio_mapping_t *mapping,
                                   uint64_t offset, const void *source,
                                   size_t length);
bool nv_port_mmio_self_test(void);

typedef struct nv_port_vbios_blob {
    uint8_t *data;
    size_t size;
    paddr_t physical_base;
    bool owned;
} nv_port_vbios_blob_t;

kstatus_t nv_port_vbios_parse_image_size(const uint8_t *image, size_t size,
                                         size_t *image_size);
kstatus_t nv_port_vbios_read(const pci_device_t *device,
                             nv_port_vbios_blob_t *out);
void nv_port_vbios_release(nv_port_vbios_blob_t *blob);
bool nv_port_vbios_self_test(void);

typedef struct nv_port_dma_segment {
    uint64_t dma_addr;
    uint64_t phys_addr;
    uint64_t length;
} nv_port_dma_segment_t;

typedef struct nv_port_dma_mapping {
    nv_port_dma_segment_t *segments;
    uint32_t segment_count;
    uint32_t direction;
    bool coherent;
    bool active;
    struct device *device;
#ifndef NV_PORT_HOST_TEST
    dma_mapping_t core_mapping;
#endif
} nv_port_dma_mapping_t;

kstatus_t nv_port_dma_build_segments(const uint64_t *phys_pages,
                                     uint32_t page_count,
                                     nv_port_dma_segment_t *segments,
                                     uint32_t segment_capacity,
                                     uint32_t *segment_count);
kstatus_t nv_port_dma_map_pages(struct device *device, page_t **pages,
                                uint32_t page_count, uint32_t direction,
                                bool coherent, nv_port_dma_mapping_t *out);
kstatus_t nv_port_dma_map_mem(struct device *device, nv_port_mem_t *memory,
                              uint32_t direction, bool coherent,
                              nv_port_dma_mapping_t *out);
kstatus_t nv_port_dma_alloc_coherent(size_t size, size_t alignment,
                                     nv_port_mem_t *memory);
void nv_port_dma_free_coherent(nv_port_mem_t *memory);
kstatus_t nv_port_dma_unmap(nv_port_dma_mapping_t *mapping);
void nv_port_dma_sync_for_device(nv_port_dma_mapping_t *mapping);
void nv_port_dma_sync_for_cpu(nv_port_dma_mapping_t *mapping);
kstatus_t nv_port_dma_validate(struct device *device, uint64_t dma_address,
                               uint64_t length, uint32_t access,
                               const nv_port_dma_mapping_t *mapping);
bool nv_port_dma_self_test(void);

typedef void (*nv_port_irq_top_fn)(void *context);

typedef struct nv_port_irq {
    const pci_device_t *device;
    uint8_t vector;
    uint16_t msix_entry;
    uint32_t apic_id;
    bool use_msix;
    bool registered;
    atomic_bool stopping;
    atomic_uint in_flight;
    nv_port_irq_top_fn top;
    void *top_context;
    nv_port_work_t work;
} nv_port_irq_t;

kstatus_t nv_port_irq_init(nv_port_irq_t *line, const pci_device_t *device,
                           bool use_msix, uint16_t msix_entry,
                           uint32_t apic_id, nv_port_irq_top_fn top,
                           void *top_context, nv_port_work_fn_t work,
                           void *work_argument);
kstatus_t nv_port_irq_start(nv_port_irq_t *line);
kstatus_t nv_port_irq_stop(nv_port_irq_t *line);
#ifdef NV_PORT_HOST_TEST
kstatus_t nv_port_irq_synthetic_fire(nv_port_irq_t *line);
#endif
bool nv_port_irq_self_test(void);
bool nv_port_irq_vector_stress_self_test(void);

typedef struct nv_port_firmware {
    firmware_manager_t *manager;
    firmware_blob_t blob;
    bool acquired;
} nv_port_firmware_t;

kstatus_t nv_port_firmware_request(firmware_manager_t *manager,
                                   const char *logical_name,
                                   uint64_t minimum_version,
                                   nv_port_firmware_t *out);
void nv_port_firmware_release(nv_port_firmware_t *firmware);
bool nv_port_firmware_self_test(void);

#define NV_PORT_REGISTRY_MAX_ENTRIES 64U
#define NV_PORT_REGISTRY_KEY_MAX     128U
#define NV_PORT_REGISTRY_STRING_MAX 256U

enum nv_port_registry_type {
    NV_PORT_REGISTRY_U32 = 1,
    NV_PORT_REGISTRY_U64,
    NV_PORT_REGISTRY_STRING,
};

typedef struct nv_port_registry_entry {
    bool present;
    uint32_t type;
    char key[NV_PORT_REGISTRY_KEY_MAX];
    union {
        uint32_t u32;
        uint64_t u64;
        char string[NV_PORT_REGISTRY_STRING_MAX];
    } value;
} nv_port_registry_entry_t;

typedef struct nv_port_registry {
    nv_port_spinlock_t lock;
    nv_port_registry_entry_t entries[NV_PORT_REGISTRY_MAX_ENTRIES];
    bool initialized;
} nv_port_registry_t;

void nv_port_registry_init(nv_port_registry_t *registry);
kstatus_t nv_port_registry_set_u32(nv_port_registry_t *registry,
                                   const char *key, uint32_t value);
kstatus_t nv_port_registry_set_u64(nv_port_registry_t *registry,
                                   const char *key, uint64_t value);
kstatus_t nv_port_registry_set_string(nv_port_registry_t *registry,
                                      const char *key, const char *value);
kstatus_t nv_port_registry_get_u32(nv_port_registry_t *registry,
                                   const char *key, uint32_t *value);
kstatus_t nv_port_registry_get_u64(nv_port_registry_t *registry,
                                   const char *key, uint64_t *value);
kstatus_t nv_port_registry_get_string(nv_port_registry_t *registry,
                                      const char *key, char *value,
                                      size_t value_capacity);
kstatus_t nv_port_registry_remove(nv_port_registry_t *registry,
                                  const char *key);
bool nv_port_registry_self_test(void);
