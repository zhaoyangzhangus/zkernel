#include <uapi/syscall.h>
#include <uapi/desktop.h>
#include <uapi/window_theme.h>
/* REFACTOR_SYSCALL_GRAPHICS_OWNER：GPU、显示、输入和窗口处理程序。 */

#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/display.h>
#include <kernel/frame_trace.h>
#include <kernel/gpu.h>
#include "../drivers/gpu/nvidia/rm/include/nv_rm_driver.h"
#include <kernel/pci.h>
#include <kernel/input.h>
#include <kernel/mm.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include <kernel/window_server.h>
#include <uapi/display.h>
#include <uapi/gpu.h>
#include <uapi/input.h>
#include <uapi/mm.h>
#include <uapi/window.h>
#include <uapi/control_tree.h>

#include "internal.h"
#include "../process/internal.h"

static volatile uint32_t g_gpu_submit_progress[MAX_CPUS];

static void gpu_submit_progress(uint32_t value) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_gpu_submit_progress[cpu_index], value,
                         __ATOMIC_RELEASE);
    }
}

uint32_t syscall_gpu_submit_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_gpu_submit_progress[cpu_index],
                           __ATOMIC_ACQUIRE);
}

int64_t syscall_gpu_create_context(uint64_t arguments_pointer,
                                      uint64_t output_pointer, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4,
                                      uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_gpu_create_context_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.device != OS_INVALID_HANDLE || arguments.flags != 0U) return K_EINVAL;

    gpu_context_t *context = 0;
    status = gpu_context_create(0, process, &context);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, context, GPU_CONTEXT_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(context);
    return status;
}

/* GPU_ALLOC(args, output_handle)：分配受句柄保护的共享命令缓冲。 */
int64_t syscall_gpu_alloc(uint64_t arguments_pointer, uint64_t output_pointer,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_gpu_alloc_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.size == 0U || arguments.flags != 0U || arguments.reserved != 0U) {
        return K_EINVAL;
    }
    gpu_allocation_t *allocation = 0;
    status = gpu_allocation_create(arguments.size, &allocation);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, allocation,
                           GPU_ALLOCATION_RIGHT_ALL, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(allocation);
    return status;
}

/* GPU_SUBMIT(args)：提交进入 deferred 完成队列，由 fence 完成回调 signal。 */
int64_t syscall_gpu_submit(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    gpu_submit_progress(1U);
    if (process == 0) return K_EPERM;
    os_gpu_submit_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    gpu_submit_progress(2U);
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.context == OS_INVALID_HANDLE ||
        arguments.command_buffer == OS_INVALID_HANDLE ||
        arguments.signal_value == 0U || arguments.command_length == 0U) return K_EINVAL;
    gpu_submit_progress(3U);

    void *context_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.context,
                           GPU_CONTEXT_RIGHT_SUBMIT, &context_object);
    gpu_submit_progress(4U);
    if (status != K_OK) return status;
    void *allocation_object = 0;
    status = handle_lookup(&process->handles, (handle_t)arguments.command_buffer,
                           GPU_ALLOCATION_RIGHT_READ, &allocation_object);
    gpu_submit_progress(5U);
    if (status != K_OK) {
        object_put(context_object);
        return status;
    }
    gpu_fence_t *fence = 0;
    bool created_fence = false;
    handle_t fence_handle = (handle_t)arguments.signal_fence;
    if (fence_handle == OS_INVALID_HANDLE) {
        status = gpu_fence_create(&fence);
        gpu_submit_progress(6U);
        if (status == K_OK) {
            status = handle_create(&process->handles, fence,
                                   GPU_FENCE_RIGHT_ALL, &fence_handle);
            gpu_submit_progress(7U);
            if (status == K_OK) {
                created_fence = true;
                arguments.signal_fence = fence_handle;
            }
        }
    } else {
        void *fence_object = 0;
        status = handle_lookup(&process->handles, fence_handle,
                               GPU_FENCE_RIGHT_SIGNAL, &fence_object);
        gpu_submit_progress(6U);
        if (status == K_OK) fence = (gpu_fence_t *)fence_object;
    }
    if (status == K_OK && (fence == 0 || fence->object.type != KOBJECT_TYPE_GPU_FENCE)) {
        status = K_EINVAL;
    }
    if (status == K_OK) {
        gpu_submit_progress(8U);
        status = gpu_submit((gpu_context_t *)context_object,
                            (gpu_allocation_t *)allocation_object,
                            arguments.command_offset, arguments.command_length,
                            fence, arguments.signal_value);
        gpu_submit_progress(9U);
    }
    if (status == K_OK && created_fence) {
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && created_fence) {
        (void)handle_close(&process->handles, fence_handle);
    }
    if (fence != 0) object_put(fence);
    object_put(allocation_object);
    object_put(context_object);
    gpu_submit_progress(10U);
    return status;
}

