#include <kernel/base.h>
#include <kernel/console.h>
#include <kernel/mm.h>

#include <arch/x86_64/paging.h>

#include <class/cl0000.h>
#include <class/cl003e.h>
#include <class/cl0040.h>
#include <nvos.h>

#include <gpu/gpu.h>
#include <gpu/mem_mgr/ce_utils.h>
#include <gpu/mem_mgr/mem_desc.h>
#include <gpu/mem_mgr/mem_mgr.h>
#include <core/locks.h>
#include <core/thread_state.h>
#include <mem_mgr/mem.h>
#include <nv-priv.h>
#include <nvoc/object.h>
#include <rmapi/rmapi.h>
#include <rmapi/rmapi_utils.h>
#include <rmapi/rs_utils.h>

#include "../../port/include/nv_port.h"

/*
 * 第 6 阶段特意通过厂商 RM 资源路径进行测试。System 不会在此处另行实现 PMA、GPU VA、GMMU
 * 或 GSP RPC。
 *
 * 常规启动探测仍只执行小规模传输。完整的第 6 阶段门控只有在编译时显式启用后才可用，绝不会由
 * 启动路径隐式启用。
 */
#define NV_RM_PHASE6_SMOKE_SIZE       4096ULL
#define NV_RM_PHASE6_SMOKE_ALIGNMENT  4096ULL
#define NV_RM_PHASE6_LARGE_SIZE       (16ULL * 1024ULL * 1024ULL)
#define NV_RM_PHASE6_CYCLE_COUNT      20U

#ifndef OS_NVIDIA_PHASE6_FULL_GATE
#define OS_NVIDIA_PHASE6_FULL_GATE 0
#endif

typedef struct nv_rm_phase6_memory {
    NvHandle handle;
    void *cpu_address;
    MEMORY_DESCRIPTOR *descriptor;
    NvBool mapped;
} nv_rm_phase6_memory_t;

typedef struct nv_rm_phase6_vendor_scope {
    THREAD_STATE_NODE thread_state;
    NvBool locks_acquired;
} nv_rm_phase6_vendor_scope_t;

static void nv_rm_phase6_log_failure(const char *stage, NV_STATUS status)
{
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_FAIL stage=%s status=%x\r\n",
        stage != NULL ? stage : "unknown", (unsigned)status);
}

static void nv_rm_phase6_fill(volatile NvU8 *address, NvU64 size, NvU8 seed)
{
    NvU64 i;

    for (i = 0U; i < size; ++i)
        address[i] = (NvU8)(seed ^ (NvU8)i ^ (NvU8)(i >> 8U));
}

static NvBool nv_rm_phase6_compare(volatile const NvU8 *address, NvU64 size,
                                   NvU8 seed, NvU64 *bad_offset)
{
    NvU64 i;

    for (i = 0U; i < size; ++i) {
        NvU8 expected = (NvU8)(seed ^ (NvU8)i ^ (NvU8)(i >> 8U));
        if (address[i] != expected) {
            if (bad_offset != NULL)
                *bad_offset = i;
            return NV_FALSE;
        }
    }

    return NV_TRUE;
}

