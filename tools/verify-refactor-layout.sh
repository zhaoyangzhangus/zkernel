#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$root_dir"

failures=0

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        printf 'refactor layout: missing required file: %s\n' "$path" >&2
        failures=1
    fi
}

require_text() {
    local path="$1"
    local pattern="$2"
    if ! grep -Fq -- "$pattern" "$path" 2>/dev/null; then
        printf 'refactor layout: missing ownership marker in %s: %s\n' \
            "$path" "$pattern" >&2
        failures=1
    fi
}

forbidden_text() {
    local pattern="$1"
    shift
    local matches
    matches="$(grep -nRE -- "$pattern" "$@" 2>/dev/null || true)"
    if [[ -n "$matches" ]]; then
        printf 'refactor layout: forbidden legacy symbol: %s\n' "$pattern" >&2
        printf '%s\n' "$matches" >&2
        failures=1
    fi
}

# 阶段 1：BootInfo 只有一个加载器/内核 ABI 来源。旧 include 仅可作为兼容转发头保留。
require_file include/kernel/bootinfo.h
require_text include/kernel/bootinfo.h 'typedef struct os_boot_info'
require_file tools/verify-debug-locations.sh
require_text tools/verify-debug-locations.sh 'kernel_debug_stage_at'
require_text tools/verify-debug-locations.sh 'unlocated_benchmarks'
require_text tools/verify-debug-locations.sh 'unlocated_kmalloc'
require_file tools/verify-roadmap-stages.sh
require_file tools/roadmap-stage-contracts.tsv
require_text tools/verify-roadmap-stages.sh 'source contract OK'
require_text tools/verify-roadmap-stages.sh 'runtime_contract_errors'
require_file tools/verify-benchmark-schema.sh
require_file tools/roadmap-benchmark-metrics.tsv
require_text tools/verify-benchmark-schema.sh 'required metrics'
require_text tools/refactor-benchmark.sh 'verify-benchmark-schema.sh'
require_text tools/refactor-benchmark-windows.ps1 'verify-benchmark-schema.sh'
require_text makefile 'roadmap-stage-layout:'
require_text makefile 'roadmap-stages: roadmap-stage-layout'
require_text makefile 'verify-roadmap-stages.sh "$(KERNEL_ELF)" "$(STAGE_LOG)"'
require_text include/kernel/perf.h 'kernel_perf_emit_scope_at'
require_text include/kernel/perf.h 'kernel_perf_emit_value_at'
require_text include/kernel/perf.h 'kernel_perf_emit_report_at'
require_text kernel/perf.c 'OS_PERF_KMALLOC_OK SAMPLES=%llu'
require_text kernel/init/self_tests.c 'kernel_perf_emit_report(&perf_report)'
require_text kernel/perf.c 'OS_BENCH name=%s cycles=%llu loc=%s:%u'
require_text kernel/perf.c 'OS_BENCH_VALUE name=%s value=%llu loc=%s:%u'
require_file tools/run-qemu-native-matrix.ps1
require_text tools/run-qemu-native-matrix.ps1 'Native QEMU matrix passed'
require_text tools/run-qemu-native-matrix.ps1 'verify-debug-locations.sh'
require_file tools/refactor-benchmark-windows.ps1
require_text tools/refactor-benchmark-windows.ps1 \
    'Native benchmark comparison not requested'
require_text tools/refactor-benchmark.sh \
    'for metadata_key in runs cpu seconds storage profile accel'
require_text tools/refactor-benchmark-windows.ps1 \
    "@('runs', 'cpu', 'seconds', 'storage', 'profile', 'accel')"
require_text tools/refactor-benchmark-windows.ps1 \
    'fixed duration of a benchmark round'
require_text tools/refactor-benchmark-windows.ps1 \
    'verify-debug-locations.sh'
require_text tools/refactor-benchmark.sh \
    'verify-debug-locations.sh'
require_text tools/refactor-benchmark-windows.ps1 \
    "[ValidateSet('usb', 'nvme')]"
require_text run-qemu-windows.ps1 'NvmeRoot'
require_text tools/run-qemu-auto.ps1 'NvmeRoot'
require_text makefile 'debug-locations:'
require_file include/uefi.h
require_text include/uefi.h 'typedef struct EFI_BOOT_SERVICES'
require_file include/kernel/block_device.h
require_text include/kernel/block_device.h 'OS_BLOCK_DEVICE_COUNT'
require_file include/kernel/block_cache.h
require_text include/kernel/block_cache.h 'OS_BLOCK_CACHE_ENTRY_COUNT'
require_file include/kernel/fat32.h
require_text include/kernel/fat32.h 'fat32_open'
require_file kernel/fs/vfs/internal.h
require_file kernel/fs/vfs/backend.c
require_text kernel/fs/vfs/backend.c \
    'REFACTOR_FS_VFS_BACKEND_OWNER'
require_file kernel/fs/vfs/file_io.c
require_text kernel/fs/vfs/file_io.c \
    'REFACTOR_FS_VFS_FILE_IO_OWNER'
require_file kernel/fs/vfs/page_cache.c
require_text kernel/fs/vfs/page_cache.c \
    'REFACTOR_FS_VFS_PAGE_CACHE_OWNER'
require_file kernel/fs/vfs/user_api.c
require_text kernel/fs/vfs/user_api.c \
    'REFACTOR_FS_VFS_USER_API_OWNER'
require_text kernel/fs/vfs/user_api.c \
    'static bool vfs_copy_path_from_user'
require_file kernel/graphics/launcher.c
require_text kernel/graphics/launcher.c \
    'REFACTOR_P7A_SHELL_LAUNCHER_OWNER'
require_file kernel/graphics/assets.c
require_text kernel/graphics/assets.c \
    'REFACTOR_P7A_SHELL_ASSETS_OWNER'
require_text kernel/graphics/assets.c 'void desktop_shell_init'
require_text kernel/graphics/assets.c 'bool desktop_shell_start_asset_worker'
require_file kernel/graphics/png.c
require_file kernel/graphics/png_internal.h
require_file kernel/graphics/png_chunks.c
require_text kernel/graphics/png_chunks.c \
    'REFACTOR_P7A_PNG_CHUNK_OWNER'