/* GPU_WAIT_FENCE(handle, value, timeout_ns)。 */
int64_t syscall_gpu_wait_fence(uint64_t handle, uint64_t value,
                                  uint64_t timeout_ns, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     GPU_FENCE_RIGHT_WAIT, &object);
    if (status != K_OK) return status;
    gpu_fence_t *fence = (gpu_fence_t *)object;
    status = fence->object.type == KOBJECT_TYPE_GPU_FENCE ?
             gpu_fence_wait(fence, value, timeout_ns) : K_EINVAL;
    object_put(object);
    return status;
}

/* GPU_MAP(args)：把分配对象的连续物理页映射到当前进程的用户地址空间。 */
int64_t syscall_gpu_map(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_gpu_map_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.allocation == OS_INVALID_HANDLE ||
        (arguments.flags & ~OS_GPU_MAP_FIXED) != 0U ||
        (arguments.prot & ~(OS_VM_READ | OS_VM_WRITE)) != 0U ||
        arguments.prot == 0U || arguments.length == 0U ||
        (arguments.offset & (PAGE_SIZE - 1ULL)) != 0U ||
        (arguments.length & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;

    void *allocation_object = 0;
    uint32_t required_rights = 0U;
    if ((arguments.prot & OS_VM_READ) != 0U) required_rights |= GPU_ALLOCATION_RIGHT_READ;
    if ((arguments.prot & OS_VM_WRITE) != 0U) required_rights |= GPU_ALLOCATION_RIGHT_WRITE;
    status = handle_lookup(&process->handles, (handle_t)arguments.allocation,
                           required_rights, &allocation_object);
    if (status != K_OK) return status;
    gpu_allocation_t *allocation = (gpu_allocation_t *)allocation_object;
    paddr_t physical;
    if (allocation->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        gpu_allocation_get_physical(allocation, arguments.offset,
                                    arguments.length, &physical) != K_OK) {
        object_put(allocation_object);
        return K_EINVAL;
    }

    /* VMA 持有一份 allocation 引用，确保用户映射存在时底层页不会释放。 */
    object_get(allocation);
    vm_object_t *object = 0;
    status = vm_object_create_device(
        physical,
        arguments.length, X86_CACHE_WB, allocation, object_put, &object);
    if (status != K_OK) {
        object_put(allocation_object);
        object_put(allocation);
        return status;
    }

    vaddr_t address = (vaddr_t)arguments.address;
    uint32_t vm_flags = VM_MAP_SHARED;
    if ((arguments.flags & OS_GPU_MAP_FIXED) != 0U) vm_flags |= VM_MAP_FIXED;
    vm_space_t *space = process_vm_reference(process);
    status = vm_map_object(space, object, &address, 0,
                           (size_t)arguments.length,
                           VM_PROT_USER | translate_vm_protection(arguments.prot),
                           vm_flags);
    vm_object_put(object);
    if (status != K_OK) goto done;

    /* 设备页必须在返回用户态前完成映射。这样既避免用户态首次写入时
       触发不可恢复的设备页缺页，也保证返回的地址已经具备有效 PTE。 */
    uint32_t fault_access = translate_vm_protection(arguments.prot);
    for (uint64_t offset = 0; offset < arguments.length; offset += PAGE_SIZE) {
        vm_fault_info_t fault = {
            .address = address + (vaddr_t)offset,
            .access = fault_access,
            .cpu_error = 0,
        };
        status = vm_handle_fault(space, &fault);
        if (status != K_OK) break;
    }

    if (status == K_OK) {
        arguments.address = (uint64_t)address;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK) {
        (void)vm_unmap(space, address, (size_t)arguments.length);
    }
done:
    vm_space_put(space);
    object_put(allocation_object);
    return status;
}

/* 仅复制启动时缓存的设备信息，查询设置页面不会启动 RM/GSP。 */
int64_t syscall_gpu_get_info(uint64_t pointer,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_gpu_info_t request;
    kstatus_t status=copy_from_user(&request,(const void __user *)(uintptr_t)pointer,sizeof(request));
    if (status!=K_OK) return status;
    if (!versioned_header_valid(&request.hdr,sizeof(request)) || request.reserved) return K_EINVAL;
    const pci_host_t *host=pci_current_host();
    if (!host || !host->initialized) return K_EAGAIN;
    os_gpu_info_t info={.hdr={sizeof(info),OS_SYSCALL_ABI_VERSION},.index=request.index};
    const pci_device_t *selected=0;
    for (uint32_t i=0;i<host->device_count && i<PCI_MAX_DEVICES;++i) {
        const pci_device_t *device=host->devices+i;
        if (device->class_code!=3U) continue;
        if (info.count++==request.index) selected=device;
    }
    if (!selected) return K_ENOENT;
    info.vram_bytes=nv_rm_driver_memory_bytes(selected);
    info.vendor_id=selected->vendor_id;info.device_id=selected->device_id;
    info.segment=selected->segment;info.bus=selected->bus;
    info.slot=selected->slot;info.function=selected->function;
    uint32_t state=atomic_load_explicit(&selected->device.state,memory_order_acquire);
    info.state=state==DEVICE_ACTIVE ? OS_GPU_DEVICE_ACTIVE : state==DEVICE_SUSPENDED ? OS_GPU_DEVICE_SUSPENDED :
        state==DEVICE_RECOVERING ? OS_GPU_DEVICE_RECOVERING : state==DEVICE_REMOVING || state==DEVICE_REMOVED ? OS_GPU_DEVICE_REMOVED :
        state==DEVICE_FAILED ? OS_GPU_DEVICE_FAILED : OS_GPU_DEVICE_DISCOVERED;
    return copy_to_user((void __user *)(uintptr_t)pointer,&info,sizeof(info));
}

/* DISPLAY_COMMIT：提交 GPU 扫描缓冲，并通过 release fence 标记完成。 */
/* 查询当前 GOP 输出的用户态提交参数。 */
int64_t syscall_display_get_info(uint64_t arguments_pointer,
                                    uint64_t unused1, uint64_t unused2,
                                    uint64_t unused3, uint64_t unused4,
                                    uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_display_info_t info;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    kstatus_t status = copy_from_user(
        &info, (const void __user *)(uintptr_t)arguments_pointer,
        sizeof(info));
    if (status != K_OK || !versioned_header_valid(&info.hdr, sizeof(info)) ||
        info.output != 0U || info.flags != 0U) return K_EINVAL;
    if (!display_core_query(info.output, &width, &height, &stride, &format)) {
        return K_EIO;
    }
    info.width = width;
    info.height = height;
    info.stride = stride;
    info.format = format;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &info, sizeof(info));
}