static void nv_rm_phase6_log_compare_detail(
    const char *direction, volatile const NvU8 *address,
    MEMORY_DESCRIPTOR *system_descriptor, MEMORY_DESCRIPTOR *video_descriptor,
    NvU64 size, NvU64 bad_offset, NvU8 seed)
{
    NvU64 page_offset = bad_offset & ~(NvU64)0xfffU;
    NvU64 next_offset = page_offset + 0x1000U;
    NvU8 actual = address[bad_offset];
    NvU8 expected = (NvU8)(seed ^ (NvU8)bad_offset ^
                           (NvU8)(bad_offset >> 8U));
    NvU64 system_phys = (NvU64)memdescGetPhysAddr(
        system_descriptor, AT_GPU, bad_offset);
    NvU64 system_pte = (NvU64)memdescGetPtePhysAddr(
        system_descriptor, AT_GPU, bad_offset);
    NvU64 video_phys = (NvU64)memdescGetPhysAddr(
        video_descriptor, AT_GPU, bad_offset);
    NvU64 video_pte = (NvU64)memdescGetPtePhysAddr(
        video_descriptor, AT_GPU, bad_offset);

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_COMPARE_DETAIL direction=%s offset=%llx "
        "actual=%x expected=%x system_phys=%llx system_pte=%llx "
        "video_phys=%llx video_pte=%llx\r\n",
        direction != NULL ? direction : "unknown",
        (unsigned long long)bad_offset, (unsigned)actual, (unsigned)expected,
        (unsigned long long)system_phys, (unsigned long long)system_pte,
        (unsigned long long)video_phys, (unsigned long long)video_pte);

    if (next_offset < size) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PHASE6_COMPARE_NEXT offset=%llx "
            "system_phys=%llx system_pte=%llx video_phys=%llx video_pte=%llx\r\n",
            (unsigned long long)next_offset,
            (unsigned long long)memdescGetPhysAddr(
                system_descriptor, AT_GPU, next_offset),
            (unsigned long long)memdescGetPtePhysAddr(
                system_descriptor, AT_GPU, next_offset),
            (unsigned long long)memdescGetPhysAddr(
                video_descriptor, AT_GPU, next_offset),
            (unsigned long long)memdescGetPtePhysAddr(
                video_descriptor, AT_GPU, next_offset));
    }
}

static void nv_rm_phase6_flush_cpu_access(MEMORY_DESCRIPTOR *descriptor)
{
    if (descriptor != NULL && descriptor->pGpu != NULL)
        memdescFlushCpuCaches(descriptor->pGpu, descriptor);
}

static void nv_rm_phase6_invalidate_system_alias(
    MEMORY_DESCRIPTOR *descriptor, NvU64 size)
{
    NvU64 offset;

    if (descriptor == NULL || size == 0U ||
        memdescGetAddressSpace(descriptor) != ADDR_SYSMEM)
        return;

    for (offset = 0U; offset < size; offset += PAGE_SIZE) {
        NvU64 physical = (NvU64)memdescGetPhysAddr(
            descriptor, AT_GPU, offset);
        void *direct = phys_to_direct(paddr_make(
            physical & ~(NvU64)(PAGE_SIZE - 1ULL)));
        if (direct != NULL)
            nv_port_cache_invalidate(direct, PAGE_SIZE);
    }
    nv_port_mb();
}

static void nv_rm_phase6_log_cpu_mapping(
    const char *name, MEMORY_DESCRIPTOR *descriptor, void *cpu_address,
    NvU64 size)
{
    paddr_t translated = paddr_make(0U);
    uint64_t entry_flags = 0U;
    kstatus_t translate_status;

    if (descriptor == NULL || cpu_address == NULL || size < 0x100000ULL)
        return;

    translate_status = x86_translate_page(
        x86_current_root_table(), (vaddr_t)(uintptr_t)cpu_address,
        &translated, &entry_flags);
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_CPU_MAP name=%s size=%llx va=%llx "
        "cache=%u aperture=%u translate=%d phys=%llx pte=%llx\r\n",
        name != NULL ? name : "unknown", (unsigned long long)size,
        (unsigned long long)(uintptr_t)cpu_address,
        (unsigned)memdescGetCpuCacheAttrib(descriptor),
        (unsigned)memdescGetAddressSpace(descriptor),
        (int)translate_status, (unsigned long long)translated.value,
        (unsigned long long)entry_flags);
}

static void nv_rm_phase6_log_samples(
    const char *stage, volatile const NvU8 *address, NvU64 size)
{
    if (address == NULL || size <= 0xb8f40ULL)
        return;

    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_CPU_SAMPLE stage=%s size=%llx "
        "at0=%x at_b8f40=%x at_b9000=%x\r\n",
        stage != NULL ? stage : "unknown", (unsigned long long)size,
        (unsigned)address[0U], (unsigned)address[0xb8f40ULL],
        (unsigned)address[0xb9000ULL]);
}