require_text kernel/graphics/png.c 'bool desktop_png_decode'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(void|bool)[[:space:]]+desktop_shell_(init|assets_available|start_asset_worker)[[:space:]]*\(' \
    kernel/graphics/shell.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(kstatus_t|bool|const[[:space:]]+char[[:space:]]+\*)[[:space:]]+(desktop_restore_minimized_app|desktop_launch_program|window_shell_(program_path|app_title|title_matches))[[:space:]]*\(' \
    kernel/graphics/shell.c
forbidden_text '^[[:space:]]*kstatus_t[[:space:]]+vfs_(open|stat|enumerate|remove|mkdir)[[:space:]]*\(' \
    kernel/fs/vfs/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?kstatus_t[[:space:]]+vfs_(truncate_node|seek|truncate_kernel|truncate|read|read_kernel|write|write_kernel|fsync)[[:space:]]*\(' \
    kernel/fs/vfs/core.c
forbidden_text '^[[:space:]]*void[[:space:]]+vfs_close[[:space:]]*\(' \
    kernel/fs/vfs/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?kstatus_t[[:space:]]+(vfs_(memory|fat32)_backend_|vfs_register_backend_file)[[:alnum:]_]*[[:space:]]*\(' \
    kernel/fs/vfs/core.c
for path in user/libc/include/libc/libc.h user/libc/memory.c \
            user/libc/syscall.c user/libc/alloc.c user/libc/stdio.c \
            user/libc/stdlib.c user/libc/crt_start.S \
            user/libc/include/stdio.h user/libc/include/stdlib.h \
            user/libc/include/string.h user/libc/include/unistd.h \
            user/libc/include/errno.h user/libc/include/fcntl.h; do
    require_file "$path"
done
for path in user/desktop/wget/libc.c user/desktop/wget/libc_compat.c; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired user libc source remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
if [[ -d user/desktop/wget/libc ]]; then
    printf 'refactor layout: retired wget libc directory remains: %s\n' \
        user/desktop/wget/libc >&2
    failures=1
fi
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(kstatus_t|void)[[:space:]]+vfs_page_cache_(flush|destroy|copy_in)[[:space:]]*\(' \
    kernel/fs/vfs/core.c
require_file kernel/fs/nativefs/internal.h
require_file kernel/fs/nativefs/directory_codec.c
require_text kernel/fs/nativefs/directory_codec.c \
    'REFACTOR_FS_FAT32_DIRECTORY_CODEC_OWNER'
forbidden_text '^[[:space:]]*static[[:space:]]+(UINT16|UINT32|void|BOOLEAN)[[:space:]]+fat32_(read_u16|read_u32|write_u16|write_u32|make_short_name|short_name_equal|make_long_name_alias|lfn_reset|lfn_store_entry|lfn_name_equal|short_name_text|lfn_name_text|dot_entry|make_lfn_entry)[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
require_file kernel/fs/nativefs/fat_table.c
require_text kernel/fs/nativefs/fat_table.c \
    'REFACTOR_FS_FAT32_FAT_TABLE_OWNER'
forbidden_text '^[[:space:]]*static[[:space:]]+(UINT64|UINT32|BOOLEAN)[[:space:]]+fat32_(cluster_valid|root_directory_cluster|read_sector|write_sector|cluster_lba|read_fat_entry|write_fat_entry|write_file_fat_value|find_free_cluster|read_next_cluster|extent_length)[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
require_file kernel/fs/nativefs/transaction.c
require_text kernel/fs/nativefs/transaction.c \
    'REFACTOR_FS_FAT32_TRANSACTION_OWNER'
forbidden_text '^[[:space:]]*static[[:space:]]+(VOID|BOOLEAN)[[:space:]]+(fat_snapshot_|fat32_free_cluster_chain|fat32_rollback_directory_extensions)[A-Za-z0-9_]*[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
require_file kernel/fs/nativefs/directory_lifecycle.c
require_text kernel/fs/nativefs/directory_lifecycle.c \
    'REFACTOR_FS_FAT32_DIRECTORY_LIFECYCLE_OWNER'
forbidden_text '^[[:space:]]*static[[:space:]]+(VOID|UINT8|UINT32|BOOLEAN)[[:space:]]+(fat32_open_files_lock|fat32_open_files_unlock|fat32_mutation_lock|fat32_mutation_unlock|directory_short_name_exists|entry_cluster|short_name_checksum|find_directory_entry_ex|find_directory_entry|resolve_path|resolve_directory_cluster|resolve_parent_directory|find_directory_slots|write_directory_slot|delete_directory_slot|read_directory_slot|allocate_zero_cluster|initialize_directory_cluster|directory_is_empty|fat32_file_open_at)[A-Za-z0-9_]*[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
require_file kernel/fs/nativefs/file_lifecycle.c
require_text kernel/fs/nativefs/file_lifecycle.c \
    'REFACTOR_FS_FAT32_FILE_LIFECYCLE_OWNER'
forbidden_text '^[[:space:]]*static[[:space:]]+BOOLEAN[[:space:]]+(locate_file_cluster|count_file_clusters|update_file_directory_entry|extend_file_to|truncate_file_to|fat32_read|fat32_write_locked|fat32_write|fat32_truncate_path_locked)[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
forbidden_text '^[[:space:]]*BOOLEAN[[:space:]]+(liteos_)?fat32_(close|read_path|write_path|truncate_path)[[:space:]]*\(' \
    kernel/fs/nativefs/fat32.c
require_file include/kernel/memory_map.h
require_text include/kernel/memory_map.h 'boot_merge_usable_memory_map'
require_file include/boot/elf.h
require_text include/boot/elf.h 'typedef struct __attribute__((packed))'
require_file include/boot/pe.h
require_text include/boot/pe.h 'IMAGE_OPTIONAL_HEADER64'
require_file include/boot/sha256.h
require_text include/boot/sha256.h 'OS_SHA256_DIGEST_SIZE'
require_file include/usb/storage.h
require_text include/usb/storage.h 'bool usb_msc_attach'
for path in boot/uefi/rsa.c include/kernel/rsa.h \
            include/kernel/update.h include/kernel/update_boot.h \
            include/kernel/update_state.h kernel/update.c \
            kernel/update_boot.c tests/kernel/rsa_test.c; do
    if [[ -e "$path" ]]; then
        printf 'refactor layout: removed signing/update source remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
for path in security include/kernel/security.h \
            include/kernel/credential.h include/kernel/audit.h \
            include/kernel/sha256.h include/uapi/security.h \
            kernel/syscall/security.c; do
    if [[ -e "$path" ]]; then
        printf 'refactor layout: removed security subsystem path remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
forbidden_text 'security_(check_access|token_create|core_self_test)|security_token|security_descriptor|SECURITY_CAPABILITY|AUDIT_EVENT|audit_(init|emit|self_test|snapshot)|credential_(manager_init|add_user|remove_user|set_enabled|authenticate|login|session_close|core_self_test)|kernel_sha256|OS_SYS_TOKEN_QUERY|syscall_token_query' \
    kernel include kernel/drivers kernel/fs kernel/net kernel/io kernel/block tests makefile
forbidden_text 'OS_BOOTINFO_(SECURE_BOOT|KERNEL_VERIFIED|HAS_UPDATE_STATE|UPDATE_PENDING|UPDATE_SAFE_MODE|HAS_KERNEL_HASH|KERNEL_SIGNED)|KernelImageHash|Update(ActiveSlot|BootSlot|BootAttempts|Version|Generation)|kernel_update_commit_boot|update_core_self_test|rsa2048_|kernel_(a|b)_(sha256|signature)|kernel_signature|verify_required|select_kernel_for_boot|LOADER_UPDATE_SELECTION' \
    boot/uefi/main.c kernel include/kernel makefile loader.conf.example
for path in compat/include/bootinfo.h compat/include/update_state.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired ABI forwarding header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
for path in compat/include/block.h compat/include/cache.h \
            compat/include/usb/storage.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired storage compatibility header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
if [[ -f compat/include/memory_map.h ]]; then
    printf 'refactor layout: retired memory-map compatibility header remains: %s\n' \
        compat/include/memory_map.h >&2
    failures=1
fi
if [[ -f compat/include/uefi.h ]]; then
    printf 'refactor layout: retired UEFI compatibility header remains: %s\n' \
        compat/include/uefi.h >&2
    failures=1
fi
if [[ -f compat/include/rsa.h ]]; then
    printf 'refactor layout: retired RSA compatibility header remains: %s\n' \
        compat/include/rsa.h >&2
    failures=1
fi
for path in compat/include/elf.h compat/include/pe.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired boot compatibility header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
if [[ -f compat/include/sha256.h ]]; then
    printf 'refactor layout: retired boot SHA-256 compatibility header remains: %s\n' \
        compat/include/sha256.h >&2
    failures=1
fi
for path in compat/include/fat32.h compat/include/vfs.h \
            compat/include/security.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired filesystem/security compatibility header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
forbidden_text '#include[[:space:]]+["<](fat32|vfs)\.h[">]' \
    kernel kernel/fs tests
forbidden_text 'liteos_vfs_|liteos_ramfs_|LITEOS_VFS_(NODE|MOUNT|MANAGER|FILE_OPERATIONS)|LITEOS_RAMFS_|LITEOS_FILE|liteos_security_|LITEOS_SECURITY_(TOKEN|DESCRIPTOR|ACL)|(liteos_)?fat32_lookup' \
    kernel kernel/fs tests include
forbidden_text '#include[[:space:]]+"sha256\.h"|#include[[:space:]]+<sha256\.h>' \
    boot tools tests
for path in compat/graphics/window.c compat/include/window.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired window compatibility source/header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
if [[ -f compat/graphics/window_server.c ]]; then
    printf 'refactor layout: retired window-server compatibility source remains: %s\n' \
        compat/graphics/window_server.c >&2
    failures=1
fi

# 阶段 4：调度器是可运行状态发布和活动上下文切换自测的唯一所有者。
require_file include/arch/x86_64/syscall_internal.h
require_text include/arch/x86_64/syscall_internal.h \
    'typedef x86_cpu_local_t syscall_cpu_local_t'
require_file kernel/syscall/internal.h
require_file kernel/syscall/network.c
require_text kernel/syscall/network.c \
    'REFACTOR_SYSCALL_NETWORK_OWNER'
require_file kernel/syscall/graphics.c
require_text kernel/syscall/graphics.c \
    'REFACTOR_SYSCALL_GRAPHICS_OWNER'
require_file kernel/syscall/filesystem.c
require_text kernel/syscall/filesystem.c \
    'REFACTOR_SYSCALL_FILESYSTEM_OWNER'
require_file kernel/syscall/audio.c
require_text kernel/syscall/audio.c \
    'REFACTOR_SYSCALL_AUDIO_OWNER'
require_file kernel/syscall/device.c
require_text kernel/syscall/device.c \
    'REFACTOR_SYSCALL_DEVICE_OWNER'
require_file kernel/syscall/sync.c
require_text kernel/syscall/sync.c \
    'REFACTOR_SYSCALL_SYNC_OWNER'
require_file kernel/syscall/io.c
require_text kernel/syscall/io.c \
    'REFACTOR_SYSCALL_IO_OWNER'
require_file kernel/syscall/process.c
require_text kernel/syscall/process.c \
    'REFACTOR_SYSCALL_PROCESS_OWNER'
require_file kernel/syscall/vm.c
require_text kernel/syscall/vm.c \
    'REFACTOR_SYSCALL_VM_OWNER'
require_file kernel/syscall/handles.c
require_text kernel/syscall/handles.c \
    'REFACTOR_SYSCALL_HANDLE_OWNER'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_(socket|net)_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_(gpu|display|input|window)_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_file_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_audio_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_device_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_(port|completion|clock|timer|wait|futex)_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_io_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(int64_t|uint32_t)[[:space:]]+syscall_(thread|process|vm)_' \
    kernel/syscall/handlers.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?int64_t[[:space:]]+syscall_(handle_close|token_query)' \
    kernel/syscall/handlers.c
forbidden_text '#include "syscall\.h"' kernel kernel/arch

for path in \
    kernel/sched/core.c \
    kernel/sched/runqueue.c \
    kernel/sched/clock.c \
    kernel/sched/balance.c \
    kernel/sched/internal.h \
    kernel/process/thread.c \
    kernel/process/exit.c; do
    require_file "$path"
done
require_text kernel/sched/core.c 'sched_context_switch_self_test'
require_text kernel/sched/core.c 'sched_publish_blocked'
require_text kernel/sched/core.c 'sched_publish_dead'

# 阶段 5：进程生命周期与线程执行分离，其规范目录不拥有图形/显示功能。
require_file include/kernel/console_backend.h
require_file kernel/console.c
require_text kernel/console.c 'BOOLEAN kernel_console_init'
require_text kernel/console.c 'void kernel_serial_write'
forbidden_text '(^|[^[:alnum:]_])serial_write(_u(32|64))?([^[:alnum:]_]|$)' \
    kernel include
require_file include/kernel/init_self_tests.h
require_file kernel/init/self_tests.c
require_text kernel/init/self_tests.c 'REFACTOR_P3_BOOT_SELF_TEST_OWNER'
require_file include/kernel/init_runtime.h
require_file kernel/init/runtime.c
require_text kernel/init/runtime.c 'REFACTOR_P3_RUNTIME_OWNER'
require_file include/kernel/init_user_services.h
require_file kernel/init/user_services.c
require_text kernel/init/user_services.c \
    'REFACTOR_P3_USER_SERVICES_OWNER'
if [[ -f include/kernel/user_init.h || -f kernel/process/user_init.c ]]; then
    printf 'refactor layout: user-service startup remains in the retired process path\n' >&2
    failures=1
fi
require_text kernel/init/main.c 'kernel_init_post_scheduler'
require_text kernel/init/main.c 'kernel_init_runtime_start'
forbidden_text 'display_core_init|window_lifecycle_self_test|userspace_run_runtime_self_test|net_manager_poll|x86_rebase_stack_and_call' \
    kernel/init/main.c
require_file kernel/init/filesystem_internal.h
require_file kernel/init/filesystem_self_test.c
require_text kernel/init/filesystem_self_test.c \
    'REFACTOR_P3_FILESYSTEM_SELF_TEST_OWNER'
require_text kernel/init/filesystem_self_test.c \
    'BOOLEAN filesystem_vfs_file_api_self_test'
require_text kernel/init/filesystem_self_test.c \
    'BOOLEAN filesystem_fat32_self_test'
require_text kernel/init/filesystem_self_test.c \
    'BOOLEAN filesystem_file_mapping_self_test'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(bool|BOOLEAN)[[:space:]]+(vfs_file_api_self_test|fat32_self_test|canonical_file_mapping_self_test)[[:space:]]*\(' \
    kernel/init/filesystem.c
require_file kernel/init/filesystem_root_internal.h
require_file kernel/init/filesystem_root.c
require_text kernel/init/filesystem_root.c \
    'REFACTOR_P3_FILESYSTEM_ROOT_OWNER'
require_text kernel/init/filesystem_root.c \
    'BOOLEAN filesystem_mount_usb_root'
require_text kernel/init/filesystem_root.c \
    'BOOLEAN filesystem_mount_nvme_root'
forbidden_text '^[[:space:]]*static[[:space:]]+BOOLEAN[[:space:]]+mount_(usb|nvme)_root_filesystem[[:space:]]*\(' \
    kernel/init/filesystem.c
require_file include/kernel/io.h
require_text include/kernel/io.h 'typedef struct io_request'
require_file include/kernel/device.h
require_text include/kernel/device.h 'kstatus_t device_register'
require_file include/kernel/message_port.h
require_text include/kernel/message_port.h 'bool message_port_self_test'
require_file include/kernel/object.h
require_text include/kernel/object.h 'void object_get'
require_text include/kernel/object.h 'handle_close'
require_file include/kernel/mm_boot.h
require_text include/kernel/mm_boot.h 'arch_enable_kernel_paging'
require_file kernel/object/object.c
require_file kernel/object/handle.c
require_text kernel/object/handle.c 'header->ops->handle_close'
forbidden_text '#include <kernel/(completion_port|message_port|timer|socket|window_server)\.h>|KOBJECT_TYPE_(SOCKET|COMPLETION_PORT|MESSAGE_PORT|TIMER|WINDOW)|window_server_handle_closed|socket_close|completion_port_close|message_port_close|timer_cancel' \
    kernel/object/handle.c
require_text kernel/net/socket.c 'socket_handle_close'
require_text kernel/ipc/completion.c 'completion_port_handle_close'
require_text kernel/ipc/port.c 'message_port_handle_close'
require_text kernel/time/hrtimer.c 'timer_handle_close'
require_text kernel/graphics/window.c 'window_handle_close'
require_text kernel/init/scheduler.c 'canonical_object_handle_self_test'
for path in kernel/io/manager.c kernel/drivers/core/driver.c kernel/ipc/core.c \
            compat/include/driver.h compat/include/io.h \
            compat/include/ipc.h kernel/object/manager.c \
            compat/include/object.h compat/include/paging.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired duplicate I/O/driver/IPC source remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
forbidden_text 'liteos_(io_manager|irp_|driver_manager|driver_register|device_register)' \
    kernel kernel/drivers kernel/io
forbidden_text 'LITEOS_OBJECT_MANAGER|liteos_object_manager|liteos_object_create|liteos_handle_open' \
    kernel kernel/drivers kernel/io
forbidden_text '^static BOOLEAN (buddy_self_test|slab_self_test|object_self_test|io_self_test|canonical_device_dma_io_self_test|driver_self_test|pci_self_test|nvme_self_test|security_self_test)\(' \
    kernel/init/main.c
forbidden_text 'gop_debug_console|GOP_DEBUG_' kernel/init/main.c
forbidden_text 'void (kernel_serial_write|serial_write)' kernel/init/main.c
require_text kernel/init/main.c 'halt_forever_at'
for path in \
    kernel/process/process.c \
    kernel/process/thread.c \
    kernel/process/exec.c \
    kernel/process/runtime_test.c \
    kernel/process/exit.c \
    kernel/process/internal.h; do
    require_file "$path"
done
require_file kernel/process/exec_internal.h
require_text kernel/process/exec_internal.h 'REFACTOR_P5_EXEC_INTERNAL'
for path in user/runtime/elf64.h user/runtime/ld_start.S user/runtime/ld.c \
            user/runtime/graphics.h user/runtime/graphics.c \
            user/runtime/dyn_gfx_start.S user/runtime/dyn_gfx.c \
            user/runtime/libc_test.c tools/verify-dynamic-loader.sh \
            tools/verify-dynamic-loader.ps1 tools/verify-libc.sh \
            tools/verify-libc.ps1; do
    require_file "$path"
done
require_text kernel/process/exec.c 'ELF_PT_INTERP'
require_text user/runtime/ld.c 'OS_RT_DT_NEEDED'
require_text user/runtime/ld.c 'OS_RT_R_X86_64_JUMP_SLOT'
require_text user/runtime/ld.c 'OS_SYS_VM_PROTECT'
require_text user/runtime/graphics.h 'caller-owned XRGB8888 buffer'
require_text user/runtime/graphics.c 'void gfx_gradient_rect'
require_text user/runtime/dyn_gfx.c 'gfx_gradient_rect'
require_text user/libc/include/libc/libc.h 'REFACTOR_P5_LIBC_OWNER'
for path in user/libc/complex.c user/libc/uchar.c \
            user/libc/include/complex.h user/libc/include/tgmath.h \
            user/libc/include/uchar.h user/libc/threads.c \
            user/libc/include/threads.h; do
    require_file "$path"
done
require_text tools/libc_header_sanity.c 'mbrtoc16'
require_text tools/verify-libc.sh 'c16rtomb'
require_text tools/verify-libc.ps1 'c32rtomb'
require_text user/libc/syscall.c 'OS_SYS_DEBUG_WRITE'
require_text user/libc/crt_start.S 'libc_crt_entry'
require_text kernel/syscall/debug.c 'REFACTOR_SYSCALL_DEBUG_OWNER'
require_text include/uapi/syscall.h 'OS_SYS_DEBUG_WRITE'
require_file include/uapi/image.h
require_text include/uapi/image.h 'OS_IMAGE_PIXEL_RGBA8888'
require_text include/uapi/syscall.h 'OS_SYS_IMAGE_DECODE'
require_file kernel/graphics/png.h
require_file kernel/syscall/image.c
require_text kernel/syscall/image.c 'REFACTOR_SYSCALL_IMAGE_OWNER'
require_text makefile '$(BUILD)/esp/lib/ld-system.so.1'
require_text makefile '$(BUILD)/esp/lib/libsystemc.so.1'
require_text makefile '$(BUILD)/esp/lib/libgraphics.so.1'
require_text makefile '$(BUILD)/esp/usr/apps/dyn-gfx/dyn-gfx'
require_text makefile '$(BUILD)/esp/usr/apps/libc-test/libc-test'
require_text makefile '$(BUILD)/esp/usr/apps/imageview/imageview'
require_file user/desktop/imageview/main.c
require_text user/desktop/imageview/main.c 'OS_SYS_IMAGE_DECODE'
require_text user/desktop/gshell/main.c '"imageview"'
require_text makefile 'libc-sanity:'
require_text kernel/process/runtime_test.c \
    'REFACTOR_P5_USER_RUNTIME_TEST_OWNER'
require_text kernel/process/runtime_test.c \
    'bool user_elf_runtime_self_test'
require_text kernel/process/thread.c \
    'static volatile uint32_t g_process_thread_create_stage'
require_text kernel/process/process.c \
    'process_register_teardown_callback'
forbidden_text 'g_process_thread_create_stage' \
    kernel/process/process.c kernel/process/internal.h
forbidden_text 'process_set_teardown_hook|g_process_teardown_hook|process_teardown_resources' \
    include/kernel/process.h kernel/process kernel/init
require_text kernel/graphics/server.c \
    'process_register_teardown_callback(window_server_close_process)'
require_text kernel/io/request.c \
    'process_register_teardown_callback(io_cancel_process)'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool[[:space:]]+user_elf_runtime_self_test[[:space:]]*\(' \
    kernel/process/exec.c
forbidden_text 'scheduler_compat|process_scheduler|liteos_arch_context_switch|LITEOS_CPU_CONTEXT|LITEOS_RUN_QUEUE|LITEOS_SCHEDULER_ACTIVE_OK' \
    kernel/sched kernel/process include kernel/arch/x86_64/cpu/context_switch.S makefile
forbidden_text '#include <kernel/(window_server|display|gpu|input)\.h>|#include "(window_server|display|gpu|input)\.h"' \
    kernel/process

# 阶段 6：VM 策略保留在 kernel/mm；页表、CR3 和 TLB 汇编原语则位于架构边界以下。
for path in \
    kernel/mm/phys/internal.h \
    kernel/mm/phys/page_db.c \
    kernel/mm/phys/page_alloc.c \
    kernel/mm/phys/percpu_page.c \
    kernel/mm/vm_space.c \
    kernel/mm/object.c \
    kernel/mm/vma.c \
    kernel/mm/map.c \
    kernel/mm/fault.c \
    kernel/mm/shared.c \
    kernel/mm/protection.c \
    kernel/mm/tlb.c \
    kernel/arch/x86_64/mm/mmu.c \
    kernel/arch/x86_64/mm/page_table.c \
    kernel/arch/x86_64/mm/tlb.c; do
    require_file "$path"
done
if [[ -f kernel/mm/page_db.c ]]; then
    printf 'refactor layout: retired monolithic physical MM source remains: %s\n' \
        kernel/mm/page_db.c >&2
    failures=1
fi
require_text kernel/mm/phys/internal.h 'REFACTOR_P2_PHYS_INTERNAL'
require_text kernel/mm/phys/page_db.c 'REFACTOR_P2_PAGE_DB_OWNER'
require_text kernel/mm/phys/page_db.c 'bool mm_boot_init'
require_text kernel/mm/phys/page_alloc.c 'REFACTOR_P2_PAGE_ALLOC_OWNER'
require_text kernel/mm/phys/page_alloc.c 'page_t *page_alloc_slow'
require_text kernel/mm/phys/percpu_page.c 'REFACTOR_P2_PERCPU_PAGE_OWNER'
require_text kernel/mm/phys/percpu_page.c 'page_t *page_alloc('
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(page_t[[:space:]]*\*|void)[[:space:]]+(page_alloc|page_free)[[:space:]]*\(' \
    kernel/mm/phys/page_db.c kernel/mm/phys/page_alloc.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?page_t[[:space:]]*\*?[[:space:]]*page_alloc_slow[[:space:]]*\(' \
    kernel/mm/phys/page_db.c kernel/mm/phys/percpu_page.c
require_file include/kernel/mm.h
require_text include/kernel/mm.h 'typedef struct page'
require_text kernel/mm/object.c 'REFACTOR_P6_VM_OBJECT_OWNER'
require_text kernel/mm/object.c 'vm_object_put'
require_text kernel/mm/shared.c 'REFACTOR_P6_SHARED_OBJECT_OWNER'
for path in compat/mm/buddy.c compat/mm/page.c compat/mm/slab.c \
            compat/include/buddy.h compat/include/page.h \
            compat/include/slab.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: retired MM compatibility source/header remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
forbidden_text '^#include "(buddy|page|slab)\.h"' \
    kernel kernel/mm kernel/arch kernel/fs tests user makefile
forbidden_text 'liteos_(buddy|page|slab)_' \
    kernel kernel/mm kernel/arch kernel/fs tests user makefile
require_text kernel/arch/x86_64/mm/mmu.c 'x86_activate_root_table_pcid'
require_text kernel/arch/x86_64/mm/page_table.c 'x86_map_page'
require_text kernel/arch/x86_64/mm/tlb.c 'x86_tlb_shootdown_page'
require_text kernel/mm/fault.c 'vm_handle_current_fault'
require_text include/kernel/vm.h 'typedef struct vm_file_ops'
require_text include/kernel/vfs.h 'vfs_vm_file_ops'
require_text kernel/fs/vfs/page_cache.c 'g_vfs_vm_file_ops'
require_text kernel/mm/fault.c 'file_ops->page_get'
require_text kernel/mm/object.c 'ops->release'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(kstatus_t|void|page_t[[:space:]]*\*)[[:space:]]+(vm_object_(create_anon|create_file|create_device|get|put)|vm_object_clone_anon|anon_page_(get|lookup|cow)|private_file_shadow_page)[[:space:]]*\(' \
    kernel/mm/vm_space.c kernel/mm/shared.c
forbidden_text '#include <kernel/vfs\.h>|vfs_(file_page_get|file_page_mark_dirty)|vnode_t|KOBJECT_TYPE_VNODE' \
    kernel/mm
forbidden_text '^#if 0|x86_pte_cache_bits|x86_activate_root_table_pcid' \
    kernel/mm/vm_space.c kernel/mm/map.c kernel/mm/fault.c kernel/mm/protection.c kernel/mm/shared.c kernel/mm/tlb.c
forbidden_text 'static bool canonical_page_fault|vm_handle_fault\(' \
    kernel/arch/x86_64/irq/exception.c

# 进程模块负责发布退出状态；调度器只发布可运行状态的终止转换。
require_text kernel/process/exit.c 'bool process_publish_thread_exit'
forbidden_text 'thread->exit_code' kernel/sched/core.c
forbidden_text 'sched_dead_prepare_fn|sched_publish_dead\([^)]*,' \
    include/kernel/sched.h kernel/sched kernel/process
forbidden_text 'THREAD_FLAG_INITIAL_PLACEMENT|THREAD_FLAG_EXECUTION_REAP_QUEUED|global_node' \
    include/kernel/sched.h include/kernel/process.h kernel/sched kernel/process
require_text include/kernel/sched.h 'void sched_initialize_new_thread'
require_text kernel/sched/runqueue.c 'void sched_initialize_new_thread'
require_text kernel/sched/runqueue.c 'void sched_initialize_test_thread'
forbidden_text 'thread->[[:space:]]*(state|sched_class|rt_priority|base_sched_class|base_rt_priority|current_cpu|affinity|sched\.[[:alnum:]_]+)[[:space:]]*=[^=]' \
    kernel/process/thread.c kernel/sync/mutex.c

# 阶段 7A：场景/输入/损坏区域几何量均有明确单位。在经过测量的 7B 拆分完成前，
# 合成器实现仍是热循环的单一所有者。
require_file include/kernel/rect.h
require_text include/kernel/rect.h 'typedef struct Rect'
forbidden_text 'window_damage_rect_t|WindowRect|RegionRect|(^|[.>-])(left|top|right|bottom)\b' \
    include/kernel/rect.h kernel/graphics compat/graphics/window_server.c
for path in \
    kernel/graphics/window_geometry.c \
    kernel/graphics/server.c \
    kernel/graphics/scene.c \
    kernel/graphics/hit_test.c \
    kernel/graphics/zorder.c \
    kernel/graphics/window.c \
    kernel/graphics/display.c \
    kernel/graphics/buffer.c \
    kernel/graphics/input.c \
    kernel/graphics/input_router.c \
    kernel/graphics/input_motion.c \
    kernel/graphics/input_drag.c \
    kernel/graphics/input_events.c \
    kernel/graphics/input_pump.c \
    kernel/graphics/present.c \
    kernel/graphics/present_cursor.c \
    kernel/graphics/damage.c \
    kernel/graphics/compose_cpu.c \
    kernel/graphics/compositor.c \
    kernel/graphics/publication_policy.c \
    kernel/graphics/render.c \
    kernel/graphics/decorations.c \
    kernel/graphics/cursor_occlusion.c \
    kernel/graphics/raster.c \
    kernel/graphics/tile_metadata.c \
    kernel/graphics/render_plan.c \
    kernel/graphics/damage_plan.c \
    kernel/graphics/shell.c \
    kernel/graphics/assets.c \
    kernel/graphics/png.c \
    kernel/graphics/png_chunks.c \
    kernel/graphics/ascii_font.c \
    kernel/graphics/ascii_font_parser.c; do
    require_file "$path"
done
require_text kernel/graphics/server.c 'void window_lock'
require_text kernel/graphics/server.c 'void window_unlock'
require_text kernel/graphics/server.c 'REFACTOR_P7A_SERVER_OWNER'
require_text kernel/graphics/server.c 'window_server_state_t g_window_server'
require_text kernel/graphics/server.c 'bool window_server_init'
require_text kernel/graphics/server.c 'bool window_server_start_worker'
require_text kernel/graphics/server.c 'bool window_server_kernel_ready'
require_text kernel/graphics/server.c 'void window_server_notify_worker'
require_text kernel/graphics/server.c 'input_core_bind_wakeup'
require_text include/kernel/input.h 'input_core_bind_wakeup'
forbidden_text '#include <kernel/window_server\.h>|window_server_notify_worker' \
    kernel/drivers/input/core.c
forbidden_text '^void window_(lock|unlock)\(' \
    compat/graphics/window_server.c
require_text kernel/graphics/scene.c 'window_scene_find_locked'
require_text kernel/graphics/hit_test.c 'window_scene_hit_test_locked'
require_text kernel/graphics/zorder.c 'window_scene_focus_locked'
require_text kernel/graphics/zorder.c \
    'void window_scene_set_focus_identifier_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/zorder.c) ;;
        *)
            forbidden_text \
                'g_window_server\.focused_identifier[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/window_geometry.c 'compositor_corner_inset'
require_text kernel/graphics/window_geometry.c 'bool window_title_button_rect'
require_text kernel/graphics/window.c 'window_server_create'
require_text kernel/graphics/window.c 'window_server_update'
require_text kernel/graphics/window.c 'bool window_lifecycle_self_test'
require_text kernel/graphics/window.c 'void remove_window_locked'
require_text kernel/graphics/window.c 'void window_registry_reset_locked'
require_text kernel/graphics/window.c 'bool window_registry_append_locked'
require_text kernel/graphics/window.c 'bool window_registry_remove_locked'
require_text kernel/graphics/window.c \
    'void window_registry_move_to_front_locked'
require_text kernel/graphics/internal.h \
    'REFACTOR_P7A_WINDOW_INTERNAL_OWNER'
require_text include/kernel/window_server.h \
    'typedef struct window_server_window window_server_window_t'
require_text include/kernel/window_server.h \
    'window_server_window_identifier'
require_text include/kernel/window_server.h \
    'window_server_window_buffer_size'
forbidden_text 'object_header_t|shared_section_t|event_waitq|compositor_cache|compositor_presented_' \
    include/kernel/window_server.h
forbidden_text 'window->[[:space:]]*(identifier|buffer_size)' \
    kernel/syscall/graphics.c
forbidden_text '#include <kernel/window_server\.h>' \
    include/kernel/window_geometry.h
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/window.c) ;;
        *)
            forbidden_text \
                'g_window_server\.windows(\[[^]]+\])?[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            forbidden_text \
                'g_window_server\.(count|next_identifier)[[:space:]]*(\+\+|--|\+=|-=|=)[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/display.c 'window_display_prepare'
require_text kernel/graphics/buffer.c 'window_buffer_prepare'
require_text kernel/graphics/display.c 'void window_display_reset_locked'
require_text kernel/graphics/buffer.c 'void window_buffer_reset_locked'
require_text kernel/graphics/buffer.c \
    'void window_buffer_set_target_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/display.c) ;;
        *)
            forbidden_text \
                'g_window_server\.(display_width|display_height|display_stride|display_format|framebuffer)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
    case "$path" in
        kernel/graphics/buffer.c) ;;
        *)
            forbidden_text \
                'g_window_server\.composite_framebuffer[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/input.c 'window_input_resize_edges_locked'
require_text kernel/graphics/input_router.c 'void window_clear_title_capture_locked'
require_text kernel/graphics/input_router.c 'bool window_title_capture_moved_locked'
require_text kernel/graphics/input_router.c 'uint32_t window_title_button_at_locked'
require_text kernel/graphics/input_router.c 'static bool window_minimize_locked'
require_text kernel/graphics/input_router.c 'bool window_toggle_maximize_locked'
require_text kernel/graphics/input_router.c 'void route_input_locked'
require_text kernel/graphics/input_drag.c 'REFACTOR_P7A_INPUT_DRAG_OWNER'
require_text kernel/graphics/input_drag.c 'bool window_resize_locked'
require_text kernel/graphics/input_drag.c 'bool window_drag_reuse_safe_locked'
require_text kernel/graphics/input_drag.c 'bool window_route_drag_motion_batch_locked'
require_text kernel/graphics/input_drag.c \
    'void window_input_reset_drag_locked'
require_text kernel/graphics/input_drag.c \
    'void window_input_clear_drag_locked'
require_text kernel/graphics/input_drag.c \
    'void window_input_begin_drag_locked'
require_text kernel/graphics/input_drag.c \
    'void window_input_record_drag_frame_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/input_drag.c) ;;
        *)
            forbidden_text \
                'g_window_server\.(dragging_identifier|drag_offset_x|drag_offset_y|resize_edges|drag_blit_valid|drag_old_x|drag_old_y|drag_new_x|drag_new_y|drag_width|drag_height)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/input_motion.c 'REFACTOR_P7A_INPUT_MOTION_OWNER'
require_text kernel/graphics/input_motion.c 'void window_route_motion_axis_locked'
require_text kernel/graphics/input_motion.c 'void window_flush_motion_batch_locked'
require_text kernel/graphics/input_motion.c 'void window_route_pointer_transaction_locked'
require_text kernel/graphics/input_motion.c \
    'void window_input_set_pointer_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/input_motion.c) ;;
        *)
            forbidden_text \
                'g_window_server\.pointer_(x|y)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(void|bool)[[:space:]]+window_(route_motion_axis_locked|flush_motion_batch_locked|route_pointer_transaction_locked)[[:space:]]*\(' \
    kernel/graphics/input_router.c
require_text kernel/graphics/input_events.c 'REFACTOR_P7A_WINDOW_EVENT_OWNER'
require_text kernel/graphics/input_events.c 'void window_event_schedule_wake_locked'
require_text kernel/graphics/input_events.c 'bool window_flush_event_wakes'
require_text kernel/graphics/input_events.c 'void window_enqueue_event_locked'
require_text kernel/graphics/input_events.c 'void window_enqueue_close_request_locked'
require_text kernel/graphics/input_events.c 'void window_enqueue_resize_event_locked'
require_text kernel/graphics/input_events.c \
    'void window_event_reset_ready_locked'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(void|bool)[[:space:]]+window_(event_schedule_wake_locked|flush_event_wakes|enqueue_event_locked|enqueue_close_request_locked|enqueue_resize_event_locked)[[:space:]]*\(' \
    kernel/graphics/input_router.c
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/input_events.c) ;;
        *)
            forbidden_text \
                'g_window_server\.event_ready_(count|overflow|windows)(\[[^]]+\])?[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/input_pump.c 'REFACTOR_P7K_INPUT_PUMP_OWNER'