/* 从内核输入队列读取一个事件，支持纳秒级超时。 */
int64_t syscall_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    input_event_t kernel_event;
    os_input_event_t user_event;
    kstatus_t status;
    if (process == 0 || !window_server_is_manager(process) || event_pointer == 0U) {
        return K_EPERM;
    }
    status = input_core_read(&kernel_event, timeout_ns);
    if (status != K_OK) return status;
    user_event.timestamp = kernel_event.timestamp;
    user_event.device_id = kernel_event.device_id;
    user_event.type = kernel_event.type;
    user_event.flags = kernel_event.flags;
    user_event.code = kernel_event.code;
    user_event.value = kernel_event.value;
    return copy_to_user((void __user *)(uintptr_t)event_pointer,
                        &user_event, sizeof(user_event));
}

static void window_copy_input_event(os_input_event_t *destination,
                                    const input_event_t *source) {
    destination->timestamp = source->timestamp;
    destination->device_id = source->device_id;
    destination->type = source->type;
    destination->flags = source->flags;
    destination->code = source->code;
    destination->value = source->value;
}

int64_t syscall_window_register_manager(uint64_t unused0, uint64_t unused1,
                                           uint64_t unused2, uint64_t unused3,
                                           uint64_t unused4, uint64_t unused5) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    return process != 0 ? window_server_register_manager(process) : K_EPERM;
}