static NV_STATUS nv_rm_phase6_get_descriptor(
    NvHandle h_client, NvHandle h_memory, MEMORY_DESCRIPTOR **descriptor)
{
    RsResourceRef *resource_ref = NULL;
    Memory *memory;
    NV_STATUS status;

    if (descriptor == NULL || h_memory == NV01_NULL_OBJECT)
        return NV_ERR_INVALID_ARGUMENT;

    *descriptor = NULL;
    status = serverutilGetResourceRef(h_client, h_memory, &resource_ref);
    if (status != NV_OK || resource_ref == NULL ||
        resource_ref->pResource == NULL)
        return status != NV_OK ? status : NV_ERR_INVALID_STATE;

    memory = dynamicCast(resource_ref->pResource, Memory);
    if (memory == NULL || memory->pMemDesc == NULL)
        return NV_ERR_INVALID_STATE;

    *descriptor = memory->pMemDesc;
    return NV_OK;
}

static NV_STATUS nv_rm_phase6_alloc_memory(
    RM_API *rm_api, NvHandle h_client, NvHandle h_device,
    NvHandle *h_memory, NvU32 class_id, NvBool video_memory,
    NvU64 size, NvU64 alignment)
{
    NV_MEMORY_ALLOCATION_PARAMS params = {0};

    if (rm_api == NULL || rm_api->Alloc == NULL || h_memory == NULL ||
        size == 0U || alignment == 0U)
        return NV_ERR_INVALID_ARGUMENT;

    params.owner = h_client;
    params.type = NVOS32_TYPE_DMA;
    params.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE;
    params.size = size;
    params.alignment = alignment;
    params.hVASpace = NV01_NULL_OBJECT;
    params.numaNode = -1;

    if (video_memory) {
        params.attr = DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
                      DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB) |
                      DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
        params.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
    } else {
        params.attr = DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) |
                      DRF_DEF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS) |
                      DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB) |
                      DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
    }

    return rm_api->Alloc(rm_api, h_client, h_device, h_memory, class_id,
                         &params, sizeof(params));
}