require_text kernel/graphics/input_pump.c 'window_server_pump_input_mode'
require_text kernel/graphics/input_pump.c 'window_input_router_self_test'
require_text kernel/graphics/shell.c 'REFACTOR_P7A_SHELL_OWNER'
require_text kernel/graphics/shell.c 'void desktop_draw_wallpaper_locked'
require_text kernel/graphics/shell.c 'uint32_t desktop_app_at_locked'
require_text kernel/graphics/shell.c 'void desktop_mark_app_locked'
require_text kernel/graphics/shell.c \
    'void desktop_set_hovered_app_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/shell.c) ;;
        *)
            forbidden_text \
                'g_window_server\.desktop_hovered_app[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/input_router.c \
    'void window_input_reset_desktop_state_locked'
require_text kernel/graphics/input_router.c \
    'uint32_t window_input_take_desktop_launch_locked'
require_text kernel/graphics/input_router.c \
    'bool window_input_take_focus_cycle_request_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/input_router.c) ;;
        *)
            forbidden_text \
                'g_window_server\.(desktop_pending_launch|desktop_gui_mask|desktop_tab_consumed|desktop_focus_cycle_requested)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/input_router.c \
    'void window_clear_title_capture_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/input_router.c) ;;
        *)
            forbidden_text \
                'g_window_server\.title_pressed_(identifier|button|pointer_x|pointer_y|client|event)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/launcher.c 'REFACTOR_P7A_SHELL_LAUNCHER_OWNER'
