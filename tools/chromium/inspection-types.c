/* Recompile against the current kernel headers before inspecting guest state.
 * This object provides DWARF types only; it is never linked into the kernel. */
#include <kernel/process.h>
#include <kernel/boot_profile.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vfs.h>
#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include "../../kernel/net/socket_model.h"
#include "../../kernel/drivers/net/core_internal.h"
#include "../../kernel/graphics/internal.h"
#include "../../kernel/fs/vfs/internal.h"
#include "../../kernel/sched/internal.h"
#include "../../kernel/drivers/gpu/nvidia/rm/include/nv_rm_driver.h"

_Static_assert(sizeof(void *) == 8 && sizeof(long) == 8,
               "Inspection types require the kernel's x86-64 LP64 ABI");

process_t *system_inspection_process_type;
kernel_boot_span_t system_inspection_boot_type;
nv_gpu_device_t system_inspection_gpu_type;
thread_t *system_inspection_thread_type;
wait_queue_t *system_inspection_wait_type;
arch_trap_frame_t *system_inspection_trap_type;
file_t *system_inspection_file_type;
vfs_backend_node_t *system_inspection_vfs_backend_type;
vfs_page_cache_t *system_inspection_vfs_cache_type;
window_server_window_t *system_inspection_window_type;
scheduler_cpu_t *system_inspection_cpu_type;
rtl8126_descriptor_t system_inspection_rtl_ring_type[RTL8126_RING_COUNT];