static NV_STATUS nv_rm_phase6_cleanup_memory(
    RM_API *rm_api, NvHandle h_client, NvHandle h_device,
    nv_rm_phase6_memory_t *system_memory,
    nv_rm_phase6_memory_t *video_memory)
{
    NV_STATUS first_failure = NV_OK;
    NV_STATUS status;

    if (rm_api == NULL || rm_api->Free == NULL ||
        rm_api->UnmapFromCpu == NULL || h_client == NV01_NULL_OBJECT ||
        h_device == NV01_NULL_OBJECT || system_memory == NULL ||
        video_memory == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    /* 按相反顺序释放资源：先释放 CPU 映射，再释放内存对象。客户端/设备句柄保持有效，使完整门控
     * 可以在下一轮分配/复制/释放中重复使用。 */
    if (video_memory->mapped) {
        status = rm_api->UnmapFromCpu(
            rm_api, h_client, h_device, video_memory->handle,
            video_memory->cpu_address, 0U, 0U);
        if (status != NV_OK && first_failure == NV_OK)
            first_failure = status;
        video_memory->mapped = NV_FALSE;
    }
    if (system_memory->mapped) {
        status = rm_api->UnmapFromCpu(
            rm_api, h_client, h_device, system_memory->handle,
            system_memory->cpu_address, 0U, 0U);
        if (status != NV_OK && first_failure == NV_OK)
            first_failure = status;
        system_memory->mapped = NV_FALSE;
    }

    if (video_memory->handle != NV01_NULL_OBJECT) {
        status = rm_api->Free(rm_api, h_client, video_memory->handle);
        if (status != NV_OK && first_failure == NV_OK)
            first_failure = status;
        video_memory->handle = NV01_NULL_OBJECT;
    }
    if (system_memory->handle != NV01_NULL_OBJECT) {
        status = rm_api->Free(rm_api, h_client, system_memory->handle);
        if (status != NV_OK && first_failure == NV_OK)
            first_failure = status;
        system_memory->handle = NV01_NULL_OBJECT;
    }

    return first_failure;
}

static void nv_rm_phase6_cleanup_handles(
    RM_API *rm_api, NvHandle *h_client, NvHandle *h_device,
    NvHandle *h_subdevice)
{
    if (rm_api == NULL || h_client == NULL || h_device == NULL ||
        h_subdevice == NULL)
        return;

    rmapiutilFreeClientAndDeviceHandles(rm_api, h_client, h_device,
                                        h_subdevice);
    *h_client = NV01_NULL_OBJECT;
    *h_device = NV01_NULL_OBJECT;
    *h_subdevice = NV01_NULL_OBJECT;
}

static NV_STATUS nv_rm_phase6_enter_vendor_scope(
    nv_rm_phase6_vendor_scope_t *scope)
{
    NV_STATUS status;

    if (scope == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    /* 探测过程不得窃取调用方持有的锁作用域。 */
    if (rmapiLockIsOwner() || rmGpuLocksGetOwnedMask() != 0U)
        return NV_ERR_INVALID_LOCK_STATE;

    threadStateInit(&scope->thread_state, THREAD_STATE_FLAGS_NONE);
    status = rmLocksAcquireAll(RM_LOCK_MODULES_CLIENT);
    if (status != NV_OK) {
        threadStateFree(&scope->thread_state, THREAD_STATE_FLAGS_NONE);
        return status;
    }

    scope->locks_acquired = NV_TRUE;
    return NV_OK;
}

static NV_STATUS nv_rm_phase6_leave_vendor_scope(
    nv_rm_phase6_vendor_scope_t *scope)
{
    NV_STATUS status = NV_OK;

    if (scope == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    if (scope->locks_acquired) {
        rmLocksReleaseAll();
        scope->locks_acquired = NV_FALSE;

        /* 厂商释放路径若仍持有任一锁，则安全失败。 */
        if (rmapiLockIsOwner() || rmGpuLocksGetOwnedMask() != 0U)
            status = NV_ERR_INVALID_LOCK_STATE;
    }

    threadStateFree(&scope->thread_state, THREAD_STATE_FLAGS_NONE);
    return status;
}

static NV_STATUS nv_rm_phase6_copy(CeUtils *ce_utils,
                                   MEMORY_DESCRIPTOR *source,
                                   MEMORY_DESCRIPTOR *destination,
                                   NvU64 size)
{
    CEUTILS_MEMCOPY_PARAMS params = {0};

    if (ce_utils == NULL || source == NULL || destination == NULL ||
        size == 0U)
        return NV_ERR_INVALID_ARGUMENT;

    params.pSrcMemDesc = source;
    params.pDstMemDesc = destination;
    params.length = size;

    /* 不提供完成回调：厂商 CE 实现在返回前会等待已提交负载完成，因此此处的比较有效，且不会退化为
     * CPU 复制或恒等成功回退。 */
    nv_rm_phase6_flush_cpu_access(source);
    nv_rm_phase6_flush_cpu_access(destination);

    NV_STATUS status = ceutilsMemcopy(ce_utils, &params);

    if (status == NV_OK) {
        nv_rm_phase6_flush_cpu_access(source);
        nv_rm_phase6_flush_cpu_access(destination);
        nv_rm_phase6_invalidate_system_alias(destination, size);
    }

    return status;
}

static void nv_rm_phase6_log_geometry_failure(
    const char *stage, NvU64 size, NvU64 alignment, NV_STATUS status)
{
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_FAIL stage=%s size=%llx alignment=%llx "
        "status=%x\r\n",
        stage != NULL ? stage : "geometry", (unsigned long long)size,
        (unsigned long long)alignment, (unsigned)status);
}

static NV_STATUS nv_rm_phase6_run_transfer(
    RM_API *rm_api, CeUtils *ce_utils, NvHandle h_client, NvHandle h_device,
    NvU64 size, NvU64 alignment, NvU8 seed, NvBool log_ready)
{
    nv_rm_phase6_memory_t system_memory = {0};
    nv_rm_phase6_memory_t video_memory = {0};
    NV_STATUS status = NV_OK;
    NV_STATUS cleanup_status;
    NvU64 bad_offset = 0U;

    if (rm_api == NULL || ce_utils == NULL ||
        h_client == NV01_NULL_OBJECT || h_device == NV01_NULL_OBJECT ||
        size == 0U || alignment == 0U)
        return NV_ERR_INVALID_ARGUMENT;

    status = nv_rm_phase6_alloc_memory(
        rm_api, h_client, h_device, &system_memory.handle,
        NV01_MEMORY_SYSTEM, NV_FALSE, size, alignment);
    if (status != NV_OK)
        goto cleanup;
    if (log_ready) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PHASE6_MEMORY_SYS_READY size=%llx\r\n",
            (unsigned long long)size);
    }

    status = nv_rm_phase6_alloc_memory(
        rm_api, h_client, h_device, &video_memory.handle,
        NV01_MEMORY_LOCAL_USER, NV_TRUE, size, alignment);
    if (status != NV_OK)
        goto cleanup;
    if (log_ready) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PHASE6_MEMORY_VRAM_READY size=%llx\r\n",
            (unsigned long long)size);
    }

    status = nv_rm_phase6_get_descriptor(h_client, system_memory.handle,
                                         &system_memory.descriptor);
    if (status != NV_OK)
        goto cleanup;
    status = nv_rm_phase6_get_descriptor(h_client, video_memory.handle,
                                         &video_memory.descriptor);
    if (status != NV_OK)
        goto cleanup;

    status = rm_api->MapToCpu(
        rm_api, h_client, h_device, system_memory.handle, 0U, size,
        &system_memory.cpu_address, 0U);
    if (status != NV_OK || system_memory.cpu_address == NULL) {
        if (status == NV_OK)
            status = NV_ERR_INVALID_STATE;
        goto cleanup;
    }
    system_memory.mapped = NV_TRUE;
    nv_rm_phase6_log_cpu_mapping("system", system_memory.descriptor,
                                 system_memory.cpu_address, size);

    status = rm_api->MapToCpu(
        rm_api, h_client, h_device, video_memory.handle, 0U, size,
        &video_memory.cpu_address, 0U);
    if (status != NV_OK || video_memory.cpu_address == NULL) {
        if (status == NV_OK)
            status = NV_ERR_INVALID_STATE;
        goto cleanup;
    }
    video_memory.mapped = NV_TRUE;
    nv_rm_phase6_log_cpu_mapping("video", video_memory.descriptor,
                                 video_memory.cpu_address, size);

    nv_rm_phase6_fill((volatile NvU8 *)system_memory.cpu_address, size, seed);
    nv_rm_phase6_fill((volatile NvU8 *)video_memory.cpu_address, size,
                      (NvU8)(seed ^ 0xffU));
    nv_rm_phase6_log_samples(
        "after_fill_sys", (volatile const NvU8 *)system_memory.cpu_address,
        size);
    nv_rm_phase6_log_samples(
        "after_fill_vram", (volatile const NvU8 *)video_memory.cpu_address,
        size);

    if (log_ready) {
        kernel_serial_write_serial_only(
            "OS_NVIDIA_PHASE6_CE_COPY_SYS_TO_VRAM\r\n");
    }
    status = nv_rm_phase6_copy(ce_utils, system_memory.descriptor,
                               video_memory.descriptor, size);
    if (status != NV_OK)
        goto cleanup;
    nv_rm_phase6_log_samples(
        "after_sys_to_vram", (volatile const NvU8 *)video_memory.cpu_address,
        size);
    if (!nv_rm_phase6_compare(
            (volatile const NvU8 *)video_memory.cpu_address, size, seed,
            &bad_offset)) {
        status = NV_ERR_INVALID_DATA;
        nv_rm_phase6_log_compare_detail(
            "sys_to_vram", (volatile const NvU8 *)video_memory.cpu_address,
            system_memory.descriptor, video_memory.descriptor, size,
            bad_offset, seed);
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PHASE6_COMPARE_FAIL direction=sys_to_vram "
            "offset=%llx\r\n", (unsigned long long)bad_offset);
        goto cleanup;
    }

    nv_rm_phase6_fill((volatile NvU8 *)system_memory.cpu_address, size,
                      (NvU8)(seed ^ 0x3cU));
    if (log_ready) {
        kernel_serial_write_serial_only(
            "OS_NVIDIA_PHASE6_CE_COPY_VRAM_TO_SYS\r\n");
    }
    status = nv_rm_phase6_copy(ce_utils, video_memory.descriptor,
                               system_memory.descriptor, size);
    if (status != NV_OK)
        goto cleanup;
    nv_rm_phase6_log_samples(
        "after_vram_to_sys", (volatile const NvU8 *)system_memory.cpu_address,
        size);
    if (!nv_rm_phase6_compare(
            (volatile const NvU8 *)system_memory.cpu_address, size, seed,
            &bad_offset)) {
        status = NV_ERR_INVALID_DATA;
        nv_rm_phase6_log_compare_detail(
            "vram_to_sys", (volatile const NvU8 *)system_memory.cpu_address,
            system_memory.descriptor, video_memory.descriptor, size,
            bad_offset, seed);
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PHASE6_COMPARE_FAIL direction=vram_to_sys "
            "offset=%llx\r\n", (unsigned long long)bad_offset);
        goto cleanup;
    }