require_text kernel/graphics/launcher.c 'kstatus_t desktop_launch_program'
require_text kernel/graphics/present_cursor.c 'REFACTOR_P7J_PRESENT_CURSOR_OWNER'
require_text kernel/graphics/present_cursor.c 'window_present_cursor_overlay'
require_text kernel/graphics/present_cursor.c 'window_present_self_test'
require_text kernel/graphics/present_cursor.c 'compositor_present_cursor_direct'
require_text kernel/graphics/present_cursor.c \
    'void window_present_cursor_reset_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/present_cursor.c) ;;
        *)
            forbidden_text \
                'g_window_server\.presented_pointer_(x|y|valid)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/present.c 'void compositor_copy_wc_scanline'
require_text kernel/graphics/damage.c 'window_mark_rect_locked'
require_text kernel/graphics/damage.c 'void window_damage_reset_locked'
require_text kernel/graphics/damage.c \
    'void window_damage_clear_pending_locked'
require_text kernel/graphics/damage.c 'void window_damage_rotate_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/damage.c) ;;
        *)
            forbidden_text \
                'g_window_server\.(dirty|damage_full|damage_tiles_active|damage_count|damage_bounds|damage_tiles|damage_rects)(\.[[:alnum:]_]+|\[[^]]+\])?[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/damage_plan.c 'REFACTOR_P7I_SNAPSHOT_DAMAGE_OWNER'
require_text kernel/graphics/damage_plan.c 'compositor_snapshot_tiles_to_rects'
require_text kernel/graphics/damage_plan.c 'compositor_snapshot_damage_bounds'
require_text kernel/graphics/render_plan.c 'REFACTOR_P7H_RENDER_PLAN_OWNER'
require_text kernel/graphics/render_plan.c 'compositor_build_render_plan'
require_text kernel/graphics/render_plan.c 'void compositor_render_plan_reset(void)'
require_text kernel/graphics/compositor.c 'compositor_snapshot_begin_locked'
require_text kernel/graphics/compositor.c 'compositor_snapshot_plan'
require_text kernel/graphics/compositor.c 'void compositor_render_snapshot(void)'
require_text kernel/graphics/compositor.c 'void compositor_snapshot_finish(void)'
require_text kernel/graphics/compositor.c 'compositor_render_plan_reset()'
require_text kernel/graphics/compositor.c \
    'void compositor_snapshot_test_clear_damage_tiles'
