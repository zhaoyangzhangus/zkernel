"""Serial cross-module host regressions; no image writes, QEMU or hardware use."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
groups = {
    'filesystem': ['block_cache_reservation', 'block_cache_perf', 'block_cache_copy',
                   'block_cache_writeback_batch', 'block_cache_stream_write',
                   'fat_sector_copy', 'fat_cluster_seek_perf', 'fat_bulk_read', 'fat_bulk_write',
                   'fat_lfn_invalidation',
                   'vfs_range_sync', 'vfs_resize_range', 'vfs_cached_exec_read',
                   'vfs_io_progress', 'vfs_user_write_batch', 'linux_file_io_progress', 'vfs_read_eof_revalidation',
                   'vfs_user_path', 'vfs_current_directory', 'vfs_registry_slots',
                   'vfs_backend_registration',
                   'vfs_volatile_prefix',
                   'vfs_mount_scan',
                   'vfs_mount_child',
                   'vfs_mount_registration',
                   'vfs_leaf_name',
                   'linux_getdents_batch',
                   'vfs_rename_replace', 'vfs_lookup_scan', 'vfs_fat_lookup_scan',
                   'vfs_directory_scan',
                   'vfs_enumeration_init',
                   'vfs_remove_scan',
                   'vfs_entry_name_scan',
                   'vfs_path_range_compare',
                   'vfs_subtree_match', 'vfs_memory_growth', 'vfs_cache_growth',
                   'vfs_copy_in_dirty', 'vfs_vnode_preempt',
                   'file_page_tail_zero', 'file_page_prefetch_fallback',
                   'file_page_physical',
                   'file_page_short_read'],
    'memory': [
        'bitmap_range', 'linux_bulk_memory', 'string_bulk', 'memcmp_bulk', 'kmalloc_size_class', 'kmalloc_once',
        'kmalloc_cpu_snapshot',
        'kmalloc_cache_cpu',
        'kzalloc_initialization', 'slab_creation', 'slab_free_poison', 'slab_empty_release', 'slab_take_payload',
        'page_table_guard', 'page_to_phys_lookup', 'buddy_boot_scan', 'buddy_runtime_lookup', 'page_compound_span',
        'page_pfn_lookup',
        'page_pool_lookup',
        'page_pool_slot',
        'page_pool_pinned_candidates', 'page_pool_drain',
        'page_pool_metadata_span',
        'page_pool_remote_drain',
        'page_free_pool_irq', 'page_alloc_reclaim_retry',
        'vm_once', 'vm_range_seek', 'vm_unmap_seek', 'vma_gap_prune', 'vma_linear_rebuild', 'linux_fixed_mapping_atomic', 'mprotect_index', 'mprotect_batch_transaction', 'vm_sparse_range', 'rss_batch', 'vmalloc_first_fit', 'vmalloc_lifecycle',
        'fork_cow_rollback', 'fork_sparse_walk',
        'fork_clone_cache',
        'anon_discard_range',
        'anon_index_clone',
        'anon_page_publish',
        'vm_object_creation',
        'vm_cow_bulk_copy',
        'vm_space_retire',
        'vm_space_creation',
        'surface_populate_batch',
        'vma_incremental_insert',
        'thread_initialization', 'exiting_user_fault', 'fp_state_lifecycle', 'fxsave_xmm_state'],
    'drivers': [
        'block_queue_init', 'bio_batch_validation', 'bio_vector_bounds',
        'dma_lifecycle', 'dma_iova_scan', 'iommu_mapping_guard', 'nvme_direct_page', 'nvme_bounce_copy',
        'device_object_init', 'device_registry', 'device_io_registration', 'display_initialization', 'input_motion_eviction',
        'input_pending_publication', 'input_motion_coalesce', 'input_batch', 'gpu_fence_notification',
        'firmware_name', 'firmware_lifecycle', 'power_lifecycle',
        'watchdog_lifecycle', 'xhci_event_guard', 'xhci_storage_hid',
        'xhci_event_batch_ack', 'xhci_runtime_handoff', 'audio_period_copy', 'audio_lifecycle',
        'bluetooth_lifecycle'],
    'sync': [
        'spinlock_contention', 'telemetry_ring', 'telemetry_snapshot_copy',
        'random_stream_lock',
        'font_cache_publication',
        'font_cache_initialization',
        'font_curve_bounds',
        'irq_registry_lock', 'mutex_pi_lifecycle', 'rcu_lifecycle', 'rcu_batch_copy',
        'message_port_io', 'completion_port_io',
        'pipe_bulk_copy', 'pipe_io_lifecycle', 'pipe_syscall_staging',
        'user_io_vector_staging',
        'user_io_creation_cleanup',
        'linux_unix_io_pin', 'linux_unix_nonblocking_lock',
        'linux_message_vectors',
        'linux_readv_progress',
        'linux_fd_entry_reference', 'linux_epoll_fork_alias',
        'linux_fd_table_lifecycle',
        'linux_stdio_reset',
        'linux_fd_install_snapshot', 'linux_fd_exhaustion',
        'linux_fd_exec_detach',
        'linux_poll_snapshot',
        'completion_token',
        'shared_section_lifecycle',
        'single_queue_wait',
        'wait_queue_sort',
        'wait_wake_scan',
        'wait_thread_pin',
        'wait_cancel_identity',
        'thread_stack_vm_pin',
        'thread_creation_publication',
        'thread_stack_registration',
        'thread_create_target_lifetime',
        'syscall_vm_snapshot',
        'linux_vm_snapshot',
        'linux_proc_maps_snapshot',
        'linux_brk_snapshot',
        'window_map_snapshot',
        'linux_window_create_cleanup',
        'native_window_cleanup',
        'gpu_map_snapshot',
        'current_fault_snapshot',
        'process_resource_release',
        'process_empty_exit',
        'process_wait_child_scan',
        'process_parent_detach_wake',
        'thread_monitor_cursor',
        'process_terminate_batch',
        'process_clone_vm_pin',
        'process_image_commit',
        'process_teardown_registry',
        'wait_timeout_scan',
        'wait_clock_fast', 'wait_delay_diagnostic',
        'futex_key_init',
        'handle_lifecycle', 'handle_exec_scan', 'job_lifecycle', 'service_lifecycle',
        'process_enumeration_lock',
        'process_enumeration_cursor',
        'process_thread_enumeration_cursor',
        'task_monitor',
        'package_registry', 'hrtimer_lifecycle', 'timer_global_guard', 'timer_registry_slot', 'timer_bitmap_slot', 'io_registry_lifecycle', 'io_submission_publication',
        'deferred_priority_fairness', 'deferred_lock', 'deferred_critical_slot', 'deferred_ring_publication', 'scheduler_cpu_choice', 'scheduler_affinity_update',
        'scheduler_command_drain', 'scheduler_command_publish',
        'scheduler_command_release', 'scheduler_release_cross_cpu',
        'remote_wake_mailbox'],
    'network': [
        'net_manager_once_lock', 'net_manager_poll', 'net_bulk_packet', 'net_queue_lifecycle', 'net_checksum',
        'net_lifecycle_preempt', 'net_tx_rx_exclusion', 'socket_initialization', 'socket_stream_ring', 'socket_slots',
        'socket_address',
        'socket_async_send', 'tcp_keepalive', 'tcp_receive_ack_sequence',
        'tcp_piggyback_ack'],
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', action='append', choices=groups,
                    help='Run only this group; repeat to select multiple groups.')
args = parser.parse_args()
selected = list(dict.fromkeys(args.group or groups))
tests = [(group, root / 'tests/kernel' / (name + '_test.py'))
         for group in selected for name in groups[group]]
for _, test in tests:
    if not test.is_file():
        parser.error(f'Missing regression: {test}')

started = time.monotonic()
failures = []
def run(label, command):
    print(f'\nRUN {label}', flush=True)
    try:
        result = subprocess.run(command, cwd=root, timeout=180)
        if result.returncode:
            failures.append((label, f'exit {result.returncode}'))
    except (OSError, subprocess.TimeoutExpired) as error:
        failures.append((label, str(error)))

for group, test in tests:
    run(f'{group}/{test.name}', [sys.executable, str(test)])

# Use the canonical host-test targets for complete FAT/cache implementations,
# not only extracted hot-path fixtures. These targets never write VM images.
if 'filesystem' in selected:
    before = len(failures)
    run('filesystem/fat_cache_compile', [
        'make', '-s', '-j2', '-f', 'GNUmakefile', 'BUILD=build', 'HOSTCC=cc',
        'build/fat32-test.exe', 'build/cache-test.exe'])
    if len(failures) == before:
        run('filesystem/fat32', [str(root / 'build/fat32-test.exe')])
        run('filesystem/block_cache', [str(root / 'build/cache-test.exe')])

# Exercise the actual subscription/object implementation as well as Python
# fixtures. Build only in a temporary directory, never in the guest image.
if 'sync' in selected:
    with tempfile.TemporaryDirectory(prefix='liteos-wait-regression-') as directory:
        binary = str(Path(directory) / 'test')
        before = len(failures)
        run('sync/wait_subscription_compile', [
            'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-Iinclude', '-DOS_WAIT_HOST_TEST', '-fsanitize=address,undefined',
            '-fno-pie', '-no-pie', 'kernel/sync/wait.c', 'kernel/object/object.c',
            'tests/kernel/wait_subscription_host.c',
            'tests/kernel/wait_subscription_test.c', '-o', binary])
        if len(failures) == before:
            run('sync/wait_subscription', [binary])

elapsed = time.monotonic() - started
print(f'\n{len(tests)} Python regressions; '
      f'FAT/cache {"included" if "filesystem" in selected else "not selected"}; '
      f'wait subscription {"included" if "sync" in selected else "not selected"}; '
      f'{len(failures)} failures; {elapsed:.1f}s', flush=True)
for label, reason in failures:
    print(f'FAIL {label}: {reason}', flush=True)
sys.exit(bool(failures))