cleanup:
    cleanup_status = nv_rm_phase6_cleanup_memory(
        rm_api, h_client, h_device, &system_memory, &video_memory);
    if (status == NV_OK && cleanup_status != NV_OK)
        status = cleanup_status;
    return status;
}

static NV_STATUS nv_rm_phase6_run_full_gate(
    RM_API *rm_api, CeUtils *ce_utils, NvHandle h_client, NvHandle h_device)
{
    static const NvU64 size_sweep[] = {
        1ULL,
        2ULL,
        3ULL,
        4ULL,
        7ULL,
        8ULL,
        15ULL,
        16ULL,
        31ULL,
        32ULL,
        63ULL,
        64ULL,
        127ULL,
        128ULL,
        255ULL,
        256ULL,
        4095ULL,
        4096ULL,
        4097ULL,
        65535ULL,
        65536ULL,
        1048575ULL,
        1048576ULL,
        4ULL * 1024ULL * 1024ULL,
        NV_RM_PHASE6_LARGE_SIZE,
    };
    static const NvU64 alignment_sweep[] = {
        1ULL,
        2ULL,
        4ULL,
        8ULL,
        16ULL,
        32ULL,
        64ULL,
        128ULL,
        256ULL,
        512ULL,
        1024ULL,
        2048ULL,
        4096ULL,
    };
    const NvU32 size_count =
        (NvU32)(sizeof(size_sweep) / sizeof(size_sweep[0]));
    const NvU32 alignment_count =
        (NvU32)(sizeof(alignment_sweep) / sizeof(alignment_sweep[0]));
    NvU32 index;
    NvU32 cycle;
    NV_STATUS status;

    if (rm_api == NULL || ce_utils == NULL ||
        h_client == NV01_NULL_OBJECT || h_device == NV01_NULL_OBJECT)
        return NV_ERR_INVALID_ARGUMENT;

    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE6_FULL_GATE_BEGIN\r\n");

    for (index = 0U; index < size_count; ++index) {
        status = nv_rm_phase6_run_transfer(
            rm_api, ce_utils, h_client, h_device, size_sweep[index],
            NV_RM_PHASE6_SMOKE_ALIGNMENT, (NvU8)(0x10U + index),
            NV_FALSE);
        if (status != NV_OK) {
            nv_rm_phase6_log_geometry_failure(
                "size_sweep", size_sweep[index],
                NV_RM_PHASE6_SMOKE_ALIGNMENT, status);
            return status;
        }
    }
    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE6_SIZE_SWEEP_READY\r\n");

    for (index = 0U; index < alignment_count; ++index) {
        status = nv_rm_phase6_run_transfer(
            rm_api, ce_utils, h_client, h_device,
            NV_RM_PHASE6_SMOKE_SIZE, alignment_sweep[index],
            (NvU8)(0x40U + index), NV_FALSE);
        if (status != NV_OK) {
            nv_rm_phase6_log_geometry_failure(
                "alignment_sweep", NV_RM_PHASE6_SMOKE_SIZE,
                alignment_sweep[index], status);
            return status;
        }
    }
    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE6_ALIGNMENT_SWEEP_READY\r\n");

    for (cycle = 0U; cycle < NV_RM_PHASE6_CYCLE_COUNT; ++cycle) {
        status = nv_rm_phase6_run_transfer(
            rm_api, ce_utils, h_client, h_device,
            NV_RM_PHASE6_SMOKE_SIZE,
            NV_RM_PHASE6_SMOKE_ALIGNMENT,
            (NvU8)(0x80U ^ (NvU8)cycle), NV_FALSE);
        if (status != NV_OK) {
            nv_rm_phase6_log_geometry_failure(
                "cycle", NV_RM_PHASE6_SMOKE_SIZE,
                NV_RM_PHASE6_SMOKE_ALIGNMENT, status);
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_PHASE6_CYCLE_FAIL index=%u\r\n",
                (unsigned)cycle);
            return status;
        }
        if ((cycle + 1U) == NV_RM_PHASE6_CYCLE_COUNT) {
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_PHASE6_CYCLE_PROGRESS completed=%u\r\n",
                (unsigned)(cycle + 1U));
        }
    }

    kernel_serial_write_serial_only(
        "OS_NVIDIA_PHASE6_FULL_GATE_READY\r\n");
    return NV_OK;
}