require_text kernel/graphics/compositor.c \
    'bool compositor_snapshot_test_set_damage_tile'
require_text kernel/graphics/compositor.c \
    'void compositor_reset_state_locked'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/compositor.c) ;;
        *)
            forbidden_text \
                'g_window_server\.composing[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/compositor.c 'void compositor_region_locked'
require_text kernel/graphics/render.c 'REFACTOR_P7B_SURFACE_RENDER_OWNER'
require_text kernel/graphics/render.c 'void compositor_draw_window_locked'
require_text kernel/graphics/render.c 'void compositor_surface_locked'
require_text kernel/graphics/render.c 'void compositor_surface_source_prepare'
require_text kernel/graphics/render.c 'void compositor_copy_surface_span'
require_text kernel/graphics/decorations.c 'REFACTOR_P7C_DECORATION_OWNER'
require_text kernel/graphics/decorations.c 'void compositor_titlebar_locked'
require_text kernel/graphics/cursor_occlusion.c 'REFACTOR_P7D_CURSOR_OCCLUSION_OWNER'
require_text kernel/graphics/cursor_occlusion.c 'void compositor_cursor_locked'
require_text kernel/graphics/cursor_occlusion.c 'uint32_t compositor_topmost_damage_cover'
require_text kernel/graphics/cursor_occlusion.c 'bool compositor_damage_inside_surface_interior'
require_text kernel/graphics/cursor_occlusion.c 'bool compositor_window_intersects_damage_locked'
require_text kernel/graphics/cursor_occlusion.c 'bool compositor_snapshot_overwrites_presented_cursor'
require_text kernel/graphics/raster.c 'REFACTOR_P7E_RASTER_OWNER'
require_text kernel/graphics/raster.c 'void compositor_fill_span_wb'
require_text kernel/graphics/raster.c 'void compositor_fill_locked'
require_text kernel/graphics/raster.c 'void compositor_fill_rounded_locked'
require_text kernel/graphics/tile_metadata.c 'REFACTOR_P7F_TILE_METADATA_OWNER'
require_text kernel/graphics/tile_metadata.c 'bool compositor_tile_metadata_build'
require_text kernel/graphics/tile_metadata.c 'bool compositor_tile_self_test'
require_text kernel/graphics/tile_metadata.c 'graphics.overlap_4'
require_text kernel/graphics/tile_metadata.c 'graphics.overlap_8'
require_text kernel/graphics/compositor.c 'void compositor_commit_snapshot'
require_text kernel/graphics/publication_policy.c 'REFACTOR_P7A_PUBLICATION_OWNER'
require_text kernel/graphics/publication_policy.c 'uint64_t compositor_publication_damage_pixels'
require_text kernel/graphics/publication_policy.c 'void compositor_publication_rect'
require_text kernel/graphics/publication_policy.c 'void compositor_publication_drag_old_exposure'
require_file kernel/graphics/compositor_drag.c
require_text kernel/graphics/compositor_drag.c 'REFACTOR_P7B_COMPOSITOR_DRAG_OWNER'
require_text kernel/graphics/compositor_drag.c 'void compositor_blit_drag_overlap'
require_text kernel/graphics/compositor.c 'const uint32_t g_linux_cursor_argb'
require_text kernel/graphics/compositor.c 'REFACTOR_P7B_FRAME_OWNER'
require_text kernel/graphics/shell.c 'static bool desktop_render_assets_locked'
require_text kernel/graphics/shell.c 'static void desktop_clear_region_locked'
require_text kernel/graphics/decorations.c 'void compositor_draw_small_glyph_locked'
forbidden_text 'desktop_(render_static_locked|draw_folder_icon_locked|draw_terminal_icon_locked|draw_notes_icon_locked|draw_network_icon_locked|put_pixel_locked|draw_small_glyph_locked)' \
    kernel/graphics/shell.c
forbidden_text '^[[:space:]]*void compositor_titlebar_locked' \
    kernel/graphics/compositor.c
forbidden_text '^[[:space:]]*void compositor_cursor_locked|^[[:space:]]*uint32_t compositor_topmost_damage_cover|^[[:space:]]*bool compositor_damage_inside_surface_interior|^[[:space:]]*bool compositor_window_intersects_damage_locked|^[[:space:]]*bool compositor_snapshot_overwrites_presented_cursor' \
    kernel/graphics/compositor.c
forbidden_text '^[[:space:]]*void compositor_fill_span_wb|^[[:space:]]*void compositor_fill_locked|^[[:space:]]*void compositor_fill_rounded_locked' \
    kernel/graphics/compositor.c
require_text kernel/graphics/tile_metadata.c 'touch_mask'
forbidden_text 'full_mask' kernel/graphics/internal.h kernel/graphics/tile_metadata.c
require_text kernel/graphics/tile_metadata.c 'opaque_mask'
require_text kernel/graphics/tile_metadata.c 'compositor_tile_occlusion_floor'
require_text kernel/graphics/tile_metadata.c 'compositor_occlusion_floor_for_damage'
require_text kernel/graphics/render_plan.c 'g_compositor_render_plan'
require_text kernel/graphics/compose_cpu.c 'compositor_copy_wb_pixels'
require_text kernel/graphics/compose_cpu.c 'bool compositor_copy_self_test'
require_text kernel/graphics/compose_cpu.c 'compositor_surface_cache_get'
require_text kernel/graphics/compose_cpu.c 'compositor_surface_page_resolve'
require_text kernel/graphics/compose_cpu.c 'compositor_surface_fill_destination'
require_text kernel/graphics/compose_cpu.c 'compositor_surface_copy_destination'
require_text kernel/graphics/compose_cpu.c 'compositor_fill_wb_pixels'
require_text kernel/graphics/compose_cpu.c 'compositor_fill_surface_pixels'
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/compositor.c) ;;
        *)
            forbidden_text \
                'g_compositor_snapshot\.[[:alnum:]_]+(\.[[:alnum:]_]+|\[[^]]+\])?[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
