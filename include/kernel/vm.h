#pragma once
#pragma once
#include "base.h"
#include "mm.h"
#include "rbtree.h"
#include "list.h"
#include "spinlock.h"
#include "refcount.h"
#include "cpumask.h"

struct vnode;

enum vm_prot {
    VM_PROT_READ  = 1u << 0,
    VM_PROT_WRITE = 1u << 1,
    VM_PROT_EXEC  = 1u << 2,
    VM_PROT_USER  = 1u << 3,
};

enum vm_map_flags {
    VM_MAP_PRIVATE = 1u << 0,
    VM_MAP_SHARED  = 1u << 1,
    VM_MAP_FIXED   = 1u << 2,
    VM_MAP_STACK   = 1u << 3,
    VM_MAP_GUARD   = 1u << 4,
    /* Linux 兼容的 JIT 映射可临时使用 RWX 权限。 */
    VM_MAP_ALLOW_RWX = 1u << 5,
    /* 在 2 GiB 以下分配；边界检查在地址空间锁保护下进行。 */
    VM_MAP_LOW32 = 1u << 6,
};

enum vm_object_type {
    VM_OBJECT_ANON = 1,
    VM_OBJECT_FILE,
    VM_OBJECT_SHARED,
    VM_OBJECT_DEVICE,
    VM_OBJECT_PAGE_ARRAY,
};

enum vm_object_flags {
    VM_OBJECT_FLAG_WINDOW_SURFACE = 1u << 0,
    /* 内部私有后备对象：仅由同一 VM 空间内互不重叠的 VMA 切片持有内容。
     * 不可通过 vm_map_object() 导出。 */
    VM_OBJECT_FLAG_MAP_OWNED = 1u << 1,
};

/* 文件后备 VM 对象使用此底层映射约定。具体映射和页缓存策略由 VFS
 * 或其他文件提供方管理。 */
typedef struct vm_file_ops {
    void (*retain)(void *mapping);
    void (*release)(void *mapping);
    kstatus_t (*page_get)(void *mapping, uint64_t page_index, page_t **out);
    void (*page_mark_dirty)(void *mapping, uint64_t page_index);
    kstatus_t (*sync)(void *mapping, uint64_t first_page,
                      uint64_t page_count);
} vm_file_ops_t;

typedef struct vm_object {
    refcount_t refs;
    uint32_t type;
    uint32_t flags;
    uint64_t size;

    union {
        struct { void *anon_root; } anon;
        struct {
            void *mapping;
            const vm_file_ops_t *ops;
            uint64_t file_offset;
        } file;
        struct { void *shared_root; } shared;
        struct { paddr_t phys; uint64_t length; uint32_t cache_mode; } device;
        struct {
            uint64_t *pages;
            uint64_t page_count;
            uint32_t cache_mode;
        } page_array;
    } u;
    void *private_data;
    void (*private_release)(void *private_data);

    /* 窗口表面的统计信息保存在对象上，因此别名映射可以共享统计数据。 */
    atomic_uint_fast64_t fault_count;
    atomic_uint_fast64_t populated_pages;
    atomic_uint_fast64_t prefaulted_pages;
    atomic_uint_fast64_t fault_around_pages;
} vm_object_t;

typedef struct vm_area {
    vaddr_t start;
    vaddr_t end;
    uint64_t object_offset;
    uint32_t prot;
    uint32_t flags;

    vm_object_t *object;
    /* 私有可写文件映射的匿名 shadow；只在发生写入时按页建立。 */
    vm_object_t *private_object;

    rb_node_t rb;
    list_head_t ordered_node;

    uint64_t subtree_min_start;
    uint64_t subtree_max_end;
    uint64_t subtree_max_gap;
} vm_area_t;

#define VM_PT_LOCK_COUNT 64u

typedef struct vm_space {
    refcount_t refs;
    paddr_t root_table;
    uint16_t pcid;
    uint16_t flags;
    uint32_t reserved;

    rwlock_t map_lock;
    /* 诊断用所有权信息，仅在持有 map_lock 时写入。 */
    uint64_t map_lock_started;
    uint64_t map_lock_caller;
    uint64_t map_lock_tid;
    rb_root_t areas;
    list_head_t area_list;

    spinlock_t pt_locks[VM_PT_LOCK_COUNT];

    cpumask_t active_cpus;
    atomic_uint_fast64_t tlb_generation;
    /* 使 MADV_DONTNEED 丢弃后备页之前开始的缺页处理失效。 */
    atomic_uint_fast64_t advice_generation;

    atomic_uint_fast64_t rss_pages;
    atomic_uint_fast64_t commit_pages;
} vm_space_t;