kstatus_t nv_rm_vendor_phase6_probe(nv_state_t *state)
{
    nv_priv_t *private_state;
    OBJGPU *gpu;
    MemoryManager *memory_manager;
    RM_API *rm_api;
    NvHandle h_client = NV01_NULL_OBJECT;
    NvHandle h_device = NV01_NULL_OBJECT;
    NvHandle h_subdevice = NV01_NULL_OBJECT;
    NV_STATUS status;
    NV_STATUS scope_status;
    nv_rm_phase6_vendor_scope_t scope = {0};
    const char *failure_stage = "memory_ce";

    kernel_serial_write_serial_only("OS_NVIDIA_PHASE6_BEGIN\r\n");

    if (state == NULL) {
        nv_rm_phase6_log_failure("state", NV_ERR_INVALID_ARGUMENT);
        return K_EINVAL;
    }

    private_state = (nv_priv_t *)NV_GET_NV_PRIV(state);
    gpu = private_state != NULL ? private_state->pGpu : NULL;
    if (gpu == NULL) {
        nv_rm_phase6_log_failure("rm_gpu", NV_ERR_INVALID_STATE);
        return K_EIO;
    }

    memory_manager = GPU_GET_MEMORY_MANAGER(gpu);
    if (memory_manager == NULL || memory_manager->pCeUtils == NULL) {
        nv_rm_phase6_log_failure("ce_utils", NV_ERR_INVALID_STATE);
        return K_EIO;
    }

    status = nv_rm_phase6_enter_vendor_scope(&scope);
    if (status != NV_OK) {
        nv_rm_phase6_log_failure("locks", status);
        return K_EIO;
    }

    rm_api = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_PHASE6_RM_API mode=%s api_owner=%u gpu_mask=%x\r\n",
        "gpu_internal", rmapiLockIsOwner() ? 1U : 0U,
        (unsigned)rmGpuLocksGetOwnedMask());
    if (rm_api == NULL || rm_api->Alloc == NULL || rm_api->Free == NULL ||
        rm_api->MapToCpu == NULL || rm_api->UnmapFromCpu == NULL) {
        status = NV_ERR_NOT_SUPPORTED;
        failure_stage = "rm_api";
        goto cleanup;
    }

    status = rmapiutilAllocClientAndDeviceHandles(
        rm_api, gpu, &h_client, &h_device, &h_subdevice);
    if (status != NV_OK) {
        failure_stage = "handles";
        goto cleanup;
    }