for path in kernel/graphics/*.c; do
    case "$path" in
        kernel/graphics/render_plan.c) ;;
        *)
            forbidden_text \
                'g_compositor_render_plan_(count|valid)[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/graphics/shell.c 'bool desktop_alpha_self_test'
forbidden_text '^void compositor_(draw_window_locked|surface_locked|surface_source_prepare|copy_surface_span)' \
    kernel/graphics/compositor.c
forbidden_text 'bool compositor_tile_metadata_build|bool compositor_tile_self_test|compositor_tile_meta_t g_compositor_tile_meta' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*compositor_render_span_t[[:space:]]*$|^[[:space:]]*uint32_t g_compositor_render_plan_count|^[[:space:]]*bool g_compositor_render_plan_valid' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*static uint8_t g_compositor_occlusion_floor|^[[:space:]]*static bool g_compositor_occlusion_floor_valid' \
    compat/graphics/window_server.c
forbidden_text 'compositor_snapshot_tiles_to_rects|compositor_build_occlusion_floor_cache|compositor_build_render_plan|compositor_occlusion_floor_for_damage|compositor_view_intersects_snapshot_damage|compositor_snapshot_occlusion_floor' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*void remove_window_locked|^[[:space:]]*uint32_t compositor_corner_inset|^[[:space:]]*void compositor_cursor_locked|^[[:space:]]*bool compositor_damage_inside_surface_interior|^[[:space:]]*uint32_t compositor_topmost_damage_cover' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(bool|void|uint32_t|int32_t)[[:space:]]+(window_title_capture_moved_locked|window_title_button_at_locked|window_event_schedule_wake_locked|window_flush_event_wakes|window_enqueue_event_locked|window_enqueue_close_request_locked|window_enqueue_resize_event_locked|window_minimize_locked|window_toggle_maximize_locked|window_resize_edges_locked|window_resize_locked|route_input_locked|window_route_motion_axis_locked|window_route_drag_motion_batch_locked|window_flush_motion_batch_locked|window_route_pointer_transaction_locked)[[:space:]]*' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*(static[[:space:]]+|static[[:space:]]+__attribute__\(\(noreturn\)\)[[:space:]]+)?(bool|void|uint32_t|kstatus_t)[[:space:]]+(desktop_shell_start_asset_worker|desktop_asset_worker_main|desktop_icon_layout_locked|desktop_put_pixel_locked|desktop_draw_small_glyph_locked|desktop_blend_asset_pixel_locked|desktop_draw_hover_frame_locked|desktop_draw_asset_icon_locked|desktop_draw_wallpaper_asset_locked|desktop_app_at_locked|desktop_mark_app_locked|desktop_draw_folder_icon_locked|desktop_draw_terminal_icon_locked|desktop_draw_notes_icon_locked|desktop_draw_network_icon_locked|desktop_draw_icon_locked|desktop_render_static_locked|desktop_load_asset|desktop_load_assets|desktop_asset_worker_never_ready|desktop_build_cache|desktop_copy_cached_region|desktop_draw_hover_locked|desktop_draw_wallpaper_locked|desktop_cycle_window_focus|desktop_restore_minimized_app|desktop_launch_program)[[:space:]]*' \
    compat/graphics/window_server.c
forbidden_text 'g_desktop_(asset|icons|wallpaper|file_manager|cache)' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(bool|void|uint32_t|const[[:space:]]+uint32_t)[[:space:]]+(compositor_window_intersects_damage_locked|compositor_title_controls_locked|compositor_titlebar_locked|compositor_commit_snapshot|compositor_blit_drag_overlap|compositor_snapshot_overwrites_presented_cursor|g_linux_cursor_argb)[[:space:]]*' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*void compositor_blit_drag_overlap' \
    kernel/graphics/compositor.c
forbidden_text 'bool compositor_snapshot_begin_locked|void compositor_snapshot_plan|void compositor_render_snapshot|void compositor_snapshot_finish|void compositor_region_locked|void compositor_draw_window_locked|void compositor_surface_locked|void compositor_surface_source_prepare|void compositor_copy_surface_span' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*(static inline )?void compositor_copy_wb_pixels|^[[:space:]]*void compositor_fill_wb_pixels|^[[:space:]]*void compositor_fill_surface_pixels|^[[:space:]]*compositor_surface_page_cache_t[[:space:]]*\*$|^[[:space:]]*uint8_t[[:space:]]*\*compositor_surface_page_resolve|^[[:space:]]*void compositor_surface_fill_destination|^[[:space:]]*void compositor_surface_copy_destination' \
    compat/graphics/window_server.c
forbidden_text '^[[:space:]]*void compositor_present_init|^[[:space:]]*bool compositor_present_start_copy_workers|^[[:space:]]*void compositor_copy_wc_scanline|^[[:space:]]*bool compositor_copy_rect_parallel_buffers|g_compositor_copy_|window_server_start_copy_workers|WINDOW_COMPOSITOR_COPY_' \
    compat/graphics/window_server.c

# Network socket protocol tests are kept out of the runtime Owner.  The
# private hook contract is explicit and does not add a production dispatch.
require_file kernel/net/socket_internal.h
require_text kernel/net/socket_internal.h 'REFACTOR_P8_SOCKET_INTERNAL'
require_file kernel/net/socket_model.h
require_text kernel/net/socket_model.h 'REFACTOR_P8_SOCKET_MODEL_OWNER'
require_text kernel/net/socket.c 'static socket_binding_t g_bindings[SOCKET_BINDING_COUNT]'
require_text kernel/net/socket.c 'static socket_tcp_connection_t g_tcp_connections[SOCKET_BINDING_COUNT]'
require_text kernel/net/socket.c 'static socket_tcp6_connection_t g_tcp6_connections[SOCKET_BINDING_COUNT]'
require_text kernel/net/socket.c 'static spinlock_t g_binding_lock'
require_text kernel/net/socket.c 'static atomic_uint g_socket_init_state'
require_text kernel/net/socket.c 'static atomic_uint g_next_ephemeral_port'
require_text kernel/net/socket.c 'static atomic_uint_fast64_t g_next_async_request_id'
for path in kernel/net/*.c; do
    case "$path" in
        kernel/net/socket.c) ;;
        *)
            forbidden_text 'g_bindings|g_tcp_connections|g_tcp6_connections|g_binding_lock|g_socket_init_state|g_next_ephemeral_port|g_next_async_request_id' "$path"
            ;;
    esac
done
require_file kernel/net/socket_transport.c
require_text kernel/net/socket_transport.c 'REFACTOR_P8_SOCKET_TRANSPORT_OWNER'
require_text kernel/net/socket_transport.c 'void socket_tcp_poll'
forbidden_text '^void socket_tcp_poll\(' kernel/net/socket.c
require_file kernel/net/socket_io.c
require_text kernel/net/socket_io.c 'REFACTOR_P8_SOCKET_IO_OWNER'
require_text kernel/net/socket_io.c 'kstatus_t socket_send'
require_text kernel/net/socket_io.c 'kstatus_t socket_recv'
forbidden_text '^[[:space:]]*static kstatus_t socket_send_stream\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_send\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_send_ipv6\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_send_async\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_send_async_ipv6\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_recv\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_recv_ipv6\(' kernel/net/socket.c
require_file kernel/net/socket_protocol.c
require_text kernel/net/socket_protocol.c 'REFACTOR_P8_SOCKET_PROTOCOL_OWNER'
require_text kernel/net/socket_protocol.c 'kstatus_t socket_inject_udp'
require_text kernel/net/socket_protocol.c 'kstatus_t socket_inject_tcp_syn_ipv4'
forbidden_text '^[[:space:]]*kstatus_t socket_inject_udp\(' kernel/net/socket.c
forbidden_text '^[[:space:]]*kstatus_t socket_inject_tcp_syn_ipv4\(' kernel/net/socket.c
require_file kernel/net/socket_test.c
require_text kernel/net/socket_test.c 'REFACTOR_P8_SOCKET_TEST_OWNER'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool[[:space:]]+socket_self_test[[:space:]]*\(' \
    kernel/net/socket.c
forbidden_text 'socket_test_tcp_output|socket_test_tcp6_output|SOCKET_LONG_TEST_' \
    kernel/net/socket.c

# Network core policy has a single firewall Owner.  The core runtime keeps
# routing/cache orchestration and may only call the public firewall contract.
require_file kernel/net/core/internal.h
require_text kernel/net/core/internal.h 'REFACTOR_P8_NET_INTERNAL'
require_file kernel/net/core/firewall.c
require_text kernel/net/core/firewall.c 'REFACTOR_P8_NET_FIREWALL_OWNER'
require_text kernel/net/core/firewall.c 'bool net_firewall_check'
require_text kernel/net/core/firewall.c 'bool net_firewall_self_test'
require_file kernel/net/core/self_test.c
require_text kernel/net/core/self_test.c 'REFACTOR_P8_NET_SELF_TEST_OWNER'
require_text kernel/net/core/self_test.c 'bool net_core_self_test'
require_text kernel/net/core/self_test.c 'bool net_tcp_self_test'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(void|kstatus_t|bool)[[:space:]]+net_firewall_(init|add|remove|check)\(' \
    kernel/net/core/core.c
forbidden_text '^[[:space:]]*bool net_firewall_self_test\(' kernel/net/core/core.c
forbidden_text '^[[:space:]]*bool net_(core|arp|ipv6|ndp|tcp)_self_test\(' \
    kernel/net/core/core.c

# Phase 8: driver primitives must be represented by their dedicated units.
for path in \
    kernel/drivers/usb/xhci/ring.c \
    kernel/drivers/usb/xhci/core.c \
    kernel/drivers/usb/xhci/interrupt.c \
    kernel/drivers/usb/xhci/runtime.c \
    kernel/drivers/usb/xhci/status.c \
    kernel/drivers/usb/xhci/self_test.c \
    kernel/drivers/usb/xhci/topology.c \
    kernel/drivers/usb/xhci/pci.c \
    kernel/drivers/usb/xhci/command.c \
    kernel/drivers/usb/xhci/command_runtime.c \
    kernel/drivers/usb/xhci/event.c \
    kernel/drivers/usb/xhci/event_runtime.c \
    kernel/drivers/usb/xhci/transfer.c \
    kernel/drivers/usb/xhci/device.c \
    kernel/drivers/usb/xhci/device_lifecycle.c \
    kernel/drivers/usb/xhci/control_transfer.c \
    kernel/drivers/usb/xhci/enumeration.c \
    kernel/drivers/usb/xhci/endpoint.c \
    kernel/drivers/usb/xhci/hid.c \
    kernel/drivers/usb/xhci/hid_runtime.c \
    kernel/drivers/usb/xhci/audio.c \
    kernel/drivers/usb/xhci/audio_runtime.c \
    kernel/drivers/usb/xhci/msc.c \
    kernel/drivers/usb/xhci/hub.c \
    kernel/drivers/usb/xhci/hub_runtime.c \
    kernel/drivers/usb/xhci/hub_transfer.c \
    kernel/drivers/usb/xhci/bluetooth.c \
    kernel/drivers/nvme/queue.c \
    kernel/drivers/nvme/core.c \
    kernel/drivers/nvme/completion.c \
    kernel/drivers/nvme/timing.c \
    kernel/drivers/nvme/pci.c \
    kernel/drivers/nvme/io.c \
    kernel/drivers/nvme/admin.c \
    kernel/drivers/nvme/namespace.c \
    kernel/drivers/net/core.c \
    kernel/drivers/net/rss.c \
    kernel/drivers/net/pci.c \
    kernel/drivers/net/queue.c \
    kernel/drivers/net/recovery.c; do
    require_file "$path"
done
require_text kernel/drivers/usb/xhci/transfer.c 'xhci_transfer_encode_normal'
require_text kernel/drivers/usb/xhci/transfer.c 'xhci_transfer_encode_isoch'
require_text kernel/drivers/usb/xhci/core.c 'REFACTOR_P8_XHCI_CORE_OWNER'
require_text kernel/drivers/usb/xhci/core.c 'xhci_core_initialize'
require_text kernel/drivers/usb/xhci/core.c 'static xhci_state_t g_xhci'
require_text kernel/drivers/usb/xhci/core.c 'xhci_state_t *xhci_controller_state'
require_text kernel/drivers/usb/xhci/internal.h 'xhci_state_t *xhci_controller_state'
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/core.c) ;;
        *) forbidden_text 'g_xhci([.]|[)]|,|;)' "$path" ;;
    esac
done
require_text kernel/drivers/usb/xhci/core.c 'REFACTOR_P8_XHCI_EVENT_LOCK_OWNER'
require_text kernel/drivers/usb/xhci/core.c 'void xhci_event_lock'
require_text kernel/drivers/usb/xhci/core.c 'bool xhci_event_try_lock'
require_text kernel/drivers/usb/xhci/core.c 'void xhci_event_unlock'
for path in \
    kernel/drivers/usb/xhci/runtime.c \
    kernel/drivers/usb/xhci/msc.c \
    kernel/drivers/usb/xhci/audio.c; do
    forbidden_text 'event_lock\.state' "$path"
done
require_text kernel/drivers/usb/xhci/interrupt.c \
    'REFACTOR_P8_XHCI_INTERRUPT_OWNER'
require_text kernel/drivers/usb/xhci/interrupt.c 'void xhci_interrupt_reset_state'
require_text kernel/drivers/usb/xhci/interrupt.c 'bool xhci_core_bind_msix'
require_text kernel/drivers/usb/xhci/interrupt.c 'void xhci_core_unbind_msix'
require_text kernel/drivers/usb/xhci/runtime.c \
    'REFACTOR_P8_XHCI_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/runtime.c \
    'REFACTOR_P8_XHCI_RUNTIME_STATE_OWNER'
require_text kernel/drivers/usb/xhci/runtime.c 'bool xhci_process_events'
require_text kernel/drivers/usb/xhci/runtime.c 'bool xhci_schedule_deferred_work'
require_text kernel/drivers/usb/xhci/runtime.c 'void xhci_deferred_work'
require_text kernel/drivers/usb/xhci/runtime.c 'void xhci_runtime_reset_state'
require_text kernel/drivers/usb/xhci/runtime.c 'bool xhci_runtime_ready'
require_text kernel/drivers/usb/xhci/runtime.c 'void xhci_runtime_set_ready'
require_text kernel/drivers/usb/xhci/lifecycle.c \
    'REFACTOR_P8_XHCI_LIFECYCLE_OWNER'
require_text kernel/drivers/usb/xhci/lifecycle.c \
    'bool xhci_controller_halt'
require_text kernel/drivers/usb/xhci/interrupt.c \
    'bool xhci_interrupt_take_pending'
require_text kernel/drivers/usb/xhci/interrupt.c \
    'uint32_t xhci_msix_failure_stage'
require_text kernel/drivers/usb/xhci/runtime.c 'bool xhci_runtime_ready'
require_text kernel/drivers/usb/xhci/runtime.c 'void xhci_runtime_set_ready'
require_text kernel/drivers/usb/xhci/status.c \
    'REFACTOR_P8_XHCI_STATUS_OWNER'
require_text kernel/drivers/usb/xhci/status.c \
    'REFACTOR_P8_XHCI_STATUS_STATE_OWNER'
require_text kernel/drivers/usb/xhci/status.c 'void xhci_set_error'
require_text kernel/drivers/usb/xhci/status.c 'void xhci_clear_error'
require_text kernel/drivers/usb/xhci/status.c 'void xhci_set_hardware_present'
require_text kernel/drivers/usb/xhci/status.c 'bool xhci_hardware_present'
require_text kernel/drivers/usb/xhci/status.c 'bool xhci_usb_device_enumerated'
require_text kernel/drivers/usb/xhci/status.c 'uint32_t xhci_usb_device_count'
require_text kernel/drivers/usb/xhci/status.c 'uint32_t xhci_last_error'
forbidden_text '^[[:space:]]*bool[[:space:]]+xhci_process_events[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*bool[[:space:]]+xhci_schedule_deferred_work[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*void[[:space:]]+xhci_deferred_work[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*void[[:space:]]+xhci_runtime_reset_state[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*bool[[:space:]]+xhci_hardware_present[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(bool|uint8_t|uint32_t)[[:space:]]+xhci_(usb_device_enumerated|usb_device_count|usb_hid_configured|usb_mouse_configured|usb_audio_configured|usb_hub_configured|usb_hub_port_count|usb_hub_downstream_configured|last_error)[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static[[:space:]]+void[[:space:]]+xhci_msix_handler[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*bool[[:space:]]+xhci_core_bind_msix[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static[[:space:]]+void[[:space:]]+xhci_unbind_msix[[:space:]]*\\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'XHCI_USBCMD' kernel/drivers/usb/xhci/core.c
forbidden_text 'XHCI_USBSTS' kernel/drivers/usb/xhci/core.c
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/status.c) ;;
        *) forbidden_text '^[[:space:]]*g_xhci_error[[:space:]]*=' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/status.c) ;;
        *) forbidden_text '^[[:space:]]*g_xhci_present[[:space:]]*=' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/topology.c) ;;
        *)
            forbidden_text '^[[:space:]]*g_xhci_usb_(enumerated|device_count|hid|mouse|hub|hub_ports|hub_present|hub_inventory_ports|hub_downstream|audio|bluetooth)[[:space:]]*=' "$path"
            ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/audio.c) ;;
        *) forbidden_text '^[[:space:]]*g_xhci_audio_slot[[:space:]]*=' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/runtime.c) ;;
        *) forbidden_text 'g_xhci_runtime_ready' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/interrupt.c) ;;
        *) forbidden_text 'g_xhci_irq_pending' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/interrupt.c) ;;
        *) forbidden_text 'g_xhci_msix_failure_stage' "$path" ;;
    esac
done
require_text kernel/drivers/usb/xhci/self_test.c \
    'REFACTOR_P8_XHCI_SELF_TEST_OWNER'
require_text kernel/drivers/usb/xhci/self_test.c 'bool xhci_hardware_self_test'
require_text kernel/drivers/usb/xhci/command_runtime.c \
    'REFACTOR_P8_XHCI_COMMAND_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/command_runtime.c \
    'bool xhci_submit_command_ex'
require_text kernel/drivers/usb/xhci/event_runtime.c \
    'REFACTOR_P8_XHCI_EVENT_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/event_runtime.c \
    'bool xhci_next_ring_event'
require_text kernel/drivers/usb/xhci/event_dispatch.c \
    'REFACTOR_P8_XHCI_EVENT_DISPATCH_OWNER'
require_text kernel/drivers/usb/xhci/event_dispatch.c \
    'bool xhci_process_event_ring'
require_text kernel/drivers/usb/xhci/topology.c 'REFACTOR_P8_XHCI_TOPOLOGY_OWNER'
require_text kernel/drivers/usb/xhci/topology.c \
    'static xhci_slot_device_t g_xhci_slot_devices[XHCI_MAX_SLOT_TABLE]'
require_text kernel/drivers/usb/xhci/topology.c \
    'xhci_slot_device_t *xhci_topology_slot'
require_text kernel/drivers/usb/xhci/internal.h \
    'xhci_slot_device_t *xhci_topology_slot'
for topology_query in \
    'bool xhci_topology_device_enumerated' \
    'uint32_t xhci_topology_device_count' \
    'bool xhci_topology_hid_configured' \
    'bool xhci_topology_mouse_configured' \
    'bool xhci_topology_audio_configured' \
    'bool xhci_topology_hub_configured' \
    'uint8_t xhci_topology_hub_port_count' \
    'bool xhci_topology_hub_downstream_configured' \
    'bool xhci_topology_bluetooth_configured'; do
    require_text kernel/drivers/usb/xhci/topology.c "$topology_query"
    require_text kernel/drivers/usb/xhci/internal.h "$topology_query"
done
for topology_state in \
    'static bool g_xhci_usb_enumerated' \
    'static uint32_t g_xhci_usb_device_count' \
    'static bool g_xhci_usb_hid' \
    'static bool g_xhci_usb_mouse' \
    'static bool g_xhci_usb_hub' \
    'static uint8_t g_xhci_usb_hub_ports' \
    'static bool g_xhci_usb_hub_present' \
    'static uint8_t g_xhci_usb_hub_inventory_ports' \
    'static bool g_xhci_usb_hub_downstream' \
    'static bool g_xhci_usb_audio' \
    'static bool g_xhci_usb_bluetooth'; do
    require_text kernel/drivers/usb/xhci/topology.c "$topology_state"
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/topology.c) ;;
        *) forbidden_text 'g_xhci_slot_devices' "$path" ;;
    esac
done
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/topology.c) ;;
        *)
            forbidden_text \
                'g_xhci_usb_(enumerated|device_count|hid|mouse|hub|hub_ports|hub_present|hub_inventory_ports|hub_downstream|audio|bluetooth)([^[:alnum:]_]|$)' \
                "$path"
            ;;
    esac
done
require_text kernel/drivers/usb/xhci/audio.c 'static uint8_t g_xhci_audio_slot'
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/audio.c) ;;
        *) forbidden_text 'g_xhci_audio_slot' "$path" ;;
    esac
done
require_text kernel/drivers/usb/xhci/topology.c 'void xhci_recompute_topology'
require_text kernel/drivers/usb/xhci/topology.c 'uint8_t xhci_context_kind'
require_text kernel/drivers/usb/xhci/topology.c 'bool xhci_topology_find_child'
require_text kernel/drivers/usb/xhci/topology.c 'bool xhci_topology_child_route'
require_text kernel/drivers/usb/xhci/topology.c 'bool xhci_topology_begin_slot'
require_text kernel/drivers/usb/xhci/topology.c 'void xhci_topology_publish_slot'
require_text kernel/drivers/usb/xhci/topology.c 'void xhci_topology_detach_slot'
require_text kernel/drivers/usb/xhci/topology.c 'void xhci_topology_clear_slot'
for path in kernel/drivers/usb/xhci/*.c; do
    case "$path" in
        kernel/drivers/usb/xhci/topology.c) ;;
        *)
            forbidden_text \
                '(^|[^[:alnum:]_])slot_dev->[[:space:]]*(used|slot_id|speed|device_class|device_protocol|parent_slot|parent_port|root_port|route_string|tt_slot|tt_port|tt_multi|is_hub|hub_port_count|hub_protocol)[[:space:]]*=' \
                "$path"
            forbidden_text \
                '(^|[^[:alnum:]_])parent->[[:space:]]*child_slots' \
                "$path"
            forbidden_text 'sizeof\(\*slot_dev\)' "$path"
            ;;
    esac
done
forbidden_text '^[[:space:]]*xhci_slot_device_t[[:space:]]+g_xhci_slot_devices' \
    kernel/drivers/usb/xhci/core.c
require_text kernel/drivers/usb/xhci/hid.c 'REFACTOR_P8_XHCI_HID_OWNER'
require_text kernel/drivers/usb/xhci/hid.c 'void xhci_hid_consume'
require_text kernel/drivers/usb/xhci/hid_runtime.c \
    'REFACTOR_P8_XHCI_HID_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/hid_runtime.c \
    'bool xhci_restart_hid_endpoint'
require_text kernel/drivers/usb/xhci/hid_runtime.c \
    'bool xhci_handle_hid_transfer_event'
require_text kernel/drivers/usb/xhci/hid_runtime.c \
    'void xhci_hid_runtime_initialize'
require_text kernel/drivers/usb/xhci/audio.c 'REFACTOR_P8_XHCI_AUDIO_OWNER'
require_text kernel/drivers/usb/xhci/audio.c 'xhci_audio_stream_configure'
require_text kernel/drivers/usb/xhci/audio_runtime.c \
    'REFACTOR_P8_XHCI_AUDIO_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/audio_runtime.c \
    'bool xhci_queue_audio_transfer_device'
require_text kernel/drivers/usb/xhci/audio_runtime.c \
    'bool xhci_configure_audio_endpoint'
require_text kernel/drivers/usb/xhci/audio_runtime.c \
    'bool xhci_handle_audio_transfer_event'
require_text kernel/drivers/usb/xhci/msc.c 'REFACTOR_P8_XHCI_MSC_OWNER'
require_text kernel/drivers/usb/xhci/msc.c 'xhci_msc_transfer_locked'
require_text kernel/drivers/usb/xhci/hub.c 'REFACTOR_P8_XHCI_HUB_OWNER'
require_text kernel/drivers/usb/xhci/hub.c 'xhci_hub_ack_all_port_changes_device'
require_text kernel/drivers/usb/xhci/hub_runtime.c 'REFACTOR_P8_XHCI_HUB_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/hub_runtime.c 'xhci_hub_runtime_handle_transfer_event'
require_text kernel/drivers/usb/xhci/hub_runtime.c 'xhci_hub_runtime_start'
require_text kernel/drivers/usb/xhci/hub_transfer.c \
    'REFACTOR_P8_XHCI_HUB_TRANSFER_OWNER'
require_text kernel/drivers/usb/xhci/hub_transfer.c \
    'bool xhci_queue_hub_status_device'
require_text kernel/drivers/usb/xhci/hub_transfer.c \
    'bool xhci_restart_hub_status_endpoint'
require_text kernel/drivers/usb/xhci/bluetooth.c 'REFACTOR_P8_XHCI_BLUETOOTH_OWNER'
require_text kernel/drivers/usb/xhci/bluetooth.c 'xhci_handle_bt_transfer_event'
require_text kernel/drivers/usb/xhci/pci.c 'xhci_pci_find_controller'
require_text kernel/drivers/usb/xhci/device.c 'xhci_device_context_clear'
require_text kernel/drivers/usb/xhci/device_lifecycle.c \
    'REFACTOR_P8_XHCI_DEVICE_LIFECYCLE_OWNER'
require_text kernel/drivers/usb/xhci/device_lifecycle.c \
    'bool xhci_free_slot_resources'
require_text kernel/drivers/usb/xhci/publication.c \
    'REFACTOR_P8_XHCI_PUBLICATION_OWNER'
require_text kernel/drivers/usb/xhci/publication.c \
    'bool xhci_publish_working_device'
require_text kernel/drivers/usb/xhci/root_runtime.c \
    'REFACTOR_P8_XHCI_ROOT_RUNTIME_OWNER'
require_text kernel/drivers/usb/xhci/root_runtime.c \
    'bool xhci_handle_root_port_event'
require_text kernel/drivers/usb/xhci/control_transfer.c \
    'REFACTOR_P8_XHCI_CONTROL_TRANSFER_OWNER'
require_text kernel/drivers/usb/xhci/control_transfer.c \
    'bool xhci_submit_control_transfer_device'
require_text kernel/drivers/usb/xhci/enumeration.c \
    'REFACTOR_P8_XHCI_ENUMERATION_OWNER'
require_text kernel/drivers/usb/xhci/enumeration.c \
    'bool xhci_enumerate_device'
require_text kernel/drivers/usb/xhci/endpoint.c 'xhci_endpoint_context_encode'
require_text kernel/drivers/nvme/admin.c 'nvme_admin_submit'
require_text kernel/drivers/nvme/core.c 'nvme_hardware_present'
require_text kernel/drivers/nvme/core.c 'nvme_driver_register'
require_text kernel/drivers/nvme/core.c 'REFACTOR_P8_NVME_CORE_OWNER'
require_text kernel/drivers/nvme/core.c 'void nvme_record_completion_status'
require_text kernel/drivers/nvme/core.c 'nvme_controller_t *nvme_controller_at'
require_text kernel/drivers/nvme/internal.h 'nvme_controller_t *nvme_controller_at'
for path in kernel/drivers/nvme/*.c; do
    case "$path" in
        kernel/drivers/nvme/core.c) ;;
        *) forbidden_text 'g_nvme_controllers' "$path" ;;
    esac
done
for path in kernel/drivers/nvme/*.c; do
    case "$path" in
        kernel/drivers/nvme/core.c) ;;
        *) forbidden_text 'g_nvme_hardware_seen' "$path" ;;
    esac
done
for path in kernel/drivers/nvme/*.c; do
    case "$path" in
        kernel/drivers/nvme/core.c) ;;
        *)
            forbidden_text \
                'g_nvme_last_completion[[:space:]]*=[[:space:]]*($|[^=])' \
                "$path"
            ;;
    esac
done
require_text kernel/drivers/nvme/completion.c \
    'REFACTOR_P8_NVME_COMPLETION_OWNER'
require_text kernel/drivers/nvme/completion.c 'void nvme_msix_handler'
require_text kernel/drivers/nvme/completion.c 'nvme_poll_device_completions'
require_text kernel/drivers/nvme/completion.c 'nvme_schedule_deferred_poll'
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(bool|void|uint32_t)[[:space:]]+nvme_(completion_ref_get|completion_ref_put|stop_completion_work|msix_handler|schedule_queue_completion|deferred_complete|abort_queue_pending|poll_queue_completions|poll_device_completions|schedule_deferred_poll)[[:space:]]*\(' \
    kernel/drivers/nvme/core.c
require_file kernel/drivers/nvme/self_test.c
require_text kernel/drivers/nvme/self_test.c \
    'REFACTOR_P8_NVME_SELF_TEST_OWNER'
require_text kernel/drivers/nvme/self_test.c 'nvme_hardware_io_self_test'
require_text kernel/drivers/nvme/self_test.c 'nvme_hardware_reset_self_test'
require_text kernel/drivers/nvme/self_test.c 'bool nvme_driver_self_test'
forbidden_text '^[[:space:]]*bool nvme_hardware_(io|reset)_self_test' \
    kernel/drivers/nvme/core.c
forbidden_text '^[[:space:]]*bool nvme_driver_self_test' \
    kernel/drivers/nvme/core.c
require_text kernel/drivers/nvme/pci.c 'nvme_pci_is_controller'
require_text kernel/drivers/nvme/timing.c 'REFACTOR_P8_NVME_TIMING_OWNER'
require_text kernel/drivers/nvme/timing.c 'nvme_deadline_reached'
require_text kernel/drivers/nvme/timing.c 'nvme_timeout_deadline'
require_text kernel/drivers/nvme/timing.c 'nvme_next_command_id'
require_text kernel/drivers/nvme/io.c 'REFACTOR_P8_NVME_IO_OWNER'
require_text kernel/drivers/nvme/io.c 'nvme_submit_io'
require_text kernel/drivers/net/queue.c 'e1000_queue_push'
require_text kernel/drivers/net/core.c 'e1000_hardware_present'
require_text kernel/drivers/net/core.c 'e1000_self_test'
require_text kernel/drivers/net/core_internal.h 'REFACTOR_P8_E1000_CORE_INTERNAL_MODEL'
require_text kernel/drivers/net/protocol.c 'REFACTOR_P8_E1000_PROTOCOL_OWNER'
require_text kernel/drivers/net/protocol.c 'e1000_tcp_ipv4_output'
require_text kernel/drivers/net/protocol.c 'e1000_tcp_ipv6_output'
require_text kernel/drivers/net/protocol.c 'e1000_udp_ipv4_output'
require_text kernel/drivers/net/protocol.c 'e1000_udp_ipv6_output'
require_text kernel/drivers/net/self_test.c 'REFACTOR_P8_E1000_SELF_TEST_OWNER'
require_text kernel/drivers/net/self_test.c 'e1000_self_test'
require_text kernel/drivers/net/runtime.c 'REFACTOR_P8_E1000_RUNTIME_OWNER'
require_text kernel/drivers/net/runtime.c 'e1000_poll_receive'
require_text kernel/drivers/net/rss.c 'REFACTOR_P8_E1000_RSS_OWNER'
require_text kernel/drivers/net/rss.c 'e1000_flow_hash_ipv4'
require_text kernel/drivers/net/rss.c 'e1000_flow_hash_ipv6'
require_text kernel/drivers/net/rss.c 'e1000_rss_self_test_state'
require_text kernel/drivers/net/pci.c 'e1000_pci_find'
require_text kernel/drivers/net/recovery.c 'REFACTOR_P8_E1000_RECOVERY_OWNER'
require_text kernel/drivers/net/recovery.c 'e1000_recovery_bind'
require_text kernel/drivers/gpu/core/core.c 'gpu_core_self_test'
for path in kernel/drivers/gpu/vendor/qemu/gpu.c compat/include/gpu.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: legacy GPU compatibility implementation remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
forbidden_text 'LITEOS_GPU_MANAGER|liteos_gpu_(manager|context|allocation|command|submit|fence)_' \
    kernel/init/main.c kernel/init/graphics.c kernel/init/self_tests.c
forbidden_text 'e1000_(msi_handler|bind_interrupt|unbind_interrupt)' \
    kernel/drivers/net/e1000.c
forbidden_text '^(bool|uint64_t|uint16_t) nvme_(deadline_reached|timeout_deadline|next_command_id)' \
    kernel/drivers/nvme/core.c
for path in compat/drivers/pci.c compat/drivers/nvme.c \
            compat/drivers/xhci.c \
            compat/include/pci.h compat/include/nvme.h \
            compat/include/usb.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: forbidden legacy driver ABI remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
for path in include/usb/device.h include/usb/hub.h; do
    require_file "$path"
done
require_file include/ascii_font.h
require_file kernel/graphics/ascii_font.c
require_file kernel/graphics/ascii_font_internal.h
require_file kernel/graphics/ascii_font_parser.c
require_text kernel/graphics/ascii_font_parser.c \
    'REFACTOR_P7A_FONT_PARSER_OWNER'
require_file include/uapi/font.h
require_file kernel/syscall/font.c
require_file user/font_runtime.h
require_text kernel/syscall/font.c 'syscall_font_cache'
require_text kernel/graphics/ascii_font.c 'ascii_font_copy_cache'
require_text user/font_runtime.h 'OS_SYS_FONT_CACHE'
forbidden_text 'font12x24|FONT_(TTF|RASTER_HEIGHT|GENERATOR|HEADER|SOURCE_HEADER|PREBUILT)|g_(upper|lower|symbol)_font|glyph_for' \
    makefile tools/build-windows.ps1 user
forbidden_text 'desktop_load_asset[(]|/etc/desktop/(wall|icons|fm)[.]raw|assets/desktop/.*[.](raw|rgba)' \
    kernel/graphics/assets.c makefile
for path in user/font12x24.h user/font12x24_data.h tools/font12x24_gen.c \
            assets/desktop/*.raw assets/desktop/*.rgba; do
    if [[ -e "$path" ]]; then
        printf 'refactor layout: removed font/image fallback path remains: %s\n' \
            "$path" >&2
        failures=1
    fi
done
for path in compat/include/usb/device.h compat/include/usb/hub.h; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: USB model header remains in compatibility tree: %s\n' \
            "$path" >&2
        failures=1
    fi
done
for path in compat/include/ascii_font.h compat/graphics/ascii_font.c; do
    if [[ -f "$path" ]]; then
        printf 'refactor layout: shared font asset remains in compatibility tree: %s\n' \
            "$path" >&2
        failures=1
    fi
done
require_text kernel/init/self_tests.c 'pci_current_host'
require_text kernel/init/self_tests.c 'nvme_driver_self_test'
forbidden_text 'LITEOS_(PCI_BUS|PCI_DEVICE|NVME_COMMAND|NVME_CONTROLLER|XHCI_CONTROLLER|USB_TRB)|liteos_(pci_|nvme_|usb_|xhci_)' \
    kernel/init/main.c kernel/init/graphics.c
require_file kernel/drivers/usb/xhci/lifecycle.c
require_text kernel/drivers/usb/xhci/lifecycle.c \
    'REFACTOR_P8_XHCI_LIFECYCLE_OWNER'
require_text kernel/drivers/usb/xhci/lifecycle.c \
    'bool xhci_controller_setup_rings'
require_text kernel/drivers/usb/xhci/lifecycle.c \
    'bool xhci_controller_handoff_legacy'
historical_sources=$(rg --files kernel include boot user tests tools 2>/dev/null |
    grep -E '(\.before-|\.bak$|\.step[0-9]|\.titlebar-|Zone\.Identifier)' || true)
if [[ -n "$historical_sources" ]]; then
    printf 'refactor layout: historical backup source remains under kernel/os:\n' >&2
    printf '%s\n' "$historical_sources" >&2
    failures=1
fi
forbidden_text 'static uint32_t e1000_(hash_bytes|hash_u16|hash_u32|flow_hash_ipv4|flow_hash_ipv6|select_software_queue)' \
    kernel/drivers/net/core.c
forbidden_text '^(static )?(kstatus_t|bool|uint32_t) e1000_(device_transmit|tcp_ipv4_output|tcp_ipv6_output|udp_ipv4_output|udp_ipv6_output|address6_equal|address6_zero|send_neighbor_advertisement|arp_reply|tcp_loopback_peer_ack6?)' \
    kernel/drivers/net/core.c
forbidden_text '^bool e1000_self_test\(' kernel/drivers/net/core.c
forbidden_text '^bool e1000_poll_receive' kernel/drivers/net/core.c
if [[ -f kernel/drivers/nvme/controller.c ]]; then
    printf 'refactor layout: forbidden legacy NVMe controller source: kernel/drivers/nvme/controller.c\n' >&2
    failures=1
fi
if [[ -f kernel/drivers/net/e1000.c ]]; then
    printf 'refactor layout: forbidden legacy e1000 controller source: kernel/drivers/net/e1000.c\n' >&2
    failures=1
fi
if [[ -f kernel/drivers/usb/xhci/controller.c ]]; then
    printf 'refactor layout: forbidden legacy xHCI controller source: kernel/drivers/usb/xhci/controller.c\n' >&2
    failures=1
fi
if grep -Fq -- 'xhci-core.o' makefile; then
    printf 'refactor layout: duplicate xHCI core object remains in makefile\n' >&2
    failures=1
fi
forbidden_text 'static bool xhci_hid_report_contains|static void xhci_hid_consume_keyboard_report|static void xhci_hid_consume_mouse_report|void xhci_hid_consume\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'xhci_audio_context\(|xhci_usb_audio_completed\(|xhci_audio_device\(|xhci_audio_stream_(configure|start|stop|queue|reset|disconnect)\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'xhci_msc_transfer_locked|xhci_usb_mass_storage_configured|xhci_usb_msc_query|xhci_usb_bulk_(session_begin|session_end|transfer_locked)|g_xhci_msc_transports|g_xhci_usb_msc|typedef struct xhci_msc_transport' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'static bool xhci_hub_(get_port_status_device|set_port_feature_device|clear_port_feature_device|ack_port_changes_device|ack_all_port_changes_device)|static uint8_t xhci_hub_port_speed' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'static (bool|void) xhci_(legacy_handle_hub_transfer_event|rearm_hub_interrupts|start_hub_runtime)' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'typedef struct xhci_bt_transport|g_xhci_bt_transports|static (bool|kstatus_t) xhci_(queue_bt|bt_send|configure_bt|handle_bt)' \
    kernel/drivers/usb/xhci/core.c
forbidden_text 'bool xhci_usb_bluetooth_configured' kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?void xhci_recompute_topology[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static uint8_t xhci_context_kind[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_submit_command_ex[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_(event_pending|next_ring_event|defer_event|next_event)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static bool xhci_process_event_ring[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_submit_control_transfer_device[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?(bool|void) xhci_(hid_consume_report|queue_hid_report|restart_hid_endpoint)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_handle_hid_transfer_event[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?void xhci_report_hid_completion_milestones[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_queue_audio_transfer_device[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_configure_audio_endpoint[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_handle_audio_transfer_event[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_(queue_hub_status_device|configure_hub_endpoint)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static bool xhci_(is_endpoint_transfer_event|drop_endpoint_transfer_events|restart_hub_status_endpoint)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*(static[[:space:]]+)?bool xhci_enumerate_device[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static (bool|void) xhci_(free_device_resources|release_working_device|release_slot_device|free_slot_resources|unpublish_slot_device)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static bool xhci_(move_device_context|publish_working_device)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*static (bool|void) xhci_(prepare_connected_port|attach_device|slot_has_root_port|remove_root_port_devices|attach_root_runtime_device|handle_root_port_event|reenumerate_self_test|probe_connected_ports|remove_device_subtree)[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c
forbidden_text '^[[:space:]]*bool xhci_hardware_self_test[[:space:]]*\(' \
    kernel/drivers/usb/xhci/core.c

# No pre-migration os source tree may remain after all kernel, header, user,
# test, and tool sources are rooted at the repository level.
for path in os; do
    if [[ -d "$path" ]] && find "$path" -type f -print -quit | grep -q .; then
        printf 'refactor layout: old source tree still contains files: %s\n' \
            "$path" >&2
        failures=1
    fi
done

if ((failures != 0)); then
    exit 1
fi
