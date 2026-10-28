/*
 * UAPI ABI 编译期和运行期检查。
 *
 * 这个程序只依赖 include/uapi，不链接内核实现。它用于阻止结构体布局、
 * syscall 编号和版本化头部在发布时被无意修改。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <uapi/all.h>

#define ABI_ASSERT(condition, message) _Static_assert((condition), message)

ABI_ASSERT(sizeof(os_versioned_header_t) == 8, "versioned header size changed");
ABI_ASSERT(offsetof(os_versioned_header_t, size) == 0, "header.size offset changed");
ABI_ASSERT(offsetof(os_versioned_header_t, version) == 4, "header.version offset changed");
ABI_ASSERT(offsetof(os_versioned_header_t, flags) == 6, "header.flags offset changed");
ABI_ASSERT(sizeof(os_timespec_t) == 16, "timespec size changed");
ABI_ASSERT(offsetof(os_timespec_t, seconds) == 0, "timespec.seconds offset changed");
ABI_ASSERT(offsetof(os_timespec_t, nanoseconds) == 8, "timespec.nanoseconds offset changed");
ABI_ASSERT(offsetof(os_timespec_t, reserved) == 12, "timespec.reserved offset changed");
ABI_ASSERT(sizeof(os_exec_fd_entry_t) == 288,
           "exec fd entry ABI size changed");
ABI_ASSERT(offsetof(os_exec_fd_entry_t, descriptor) == 0,
           "exec fd entry.descriptor offset changed");
ABI_ASSERT(offsetof(os_exec_fd_entry_t, handle) == 8,
           "exec fd entry.handle offset changed");
ABI_ASSERT(offsetof(os_exec_fd_entry_t, path) == 32,
           "exec fd entry.path offset changed");
ABI_ASSERT(sizeof(os_exec_fd_map_t) == 73744,
           "exec fd map ABI size changed");
ABI_ASSERT(offsetof(os_exec_fd_map_t, entries) == 16,
           "exec fd map.entries offset changed");
ABI_ASSERT(sizeof(os_clock_set_t) == 32, "clock set ABI size changed");
ABI_ASSERT(offsetof(os_clock_set_t, hdr) == 0, "clock set.hdr offset changed");
ABI_ASSERT(offsetof(os_clock_set_t, clock_id) == 8, "clock set.clock_id offset changed");
ABI_ASSERT(offsetof(os_clock_set_t, value) == 16, "clock set.value offset changed");
ABI_ASSERT(sizeof(os_pipe_create_t) == 32, "pipe create ABI size changed");
ABI_ASSERT(offsetof(os_pipe_create_t, hdr) == 0, "pipe create.hdr offset changed");
ABI_ASSERT(offsetof(os_pipe_create_t, flags) == 8, "pipe create.flags offset changed");
ABI_ASSERT(offsetof(os_pipe_create_t, read_handle) == 16, "pipe create.read_handle offset changed");
ABI_ASSERT(offsetof(os_pipe_create_t, write_handle) == 24, "pipe create.write_handle offset changed");
ABI_ASSERT(sizeof(os_signal_action_t) == 32, "signal action ABI size changed");
ABI_ASSERT(offsetof(os_signal_action_t, handler) == 0,
           "signal action.handler offset changed");
ABI_ASSERT(offsetof(os_signal_action_t, mask) == 8,
           "signal action.mask offset changed");
ABI_ASSERT(offsetof(os_signal_action_t, restorer) == 16,
           "signal action.restorer offset changed");
ABI_ASSERT(offsetof(os_signal_action_t, flags) == 24,
           "signal action.flags offset changed");
ABI_ASSERT(offsetof(os_signal_action_t, reserved) == 28,
           "signal action.reserved offset changed");
ABI_ASSERT(sizeof(os_io_vec_t) == 16, "io vector ABI size changed");
ABI_ASSERT(offsetof(os_io_vec_t, address) == 0, "io vector.address offset changed");
ABI_ASSERT(offsetof(os_io_vec_t, length) == 8, "io vector.length offset changed");
ABI_ASSERT(sizeof(os_vm_map_args_t) == 48, "vm map ABI size changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, hdr) == 0, "vm map.hdr offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, address) == 8, "vm map.address offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, length) == 16, "vm map.length offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, offset) == 24, "vm map.offset offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, object) == 32, "vm map.object offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, prot) == 40, "vm map.prot offset changed");
ABI_ASSERT(offsetof(os_vm_map_args_t, flags) == 44, "vm map.flags offset changed");
ABI_ASSERT(sizeof(os_vm_share_args_t) == 32, "vm share ABI size changed");
ABI_ASSERT(offsetof(os_vm_share_args_t, hdr) == 0, "vm share.hdr offset changed");
ABI_ASSERT(offsetof(os_vm_share_args_t, size) == 8, "vm share.size offset changed");
ABI_ASSERT(offsetof(os_vm_share_args_t, flags) == 16, "vm share.flags offset changed");
ABI_ASSERT(offsetof(os_vm_share_args_t, reserved) == 20, "vm share.reserved offset changed");
ABI_ASSERT(offsetof(os_vm_share_args_t, section) == 24, "vm share.section offset changed");
ABI_ASSERT(sizeof(os_io_submit_t) == 56, "io submit ABI size changed");
ABI_ASSERT(offsetof(os_io_submit_t, hdr) == 0, "io submit.hdr offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, target) == 8, "io submit.target offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, completion_port) == 16, "io submit.completion_port offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, user_key) == 24, "io submit.user_key offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, offset) == 32, "io submit.offset offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, vectors) == 40, "io submit.vectors offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, vector_count) == 48, "io submit.vector_count offset changed");
ABI_ASSERT(offsetof(os_io_submit_t, opcode) == 52, "io submit.opcode offset changed");
ABI_ASSERT(sizeof(os_completion_entry_t) == 32, "completion ABI size changed");
ABI_ASSERT(offsetof(os_completion_entry_t, user_key) == 0, "completion.user_key offset changed");
ABI_ASSERT(offsetof(os_completion_entry_t, status) == 8, "completion.status offset changed");
ABI_ASSERT(offsetof(os_completion_entry_t, bytes_done) == 16, "completion.bytes_done offset changed");
ABI_ASSERT(offsetof(os_completion_entry_t, request_id) == 24, "completion.request_id offset changed");
ABI_ASSERT(sizeof(os_thread_create_t) == 48, "thread create ABI size changed");
ABI_ASSERT(offsetof(os_thread_create_t, hdr) == 0, "thread create.hdr offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, entry) == 8, "thread create.entry offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, stack_top) == 16, "thread create.stack_top offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, fs_base) == 24, "thread create.fs_base offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, argument) == 32, "thread create.argument offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, flags) == 40, "thread create.flags offset changed");
ABI_ASSERT(offsetof(os_thread_create_t, reserved) == 44, "thread create.reserved offset changed");
ABI_ASSERT(sizeof(os_wait_result_t) == 16, "wait result ABI size changed");
ABI_ASSERT(offsetof(os_wait_result_t, index) == 0, "wait result.index offset changed");
ABI_ASSERT(offsetof(os_wait_result_t, reserved) == 4, "wait result.reserved offset changed");
ABI_ASSERT(offsetof(os_wait_result_t, value) == 8, "wait result.value offset changed");
ABI_ASSERT(sizeof(os_wait_many_t) == 48, "wait many ABI size changed");
ABI_ASSERT(offsetof(os_wait_many_t, hdr) == 0, "wait many.hdr offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, count) == 8, "wait many.count offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, wait_flags) == 12, "wait many.wait_flags offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, handles) == 16, "wait many.handles offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, timeout_ns) == 24, "wait many.timeout_ns offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, result_index) == 32, "wait many.result_index offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, reserved) == 36, "wait many.reserved offset changed");
ABI_ASSERT(offsetof(os_wait_many_t, result_value) == 40, "wait many.result_value offset changed");
ABI_ASSERT(sizeof(os_file_info_t) == 80, "file info ABI size changed");
ABI_ASSERT(offsetof(os_file_info_t, type) == 0, "file info.type offset changed");
ABI_ASSERT(offsetof(os_file_info_t, mode) == 4, "file info.mode offset changed");
ABI_ASSERT(offsetof(os_file_info_t, size) == 8, "file info.size offset changed");
ABI_ASSERT(offsetof(os_file_info_t, name) == 16, "file info.name offset changed");
ABI_ASSERT(sizeof(os_file_enumerate_t) == 104, "file enumerate ABI size changed");
ABI_ASSERT(offsetof(os_file_enumerate_t, hdr) == 0, "file enumerate.hdr offset changed");
ABI_ASSERT(offsetof(os_file_enumerate_t, path) == 8, "file enumerate.path offset changed");
ABI_ASSERT(offsetof(os_file_enumerate_t, index) == 16, "file enumerate.index offset changed");
ABI_ASSERT(offsetof(os_file_enumerate_t, info) == 24, "file enumerate.info offset changed");
ABI_ASSERT(sizeof(os_file_seek_t) == 40, "file seek ABI size changed");
ABI_ASSERT(offsetof(os_file_seek_t, hdr) == 0, "file seek.hdr offset changed");
ABI_ASSERT(offsetof(os_file_seek_t, handle) == 8, "file seek.handle offset changed");
ABI_ASSERT(offsetof(os_file_seek_t, offset) == 16, "file seek.offset offset changed");
ABI_ASSERT(offsetof(os_file_seek_t, position) == 32, "file seek.position offset changed");
ABI_ASSERT(sizeof(os_file_stat_t) == 96, "file stat ABI size changed");
ABI_ASSERT(offsetof(os_file_stat_t, hdr) == 0, "file stat.hdr offset changed");
ABI_ASSERT(offsetof(os_file_stat_t, path) == 8, "file stat.path offset changed");
ABI_ASSERT(offsetof(os_file_stat_t, info) == 16, "file stat.info offset changed");
ABI_ASSERT(sizeof(os_file_truncate_t) == 24, "file truncate ABI size changed");
ABI_ASSERT(offsetof(os_file_truncate_t, handle) == 8, "file truncate.handle offset changed");
ABI_ASSERT(offsetof(os_file_truncate_t, size) == 16, "file truncate.size offset changed");
ABI_ASSERT(sizeof(os_file_path_op_t) == 24, "file path op ABI size changed");
ABI_ASSERT(offsetof(os_file_path_op_t, path) == 8, "file path op.path offset changed");
ABI_ASSERT(offsetof(os_file_path_op_t, mode) == 16, "file path op.mode offset changed");
ABI_ASSERT(sizeof(os_socket_async_send_t) == 56, "socket async ABI size changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, hdr) == 0, "socket async.hdr offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, socket) == 8, "socket async.socket offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, completion_port) == 16, "socket async.completion_port offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, user_key) == 24, "socket async.user_key offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, buffer) == 32, "socket async.buffer offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, length) == 40, "socket async.length offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, address) == 48, "socket async.address offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, port) == 52, "socket async.port offset changed");
ABI_ASSERT(offsetof(os_socket_async_send_t, flags) == 54, "socket async.flags offset changed");
ABI_ASSERT(sizeof(os_socket_ipv6_endpoint_t) == 20, "socket IPv6 endpoint ABI size changed");
ABI_ASSERT(offsetof(os_socket_ipv6_endpoint_t, address) == 0,
           "socket IPv6 endpoint.address offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_endpoint_t, port) == 16,
           "socket IPv6 endpoint.port offset changed");
ABI_ASSERT(sizeof(os_socket_ipv6_async_send_t) == 72,
           "socket IPv6 async ABI size changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, hdr) == 0,
           "socket IPv6 async.hdr offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, socket) == 8,
           "socket IPv6 async.socket offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, completion_port) == 16,
           "socket IPv6 async.completion_port offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, user_key) == 24,
           "socket IPv6 async.user_key offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, buffer) == 32,
           "socket IPv6 async.buffer offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, length) == 40,
           "socket IPv6 async.length offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, address) == 48,
           "socket IPv6 async.address offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, port) == 64,
           "socket IPv6 async.port offset changed");
ABI_ASSERT(offsetof(os_socket_ipv6_async_send_t, flags) == 66,
           "socket IPv6 async.flags offset changed");
ABI_ASSERT(sizeof(os_socket_shutdown_t) == 24,
           "socket shutdown ABI size changed");
ABI_ASSERT(offsetof(os_socket_shutdown_t, hdr) == 0,
           "socket shutdown.hdr offset changed");
ABI_ASSERT(offsetof(os_socket_shutdown_t, socket) == 8,
           "socket shutdown.socket offset changed");
ABI_ASSERT(offsetof(os_socket_shutdown_t, how) == 16,
           "socket shutdown.how offset changed");
ABI_ASSERT(offsetof(os_socket_shutdown_t, reserved) == 20,
           "socket shutdown.reserved offset changed");
ABI_ASSERT(sizeof(os_gpu_create_context_t) == 24, "gpu context ABI size changed");
ABI_ASSERT(offsetof(os_gpu_create_context_t, hdr) == 0, "gpu context.hdr offset changed");
ABI_ASSERT(offsetof(os_gpu_create_context_t, device) == 8, "gpu context.device offset changed");
ABI_ASSERT(offsetof(os_gpu_create_context_t, flags) == 16, "gpu context.flags offset changed");
ABI_ASSERT(sizeof(os_gpu_alloc_t) == 24, "gpu alloc ABI size changed");
ABI_ASSERT(offsetof(os_gpu_alloc_t, hdr) == 0, "gpu alloc.hdr offset changed");
ABI_ASSERT(offsetof(os_gpu_alloc_t, size) == 8, "gpu alloc.size offset changed");
ABI_ASSERT(offsetof(os_gpu_alloc_t, flags) == 16, "gpu alloc.flags offset changed");
ABI_ASSERT(offsetof(os_gpu_alloc_t, reserved) == 20, "gpu alloc.reserved offset changed");
ABI_ASSERT(sizeof(os_gpu_map_t) == 48, "gpu map ABI size changed");
ABI_ASSERT(offsetof(os_gpu_map_t, hdr) == 0, "gpu map.hdr offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, allocation) == 8, "gpu map.allocation offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, address) == 16, "gpu map.address offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, offset) == 24, "gpu map.offset offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, length) == 32, "gpu map.length offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, prot) == 40, "gpu map.prot offset changed");
ABI_ASSERT(offsetof(os_gpu_map_t, flags) == 44, "gpu map.flags offset changed");
ABI_ASSERT(sizeof(os_gpu_submit_t) == 56, "gpu submit ABI size changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, hdr) == 0, "gpu submit.hdr offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, context) == 8, "gpu submit.context offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, command_buffer) == 16, "gpu submit.command_buffer offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, signal_fence) == 24, "gpu submit.signal_fence offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, signal_value) == 32, "gpu submit.signal_value offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, command_offset) == 40, "gpu submit.command_offset offset changed");
ABI_ASSERT(offsetof(os_gpu_submit_t, command_length) == 48, "gpu submit.command_length offset changed");
ABI_ASSERT(sizeof(os_display_commit_t) == 88, "display commit ABI size changed");
ABI_ASSERT(offsetof(os_display_commit_t, hdr) == 0, "display commit.hdr offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, output) == 8, "display commit.output offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, flags) == 12, "display commit.flags offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, buffer) == 16, "display commit.buffer offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, offset) == 24, "display commit.offset offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, stride) == 32, "display commit.stride offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, width) == 40, "display commit.width offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, height) == 44, "display commit.height offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, format) == 48, "display commit.format offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, reserved) == 52, "display commit.reserved offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, wait_fence) == 56, "display commit.wait_fence offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, wait_value) == 64, "display commit.wait_value offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, signal_fence) == 72, "display commit.signal_fence offset changed");
ABI_ASSERT(offsetof(os_display_commit_t, signal_value) == 80, "display commit.signal_value offset changed");
ABI_ASSERT(sizeof(os_device_open_t) == 32, "device open ABI size changed");
ABI_ASSERT(offsetof(os_device_open_t, hdr) == 0, "device open.hdr offset changed");
ABI_ASSERT(offsetof(os_device_open_t, device_id) == 8, "device open.device_id offset changed");
ABI_ASSERT(offsetof(os_device_open_t, desired_rights) == 16, "device open.desired_rights offset changed");
ABI_ASSERT(offsetof(os_device_open_t, reserved) == 20, "device open.reserved offset changed");
ABI_ASSERT(offsetof(os_device_open_t, handle) == 24, "device open.handle offset changed");
ABI_ASSERT(sizeof(os_device_info_t) == 32, "device info ABI size changed");
ABI_ASSERT(offsetof(os_device_info_t, hdr) == 0, "device info.hdr offset changed");
ABI_ASSERT(offsetof(os_device_info_t, device_id) == 8, "device info.device_id offset changed");
ABI_ASSERT(offsetof(os_device_info_t, class_id) == 16, "device info.class_id offset changed");
ABI_ASSERT(offsetof(os_device_info_t, state) == 20, "device info.state offset changed");
ABI_ASSERT(offsetof(os_device_info_t, flags) == 24, "device info.flags offset changed");
ABI_ASSERT(offsetof(os_device_info_t, reserved) == 28, "device info.reserved offset changed");
ABI_ASSERT(sizeof(os_device_control_t) == 48, "device control ABI size changed");
ABI_ASSERT(offsetof(os_device_control_t, hdr) == 0, "device control.hdr offset changed");
ABI_ASSERT(offsetof(os_device_control_t, code) == 8, "device control.code offset changed");
ABI_ASSERT(offsetof(os_device_control_t, flags) == 12, "device control.flags offset changed");
ABI_ASSERT(offsetof(os_device_control_t, level_or_state) == 16, "device control.level_or_state offset changed");
ABI_ASSERT(offsetof(os_device_control_t, reserved) == 20, "device control.reserved offset changed");
ABI_ASSERT(offsetof(os_device_control_t, output) == 24, "device control.output offset changed");
ABI_ASSERT(offsetof(os_device_control_t, output_size) == 32, "device control.output_size offset changed");
ABI_ASSERT(offsetof(os_device_control_t, bytes_returned) == 40, "device control.bytes_returned offset changed");
ABI_ASSERT(sizeof(os_device_enumerate_t) == 40, "device enumerate ABI size changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, hdr) == 0, "device enumerate.hdr offset changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, index) == 8, "device enumerate.index offset changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, reserved) == 12, "device enumerate.reserved offset changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, output) == 16, "device enumerate.output offset changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, output_size) == 24, "device enumerate.output_size offset changed");
ABI_ASSERT(offsetof(os_device_enumerate_t, bytes_returned) == 32, "device enumerate.bytes_returned offset changed");
ABI_ASSERT(sizeof(os_audio_stream_config_t) == 28, "audio config ABI size changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, hdr) == 0, "audio config.hdr offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, direction) == 8, "audio config.direction offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, sample_rate) == 12, "audio config.sample_rate offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, channels) == 16, "audio config.channels offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, sample_format) == 18, "audio config.sample_format offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, period_frames) == 20, "audio config.period_frames offset changed");
ABI_ASSERT(offsetof(os_audio_stream_config_t, period_count) == 24, "audio config.period_count offset changed");
ABI_ASSERT(sizeof(os_audio_stream_stats_t) == 56, "audio stats ABI size changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, hdr) == 0, "audio stats.hdr offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, queued_frames) == 8, "audio stats.queued_frames offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, mixed_frames) == 16, "audio stats.mixed_frames offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, underruns) == 24, "audio stats.underruns offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, overruns) == 32, "audio stats.overruns offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, device_generation) == 40, "audio stats.device_generation offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, state) == 48, "audio stats.state offset changed");
ABI_ASSERT(offsetof(os_audio_stream_stats_t, reserved) == 52, "audio stats.reserved offset changed");
ABI_ASSERT(sizeof(os_audio_open_t) == 48, "audio open ABI size changed");
ABI_ASSERT(offsetof(os_audio_open_t, hdr) == 0, "audio open.hdr offset changed");
ABI_ASSERT(offsetof(os_audio_open_t, config) == 8, "audio open.config offset changed");
ABI_ASSERT(offsetof(os_audio_open_t, handle) == 40, "audio open.handle offset changed");
ABI_ASSERT(sizeof(os_audio_control_t) == 48, "audio control ABI size changed");
ABI_ASSERT(offsetof(os_audio_control_t, hdr) == 0, "audio control.hdr offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, code) == 8, "audio control.code offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, period) == 12, "audio control.period offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, frames) == 16, "audio control.frames offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, flags) == 20, "audio control.flags offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, buffer) == 24, "audio control.buffer offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, buffer_size) == 32, "audio control.buffer_size offset changed");
ABI_ASSERT(offsetof(os_audio_control_t, bytes_returned) == 40, "audio control.bytes_returned offset changed");
ABI_ASSERT(sizeof(os_display_info_t) == 32, "display info ABI size changed");
ABI_ASSERT(offsetof(os_display_info_t, hdr) == 0, "display info.hdr offset changed");
ABI_ASSERT(offsetof(os_display_info_t, width) == 16, "display info.width offset changed");
ABI_ASSERT(offsetof(os_display_info_t, height) == 20, "display info.height offset changed");
ABI_ASSERT(offsetof(os_display_info_t, stride) == 24, "display info.stride offset changed");
ABI_ASSERT(offsetof(os_display_info_t, format) == 28, "display info.format offset changed");
ABI_ASSERT(sizeof(os_input_event_t) == 24, "input event ABI size changed");
ABI_ASSERT(offsetof(os_input_event_t, timestamp) == 0, "input event.timestamp offset changed");
ABI_ASSERT(offsetof(os_input_event_t, code) == 16, "input event.code offset changed");
ABI_ASSERT(sizeof(os_window_create_t) == 96, "window create ABI size changed");
ABI_ASSERT(offsetof(os_window_create_t, title) == 32, "window create.title offset changed");
ABI_ASSERT(offsetof(os_window_create_t, window) == 64, "window create.window offset changed");
ABI_ASSERT(offsetof(os_window_create_t, address) == 80, "window create.address offset changed");
ABI_ASSERT(sizeof(os_window_info_t) == 80, "window info ABI size changed");
ABI_ASSERT(offsetof(os_window_info_t, buffer_size) == 40, "window info.buffer_size offset changed");
ABI_ASSERT(sizeof(os_window_enumerate_t) == 96, "window enumerate ABI size changed");
ABI_ASSERT(offsetof(os_window_enumerate_t, info) == 16, "window enumerate.info offset changed");
ABI_ASSERT(sizeof(os_window_map_t) == 32, "window map ABI size changed");
ABI_ASSERT(sizeof(os_window_set_t) == 28, "window set ABI size changed");
ABI_ASSERT(sizeof(os_window_event_t) == 64, "window event ABI size changed");
ABI_ASSERT(sizeof(os_window_event_read_t) == 88, "window event read ABI size changed");
ABI_ASSERT(sizeof(os_window_input_dispatch_t) == 40, "window dispatch ABI size changed");

static int abi_runtime_checks(void) {
    if (OS_SYSCALL_ABI_VERSION != 1U || OS_INVALID_HANDLE != 0 ||
        OS_CLOCK_REALTIME != 0U || OS_CLOCK_MONOTONIC != 1U) return 1;
    if (OS_SYS_THREAD_EXIT != 0x0000 || OS_SYS_PROCESS_FORK != 0x0006 ||
        OS_SYS_PROCESS_WAIT != 0x0007 || OS_SYS_THREAD_CONTEXT != 0x0008 ||
        OS_SYS_VM_MAP != 0x0100 ||
        OS_SYS_VM_SYNC != 0x0104 || OS_SYS_VM_ADVISE != 0x0105 ||
        OS_SYS_HANDLE_CLOSE != 0x0200 || OS_SYS_FILE_OPEN != 0x0300 ||
        OS_SYS_FILE_ENUMERATE != 0x0304 ||
        OS_SYS_FILE_SEEK != 0x0305 || OS_SYS_FILE_STAT != 0x0306 ||
        OS_SYS_FILE_TRUNCATE != 0x0307 || OS_SYS_FILE_REMOVE != 0x0308 ||
        OS_SYS_FILE_MKDIR != 0x0309 ||
        OS_SYS_PORT_CREATE != 0x0400 || OS_SYS_SOCKET_CREATE != 0x0500 ||
        OS_SYS_SOCKET_SHUTDOWN != 0x0510 ||
        OS_SYS_DEVICE_OPEN != 0x0600 || OS_SYS_DEVICE_ENUMERATE != 0x0602 ||
        OS_SYS_GPU_CREATE_CTX != 0x0700 ||
        OS_SYS_DISPLAY_COMMIT != 0x0710 ||
        OS_SYS_DISPLAY_GET_INFO != 0x0711 ||
        OS_SYS_AUDIO_OPEN != 0x0720 || OS_SYS_AUDIO_CONTROL != 0x0721 ||
        OS_SYS_CLOCK_GET != 0x0900 || OS_SYS_CLOCK_SET != 0x0902 ||
        OS_SYS_EXCEPTION_RETURN != 0x0910 ||
        OS_SYS_SIGNAL_ACTION != 0x0911 || OS_SYS_SIGNAL_MASK != 0x0912 ||
        OS_SYS_SIGNAL_SEND != 0x0913 ||
        OS_SYS_INPUT_READ != 0x0A00 ||
        OS_SYS_WINDOW_REGISTER_MANAGER != 0x0C00 ||
        OS_SYS_WINDOW_EVENT_READ != 0x0C08) return 1;
    if (OS_WAIT_MAX_HANDLES != 1024U || OS_WAIT_INDEX_ALL != UINT32_MAX ||
        OS_WAIT_INFINITE != UINT64_MAX) return 1;
    if (OS_PIPE_BUF != 4096U || OS_PIPE_DEFAULT_SIZE != 65536U ||
        (OS_PIPE_FLAG_CLOEXEC | OS_PIPE_FLAG_NONBLOCK) != OS_PIPE_FLAG_MASK) return 1;
    if ((OS_VM_READ | OS_VM_WRITE | OS_VM_EXEC) != 7U) return 1;
    if ((OS_VM_SYNC_ASYNC | OS_VM_SYNC_INVALIDATE | OS_VM_SYNC_SYNC) != 7U ||
        OS_VM_ADVICE_NORMAL != 0U || OS_VM_ADVICE_RANDOM != 1U ||
        OS_VM_ADVICE_SEQUENTIAL != 2U || OS_VM_ADVICE_WILLNEED != 3U ||
        OS_VM_ADVICE_DONTNEED != 4U) return 1;
    if (OS_THREAD_CONTEXT_GET_FS != 0U ||
        OS_THREAD_CONTEXT_SET_FS != 1U) return 1;
    if (OS_SIGNAL_COUNT != 64U || OS_SIGKILL != 9U || OS_SIGSTOP != 19U ||
        OS_SIGNAL_UNBLOCKABLE_MASK != ((1ULL << 8U) | (1ULL << 18U))) return 1;
    return 0;
}

int main(void) {
    if (abi_runtime_checks() != 0) return 1;
    puts("uapi-abi: ok");
    return 0;
}