int64_t syscall_window_create(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_create_t arguments;
    window_server_window_t *window = 0;
    vm_space_t *space = 0;
    bool mapped = false;
    uint64_t buffer_size;
    handle_t handle = OS_INVALID_HANDLE;
    kstatus_t status;
#if OS_ENABLE_FRAME_TRACE
    uint64_t create_tsc = telemetry_timestamp();
#endif
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U ||
        (arguments.flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                             OS_WINDOW_CLIENT_DECORATIONS |
                             OS_WINDOW_PREMULTIPLIED_ALPHA |
                             OS_WINDOW_DARK_CAPTION |
                             OS_WINDOW_STANDARD_CAPTION_CONTROLS |
                             OS_WINDOW_CAPTION_HEIGHT_MASK)) != 0U ||
        !os_window_caption_create_valid(arguments.flags, arguments.height) ||
        arguments.width == 0U || arguments.height == 0U) return K_EINVAL;
    status = window_server_create(process, arguments.x, arguments.y,
                                  arguments.width, arguments.height,
                                  arguments.flags, arguments.background,
                                  arguments.title, &window);
    if (status != K_OK) return status;
    buffer_size = window_server_window_buffer_size(window);
    status = handle_create(&process->handles, window, WINDOW_RIGHT_ALL, &handle);
    if (status == K_OK) {
        space = process_vm_reference(process);
        status = window_server_map_space(window, space, arguments.address,
                                   &arguments.address);
        mapped = status == K_OK;
    }
    if (status == K_OK) {
        status = window_server_set_owner_address(window, arguments.address);
    }
    if (status == K_OK && (arguments.flags & OS_WINDOW_VISIBLE) != 0U) {
        status = window_server_set(window, arguments.x, arguments.y, 1U);
    }
    if (status == K_OK) {
        arguments.window = handle;
        arguments.identifier = window_server_window_identifier(window);
        arguments.buffer_size = buffer_size;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK) {
        if (mapped) (void)vm_unmap(space, arguments.address, (size_t)buffer_size);
        (void)window_server_close_owned(process, window);
        if (handle != OS_INVALID_HANDLE) (void)handle_close(&process->handles, handle);
    }
#if OS_ENABLE_FRAME_TRACE
    if (status == K_OK) {
        uint64_t elapsed_tsc = telemetry_timestamp() - create_tsc;
        kernel_frame_trace_emit_scope("window.create", create_tsc);
        kernel_frame_trace_emit_value(
            "window_create_us",
            x86_boot_cpu_features.tsc_hz == 0U ? 0U :
                x86_tsc_to_ns(elapsed_tsc) / 1000U);
    }
#endif
    object_put(window);
    vm_space_put(space);
    return status;
}

/* WINDOW_UPDATE：客户完成自己的 surface 绘制后请求内核合成并显示。 */
int64_t syscall_window_update(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_update_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || arguments_pointer == 0U) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.identifier == 0U ||
        (arguments.flags & ~OS_WINDOW_CAPTION_HEIGHT_MASK) != 0U ||
        ((arguments.width == 0U) != (arguments.height == 0U))) return K_EINVAL;
    return window_server_update(process, arguments.identifier, arguments.x,
                                arguments.y, arguments.width, arguments.height,
                                arguments.flags);
}

int64_t syscall_window_control_tree(uint64_t pointer, uint64_t a, uint64_t b,
                                    uint64_t c, uint64_t d, uint64_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    process_t *process = current_process();
    os_window_control_tree_t request;
    if (process == 0) return K_EPERM;
    kstatus_t status = copy_from_user(&request,
        (const void __user *)(uintptr_t)pointer, sizeof(request));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&request.hdr, sizeof(request)) ||
        request.reserved != 0U || request.identifier == 0U ||
        request.count > OS_CONTROL_TREE_MAX_NODES || request.text_size > OS_CONTROL_TREE_MAX_TEXT ||
        (request.count && !request.nodes) || (request.text_size && !request.text) ||
        (!request.count && request.text_size)) return K_EINVAL;
    status = window_server_control_tree(process, &request);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)pointer, &request, sizeof(request));
}