#if OS_NVIDIA_PHASE6_FULL_GATE
    status = nv_rm_phase6_run_full_gate(
        rm_api, memory_manager->pCeUtils, h_client, h_device);
#else
    status = nv_rm_phase6_run_transfer(
        rm_api, memory_manager->pCeUtils, h_client, h_device,
        NV_RM_PHASE6_SMOKE_SIZE,
        NV_RM_PHASE6_SMOKE_ALIGNMENT, 0x5aU, NV_TRUE);
#endif

cleanup:
    nv_rm_phase6_cleanup_handles(
        rm_api, &h_client, &h_device, &h_subdevice);

    scope_status = nv_rm_phase6_leave_vendor_scope(&scope);
    if (scope_status != NV_OK) {
        if (status == NV_OK) {
            status = scope_status;
            failure_stage = "locks_release";
        } else {
            kernel_serial_printf_serial_only(
                "OS_NVIDIA_PHASE6_FAIL stage=locks_release status=%x\r\n",
                (unsigned)scope_status);
        }
    }

    if (status != NV_OK) {
        nv_rm_phase6_log_failure(failure_stage, status);
        return K_EIO;
    }

    kernel_serial_write_serial_only("OS_NVIDIA_PHASE6_GATE_READY\r\n");
    return K_OK;
}
