#pragma once

#include <kernel/process.h>
#include <uapi/abi.h>

/* 系统调用处理程序的私有约定；不属于用户 ABI。 */
process_t *current_process(void);
/* Bounded Chrome-only stderr capture, shared by Linux and native writes. */
void linux_chrome_stderr_write(process_t *process, const char *text,
                              size_t length);
bool versioned_header_valid(const os_versioned_header_t *header,
                            size_t required_size);
int64_t syscall_thread_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4,
                            uint64_t unused5);
int64_t syscall_process_exit(uint64_t status, uint64_t unused1, uint64_t unused2,
                             uint64_t unused3, uint64_t unused4,
                             uint64_t unused5);
int64_t syscall_linux_activate(uint64_t unused0, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_memory_info(uint64_t output_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_process_exec(uint64_t path, uint64_t argv, uint64_t envp,
                             uint64_t descriptor_map, uint64_t unused4,
                             uint64_t unused5);
int64_t syscall_process_info(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_process_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_thread_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_process_fork(uint64_t unused0, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t frame_pointer);
int64_t syscall_process_clone_linux(uint64_t flags, uint64_t child_stack,
                                    uint64_t parent_tid, uint64_t child_tid,
                                    uint64_t frame_pointer);
int64_t syscall_process_wait(uint64_t pid, uint64_t options,
                             uint64_t status_pointer, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_thread_create(uint64_t process_handle,
                              uint64_t arguments_pointer,
                              uint64_t output_pointer, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_thread_context(uint64_t operation, uint64_t fs_base,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_thread_signal(uint64_t thread_handle, uint64_t signal,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_task_query(uint64_t pointer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_process_terminate(uint64_t pointer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_process_create(uint64_t flags, uint64_t output_pointer,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_signal_action(uint64_t signal, uint64_t action_pointer,
                              uint64_t old_pointer, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_signal_mask(uint64_t how, uint64_t set_pointer,
                            uint64_t old_pointer, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_signal_send(uint64_t pid, uint64_t signal,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_signal_return(uint64_t frame_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
uint32_t syscall_thread_create_stage(void);
int64_t syscall_vm_map(uint64_t arguments_pointer, uint64_t unused1,
                       uint64_t unused2, uint64_t unused3,
                       uint64_t unused4, uint64_t unused5);
int64_t syscall_vm_share(uint64_t arguments_pointer, uint64_t unused1,
                         uint64_t unused2, uint64_t unused3,
                         uint64_t unused4, uint64_t unused5);
int64_t syscall_vm_unmap(uint64_t address, uint64_t length, uint64_t unused2,
                         uint64_t unused3, uint64_t unused4, uint64_t unused5);
int64_t syscall_vm_protect(uint64_t address, uint64_t length,
                           uint64_t protection, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_vm_sync(uint64_t address, uint64_t length, uint64_t flags,
                        uint64_t unused3, uint64_t unused4,
                        uint64_t unused5);
int64_t syscall_vm_advise(uint64_t address, uint64_t length, uint64_t advice,
                          uint64_t unused3, uint64_t unused4,
                          uint64_t unused5);
uint32_t translate_vm_protection(uint32_t protection);
int64_t syscall_handle_close(uint64_t handle, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_handle_dup(uint64_t handle, uint64_t flags,
                           uint64_t output_pointer, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_handle_get_flags(uint64_t handle, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_handle_set_flags(uint64_t handle, uint64_t flags,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_create(uint64_t family, uint64_t type, uint64_t protocol,
                              uint64_t output_pointer, uint64_t create_flags,
                              uint64_t unused5);
int64_t syscall_net_enum_adapter(uint64_t pointer,uint64_t a,uint64_t b,
    uint64_t c,uint64_t d,uint64_t e);
int64_t syscall_net_get_status(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_net_subscribe(uint64_t port_handle, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_net_set_ipv4(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_bind(uint64_t handle, uint64_t address, uint64_t port,
                            uint64_t unused3, uint64_t unused4,
                            uint64_t unused5);
int64_t syscall_socket_connect(uint64_t handle, uint64_t address, uint64_t port,
                               uint64_t unused3, uint64_t unused4,
                               uint64_t unused5);
int64_t syscall_socket_bind6(uint64_t handle, uint64_t address_pointer,
                             uint64_t port, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_connect6(uint64_t handle, uint64_t address_pointer,
                                uint64_t port, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_listen(uint64_t handle, uint64_t backlog,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_accept(uint64_t handle, uint64_t timeout_ns,
                              uint64_t output_handle, uint64_t create_flags,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_send(uint64_t handle, uint64_t buffer, uint64_t length,
                            uint64_t address, uint64_t port, uint64_t flags);
int64_t syscall_socket_send6(uint64_t handle, uint64_t buffer, uint64_t length,
                             uint64_t address_pointer, uint64_t port,
                             uint64_t flags);
int64_t syscall_socket_send_async(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_send_async6(uint64_t arguments_pointer, uint64_t unused1,
                                   uint64_t unused2, uint64_t unused3,
                                   uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_recv(uint64_t handle, uint64_t buffer, uint64_t length,
                            uint64_t source_address, uint64_t source_port,
                            uint64_t timeout_ns);
int64_t syscall_socket_recv6(uint64_t handle, uint64_t buffer, uint64_t length,
                             uint64_t source_address, uint64_t source_port,
                             uint64_t timeout_ns);
int64_t syscall_socket_get_info(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_get_option(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_set_option(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_socket_shutdown(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5);
uint32_t translate_vm_protection(uint32_t protection);
int64_t syscall_gpu_create_context(uint64_t arguments_pointer,
                                      uint64_t output_pointer, uint64_t unused2,
                                      uint64_t unused3, uint64_t unused4,
                                      uint64_t unused5);
int64_t syscall_gpu_alloc(uint64_t arguments_pointer, uint64_t output_pointer,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_gpu_submit(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_gpu_wait_fence(uint64_t handle, uint64_t value,
                                  uint64_t timeout_ns, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_gpu_map(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_theme_settings(uint64_t pointer,uint64_t a,uint64_t b,
    uint64_t c,uint64_t d,uint64_t e);
int64_t syscall_display_get_info(uint64_t arguments_pointer,
                                    uint64_t unused1, uint64_t unused2,
                                    uint64_t unused3, uint64_t unused4,
                                    uint64_t unused5);
int64_t syscall_font_cache(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_image_info(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_image_decode(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_window_register_manager(uint64_t unused0, uint64_t unused1,
                                           uint64_t unused2, uint64_t unused3,
                                           uint64_t unused4, uint64_t unused5);
int64_t syscall_window_create(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_thread_terminate(uint64_t pointer, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_window_update(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_clipboard_set(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_clipboard_get(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_window_title(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_window_control_tree(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t syscall_window_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5);
int64_t syscall_window_map(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_window_set(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_window_focus(uint64_t arguments_pointer, uint64_t unused1,
                                uint64_t unused2, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5);
int64_t syscall_window_input_read(uint64_t event_pointer, uint64_t timeout_ns,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5);
int64_t syscall_window_input_dispatch(uint64_t arguments_pointer,
                                         uint64_t unused1, uint64_t unused2,
                                         uint64_t unused3, uint64_t unused4,
                                         uint64_t unused5);
int64_t syscall_window_event_read(uint64_t arguments_pointer, uint64_t unused1,
                                     uint64_t unused2, uint64_t unused3,
                                     uint64_t unused4, uint64_t unused5);
int64_t syscall_display_commit(uint64_t arguments_pointer,
                                  uint64_t unused1, uint64_t unused2,
                                  uint64_t unused3, uint64_t unused4,
                                  uint64_t unused5);
int64_t syscall_file_open(uint64_t path, uint64_t flags, uint64_t mode,
                             uint64_t output_pointer, uint64_t unused4,
                             uint64_t unused5);
int64_t syscall_file_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5);
int64_t syscall_file_enumerate_batch(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5);
int64_t syscall_file_seek(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_file_stat(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);
int64_t syscall_file_fstat(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_file_truncate(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_file_remove(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_file_mkdir(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_file_chdir(uint64_t path, uint64_t unused1, uint64_t unused2,
                           uint64_t unused3, uint64_t unused4, uint64_t unused5);
int64_t syscall_file_getcwd(uint64_t buffer, uint64_t size, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4, uint64_t unused5);
int64_t syscall_file_rename(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_pipe_create(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_pipe_read(uint64_t handle, uint64_t buffer, uint64_t length,
                          uint64_t timeout_ns, uint64_t output_bytes,
                          uint64_t unused5);
int64_t syscall_pipe_write(uint64_t handle, uint64_t buffer, uint64_t length,
                           uint64_t timeout_ns, uint64_t output_bytes,
                           uint64_t unused5);
int64_t syscall_file_read(uint64_t handle, uint64_t buffer, uint64_t length,
                             uint64_t output_bytes, uint64_t unused4,
                             uint64_t unused5);
int64_t syscall_file_write(uint64_t handle, uint64_t buffer, uint64_t length,
                              uint64_t output_bytes, uint64_t unused4,
                              uint64_t unused5);
int64_t syscall_file_fsync(uint64_t handle, uint64_t unused1, uint64_t unused2,
                              uint64_t unused3, uint64_t unused4,
                              uint64_t unused5);
int64_t syscall_audio_settings(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_audio_open(uint64_t arguments_pointer, uint64_t unused1,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_audio_control(uint64_t handle, uint64_t arguments_pointer,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5);
int64_t syscall_debug_write(uint64_t buffer, uint64_t length,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_device_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5);
int64_t syscall_device_open(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_device_control(uint64_t handle, uint64_t arguments_pointer,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5);
int64_t syscall_port_create(uint64_t kind, uint64_t capacity,
                            uint64_t output_pointer, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5);
int64_t syscall_completion_wait(uint64_t handle, uint64_t timeout_ns,
                                uint64_t output_pointer, uint64_t unused3,
                                uint64_t unused4, uint64_t unused5);
int64_t syscall_clock_get(uint64_t clock_id, uint64_t output_pointer,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);
int64_t syscall_clock_set(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);
int64_t syscall_random_get(uint64_t buffer, uint64_t length,
                           uint64_t unused2, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_timer_create(uint64_t delay_ns, uint64_t period_ns,
                             uint64_t output_pointer, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_port_send(uint64_t handle, uint64_t buffer, uint64_t size,
                          uint64_t unused3, uint64_t unused4,
                          uint64_t unused5);
int64_t syscall_port_receive(uint64_t handle, uint64_t buffer, uint64_t capacity,
                             uint64_t output_size, uint64_t timeout_ns,
                             uint64_t unused5);
int64_t syscall_wait_one(uint64_t handle, uint64_t timeout_ns,
                         uint64_t output_pointer, uint64_t unused3,
                         uint64_t unused4, uint64_t unused5);
int64_t syscall_wait_many(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);
int64_t syscall_futex_wait(uint64_t address, uint64_t expected,
                           uint64_t timeout_ns, uint64_t flags,
                           uint64_t unused4, uint64_t unused5);
int64_t syscall_futex_wake(uint64_t address, uint64_t maximum, uint64_t flags,
                           uint64_t unused3, uint64_t unused4,
                           uint64_t unused5);
int64_t syscall_io_submit(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);
int64_t syscall_system_power(uint64_t action, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5);
int64_t syscall_io_cancel(uint64_t request_id, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5);

int64_t syscall_cpu_get_usage(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
int64_t syscall_gpu_get_info(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);

int64_t syscall_desktop_wallpaper(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