int64_t syscall_window_title(uint64_t pointer, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    process_t *process = current_process();
    os_window_title_t request;
    if (process == 0 || pointer == 0U) return K_EPERM;
    kstatus_t status = copy_from_user(&request,
        (const void __user *)(uintptr_t)pointer, sizeof(request));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&request.hdr, sizeof(request)) ||
        request.reserved != 0U || request.identifier == 0U) return K_EINVAL;
    return window_server_title_owned(process, request.identifier, request.title);
}

int64_t syscall_window_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_enumerate_t arguments;
    window_server_snapshot_t snapshot;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_snapshot(arguments.index, &snapshot);
    if (status != K_OK) return status;
    arguments.info.identifier = snapshot.identifier;
    arguments.info.owner_pid = snapshot.owner_pid;
    arguments.info.x = snapshot.x;
    arguments.info.y = snapshot.y;
    arguments.info.width = snapshot.width;
    arguments.info.height = snapshot.height;
    arguments.info.visible = snapshot.visible;
    arguments.info.focused = snapshot.focused;
    arguments.info.z_order = snapshot.z_order;
    arguments.info.reserved = 0U;
    arguments.info.buffer_size = snapshot.buffer_size;
    for (uint32_t i = 0U; i < sizeof(arguments.info.title); ++i) {
        arguments.info.title[i] = snapshot.title[i];
    }
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_window_map(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_map_t arguments;
    window_server_window_t *window = 0;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_lookup(arguments.identifier, &window);
    if (status != K_OK) return status;
    if (arguments.length != 0U &&
        arguments.length != window_server_window_buffer_size(window)) {
        window_server_put(window);
        return K_EINVAL;
    }
    vm_space_t *space = process_vm_reference(process);
    status = window_server_map_space(window, space, arguments.address,
                               &arguments.address);
    arguments.length = window_server_window_buffer_size(window);
    window_server_put(window);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
        if (status != K_OK)
            (void)vm_unmap(space, arguments.address, (size_t)arguments.length);
    }
    vm_space_put(space);
    return status;
}

int64_t syscall_window_set(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_set_t arguments;
    window_server_window_t *window = 0;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.visible > 1U) return K_EINVAL;
    status = window_server_lookup(arguments.identifier, &window);
    if (status == K_OK) status = window_server_set(window, arguments.x,
                                                   arguments.y,
                                                   arguments.visible);
    window_server_put(window);
    return status;
}

int64_t syscall_window_focus(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_focus_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    return window_server_focus(arguments.identifier);
}

int64_t syscall_window_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    input_event_t kernel_event;
    os_input_event_t user_event;
    kstatus_t status;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process) || event_pointer == 0U) {
        return K_EPERM;
    }
    status = input_core_read(&kernel_event, timeout_ns);
    if (status != K_OK) return status;
    window_copy_input_event(&user_event, &kernel_event);
    return copy_to_user((void __user *)(uintptr_t)event_pointer,
                        &user_event, sizeof(user_event));
}

int64_t syscall_window_input_dispatch(uint64_t arguments_pointer,
                                         uint64_t unused1, uint64_t unused2,
                                         uint64_t unused3, uint64_t unused4,
                                         uint64_t unused5) {
    process_t *process = current_process();
    os_window_input_dispatch_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0 || !window_server_is_manager(process)) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    return window_server_dispatch(arguments.identifier, &arguments.event);
}