typedef struct vm_fault_info {
    vaddr_t address;
    uint32_t access;
    uint32_t cpu_error;
} vm_fault_info_t;

kstatus_t vm_space_create(vm_space_t **out);
void vm_space_get(vm_space_t *mm);
void vm_space_put(vm_space_t *mm);

kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out);
kstatus_t vm_object_create_page_array(const uint64_t *physical_pages,
                                      uint64_t page_count,
                                      uint32_t cache_mode,
                                      void *private_data,
                                      void (*private_release)(void *private_data),
                                      vm_object_t **out);

/* 创建可被多个地址空间映射并共享后备页的共享对象。 */
kstatus_t vm_object_create_shared(size_t size, vm_object_t **out);

kstatus_t vm_map_object(vm_space_t *mm, vm_object_t *object, vaddr_t *inout_addr,
                        uint64_t offset, size_t size, uint32_t prot, uint32_t flags);
/* 原子替换重叠的 VMA；必须设置 VM_MAP_FIXED。 */
kstatus_t vm_map_object_replace(vm_space_t *mm, vm_object_t *object,
                                vaddr_t *inout_addr, uint64_t offset, size_t size,
                                uint32_t prot, uint32_t flags);
kstatus_t vm_unmap(vm_space_t *mm, vaddr_t addr, size_t size);
kstatus_t vm_protect(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t prot);
kstatus_t vm_protect_allow_rwx(vm_space_t *mm, vaddr_t addr, size_t size,
                               uint32_t prot);
bool vm_range_is_mapped(vm_space_t *mm, vaddr_t addr, size_t size);
kstatus_t vm_sync(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t flags);
kstatus_t vm_advise(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t advice);
kstatus_t vm_handle_fault(vm_space_t *mm, const vm_fault_info_t *fault);
/* 在不进入用户态的情况下，为已映射且按页对齐的范围填充页面。 */
kstatus_t vm_populate_range(vm_space_t *mm, vaddr_t start, size_t length);
/* 在 unmap 期间保持匿名/共享用户页常驻。写入固定会先处理 COW，再获取引用。
 * 最后调用 vm_unpin_user_page 释放一次。 */
kstatus_t vm_pin_user_page(vm_space_t *mm, vaddr_t address, bool writable,
                           page_t **out);
void vm_unpin_user_page(page_t *page);
/* 处理当前线程的用户页错误。架构层只提供 CR2、CPU 错误码，以及错误来自用户态
 * 还是 uaccess 代码；地址空间和权限策略由 VM 管理。 */
bool vm_handle_current_fault(vaddr_t address, uint32_t cpu_error,
                             bool from_user, bool from_uaccess);

/* 规范 VM 核心的构造、引用和 COW 克隆扩展。 */
kstatus_t vm_object_create_anon(size_t size, vm_object_t **out);
kstatus_t vm_object_create_shared(size_t size, vm_object_t **out);
kstatus_t vm_object_create_file(void *mapping, const vm_file_ops_t *ops,
                                uint64_t mapping_size, uint64_t file_offset,
                                size_t size, vm_object_t **out);
kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out);
kstatus_t vm_object_create_page_array(const uint64_t *physical_pages,
                                      uint64_t page_count,
                                      uint32_t cache_mode,
                                      void *private_data,
                                      void (*private_release)(void *private_data),
                                      vm_object_t **out);
void vm_object_get(vm_object_t *object);
void vm_object_put(vm_object_t *object);

/*
 * 将 VM_OBJECT_SHARED 中的一个页面解析到内核物理直接映射中。
 *
 * offset 可以指向请求页面内的任意位置。create 为 true 时，如果共享页不存在，
 * 就会像用户映射首次缺页一样分配一个清零页面。
 *
 * 返回地址指向所请求 4K 对象页的起始位置。该页的生命周期由 vm_object 管理；
 * 调用方必须确保对象保持有效。
 */
kstatus_t vm_object_shared_page_direct(vm_object_t *object,
                                       uint64_t offset,
                                       bool create,
                                       uint8_t **out);
void vm_object_mark_window_surface(vm_object_t *object);
uint64_t vm_object_fault_count(const vm_object_t *object);
uint64_t vm_object_populated_pages(const vm_object_t *object);
uint64_t vm_object_prefaulted_pages(const vm_object_t *object);
uint64_t vm_object_fault_around_pages(const vm_object_t *object);

/* 缺页遥测数据每个测量窗口仅输出一次，不会按每次 PF 输出。 */
void vm_fault_telemetry_reset(void);
void vm_fault_telemetry_report(void);
kstatus_t vm_space_clone_cow(vm_space_t *source, vm_space_t **out);