int64_t syscall_window_event_read(uint64_t arguments_pointer, uint64_t unused1,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5) {
    process_t *process = current_process();
    os_window_event_read_t arguments;
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U) return K_EINVAL;
    status = window_server_event_read(process, arguments.identifier,
                                      &arguments.event, arguments.timeout_ns);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_display_commit(uint64_t arguments_pointer,
                                  uint64_t unused1, uint64_t unused2,
                                  uint64_t unused3, uint64_t unused4,
                                  uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_display_commit_t arguments;
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *wait_fence = 0;
    gpu_fence_t *signal_fence = 0;
    handle_t signal_handle = 0;
    bool created_signal = false;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK || !versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.output != 0U || arguments.flags != 0U || arguments.reserved != 0U ||
        arguments.buffer == OS_INVALID_HANDLE || arguments.width == 0U ||
        arguments.height == 0U || arguments.stride == 0U || arguments.format != 1U ||
        arguments.signal_value == 0U ||
        (arguments.wait_fence == OS_INVALID_HANDLE && arguments.wait_value != 0U) ||
        (arguments.wait_fence != OS_INVALID_HANDLE && arguments.wait_value == 0U) ||
        (arguments.signal_fence == arguments.wait_fence &&
         arguments.signal_fence != OS_INVALID_HANDLE)) return K_EINVAL;

    status = handle_lookup(&process->handles, (handle_t)arguments.buffer,
                           GPU_ALLOCATION_RIGHT_READ, (void **)&buffer);
    if (status != K_OK) return status;
    if (buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION) {
        status = K_EINVAL;
        goto display_commit_cleanup;
    }
    if (arguments.wait_fence != OS_INVALID_HANDLE) {
        void *object = 0;
        status = handle_lookup(&process->handles, (handle_t)arguments.wait_fence,
                               GPU_FENCE_RIGHT_WAIT, &object);
        if (status != K_OK) goto display_commit_cleanup;
        wait_fence = (gpu_fence_t *)object;
        if (wait_fence->object.type != KOBJECT_TYPE_GPU_FENCE) {
            status = K_EINVAL;
            goto display_commit_cleanup;
        }
    }
    if (arguments.signal_fence == OS_INVALID_HANDLE) {
        status = gpu_fence_create(&signal_fence);
        if (status != K_OK) goto display_commit_cleanup;
        status = handle_create(&process->handles, signal_fence,
                               GPU_FENCE_RIGHT_ALL, &signal_handle);
        if (status != K_OK) goto display_commit_cleanup;
        created_signal = true;
    } else {
        void *object = 0;
        status = handle_lookup(&process->handles, (handle_t)arguments.signal_fence,
                               GPU_FENCE_RIGHT_SIGNAL, &object);
        if (status != K_OK) goto display_commit_cleanup;
        signal_fence = (gpu_fence_t *)object;
        signal_handle = (handle_t)arguments.signal_fence;
        if (signal_fence->object.type != KOBJECT_TYPE_GPU_FENCE) {
            status = K_EINVAL;
            goto display_commit_cleanup;
        }
    }
    status = display_commit_submit(arguments.output, buffer, arguments.offset,
                                   arguments.stride, arguments.width,
                                   arguments.height, arguments.format,
                                   wait_fence, arguments.wait_value,
                                   signal_fence, arguments.signal_value);
    if (status == K_OK && created_signal) {
        arguments.signal_fence = signal_handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && created_signal && signal_handle != 0) {
        (void)handle_close(&process->handles, signal_handle);
    }

display_commit_cleanup:
    if (signal_fence != 0) object_put(signal_fence);
    if (wait_fence != 0) object_put(wait_fence);
    if (buffer != 0) object_put(buffer);
    return status;
}


int64_t syscall_desktop_wallpaper(uint64_t pointer,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_desktop_wallpaper_t request;
    if (!current_process()) return K_EPERM;
    kstatus_t status=copy_from_user(&request,(void __user *)(uintptr_t)pointer,sizeof(request));
    if (status!=K_OK) return status;
    uint32_t length=0;while(length<sizeof(request.path) && request.path[length]) ++length;
    if (!versioned_header_valid(&request.hdr,sizeof(request)) || request.path[0]!='/' ||
        length==sizeof(request.path)) return K_EINVAL;
    return desktop_wallpaper_set(request.path);
}

/* 外观设置只更新语义调色板，不改写应用的图像缓冲。 */
int64_t syscall_theme_settings(uint64_t pointer,uint64_t a,uint64_t b,
    uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_theme_settings_t request;
    kstatus_t result=copy_from_user(&request,(const void __user *)(uintptr_t)pointer,sizeof(request));
    if (result!=K_OK) return result;
    if (!versioned_header_valid(&request.hdr,sizeof(request)) ||
        request.operation>OS_THEME_WRITE || request.reserved) return K_EINVAL;
    if (request.operation==OS_THEME_WRITE) {
        result=window_server_set_theme(request.theme,request.background);
        if (result!=K_OK) return result;
    }
    uint32_t palette=window_server_theme();
    request.theme=palette&255U;request.background=palette>>8U;
    return copy_to_user((void __user *)(uintptr_t)pointer,&request,sizeof(request));
}
