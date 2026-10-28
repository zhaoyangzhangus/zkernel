#pragma once
#pragma once
#include "abi.h"

/* 原始 Linux 系统调用直接使用 RAX 中的调用号。进程切换到 Linux 兼容模式后，
 * 共用的 System libc 会为原生 ABI 调用设置此最高位标记，此时低位调用号会重叠。 */
#define OS_SYSCALL_NATIVE_FLAG (1ULL << 63)

/* FUTEX_WAIT / FUTEX_WAKE 标志：零表示使用进程私有键；设置此标志时，
 * 映射同一物理字的进程将使用共享键。 */
#define OS_FUTEX_FLAG_SHARED 0x1U

enum os_syscall_number {
    OS_SYS_THREAD_EXIT       = 0x0000,
    OS_SYS_PROCESS_EXIT      = 0x0001,
    OS_SYS_THREAD_CREATE     = 0x0002,
    OS_SYS_PROCESS_CREATE    = 0x0003,
    OS_SYS_PROCESS_EXEC      = 0x0004,
    OS_SYS_PROCESS_INFO      = 0x0005,
    OS_SYS_PROCESS_FORK      = 0x0006,
    OS_SYS_PROCESS_WAIT      = 0x0007,
    OS_SYS_THREAD_CONTEXT    = 0x0008,
    OS_SYS_PROCESS_ENUMERATE = 0x0009,
    OS_SYS_THREAD_ENUMERATE  = 0x000A,
    OS_SYS_THREAD_SIGNAL     = 0x000B,
    /* 系统 ELF 加载器在跳转到 Linux PT_INTERP 主映像前使用原生系统调用，
     * 随后显式启用 Linux 兼容模式。 */
    OS_SYS_LINUX_ACTIVATE    = 0x000C,
    OS_SYS_MEMORY_INFO       = 0x000D,
    OS_SYS_TASK_QUERY        = 0x000E,
    OS_SYS_PROCESS_TERMINATE = 0x000F,
    OS_SYS_THREAD_TERMINATE  = 0x0010,
    OS_SYS_CPU_GET_USAGE     = 0x0011,

    OS_SYS_VM_MAP            = 0x0100,
    OS_SYS_VM_UNMAP          = 0x0101,
    OS_SYS_VM_PROTECT        = 0x0102,
    OS_SYS_VM_SHARE          = 0x0103,
    OS_SYS_VM_SYNC           = 0x0104,
    OS_SYS_VM_ADVISE         = 0x0105,

    OS_SYS_HANDLE_CLOSE      = 0x0200,
    OS_SYS_WAIT_ONE          = 0x0201,
    OS_SYS_WAIT_MANY         = 0x0202,
    OS_SYS_FUTEX_WAIT        = 0x0203,
    OS_SYS_FUTEX_WAKE        = 0x0204,
    OS_SYS_HANDLE_DUP        = 0x0205,
    OS_SYS_HANDLE_GET_FLAGS  = 0x0206,
    OS_SYS_HANDLE_SET_FLAGS  = 0x0207,

    OS_SYS_FILE_OPEN         = 0x0300,
    OS_SYS_FILE_READ         = 0x0301,
    OS_SYS_FILE_WRITE        = 0x0302,
    OS_SYS_FILE_FSYNC        = 0x0303,
    OS_SYS_FILE_ENUMERATE    = 0x0304,
    OS_SYS_FILE_SEEK         = 0x0305,
    OS_SYS_FILE_STAT         = 0x0306,
    OS_SYS_FILE_TRUNCATE     = 0x0307,
    OS_SYS_FILE_REMOVE       = 0x0308,
    OS_SYS_FILE_MKDIR        = 0x0309,
    OS_SYS_FILE_FSTAT        = 0x030A,
    OS_SYS_FILE_RENAME       = 0x030B,
    OS_SYS_FILE_ENUMERATE_BATCH = 0x030C,
    /* CHDIR(path)；GETCWD(buffer, size) 返回含终止符的字节数。 */
    OS_SYS_FILE_CHDIR        = 0x030D,
    OS_SYS_FILE_GETCWD       = 0x030E,
    OS_SYS_IO_SUBMIT         = 0x0310,
    OS_SYS_IO_CANCEL         = 0x0311,
    OS_SYS_PIPE_CREATE       = 0x0312,
    OS_SYS_PIPE_READ         = 0x0313,
    OS_SYS_PIPE_WRITE        = 0x0314,

    OS_SYS_PORT_CREATE       = 0x0400,
    OS_SYS_PORT_SEND         = 0x0401,
    OS_SYS_PORT_RECEIVE      = 0x0402,
    OS_SYS_COMPLETION_WAIT   = 0x0410,

    OS_SYS_SOCKET_CREATE     = 0x0500,
    OS_SYS_SOCKET_BIND       = 0x0501,
    OS_SYS_SOCKET_CONNECT    = 0x0502,
    OS_SYS_SOCKET_LISTEN     = 0x0503,
    OS_SYS_SOCKET_ACCEPT     = 0x0504,
    OS_SYS_SOCKET_SEND       = 0x0505,
    OS_SYS_SOCKET_RECV       = 0x0506,
    OS_SYS_SOCKET_SEND_ASYNC = 0x0507,
    OS_SYS_SOCKET_BIND6      = 0x0508,
    OS_SYS_SOCKET_CONNECT6   = 0x0509,
    OS_SYS_SOCKET_SEND6      = 0x050A,
    OS_SYS_SOCKET_RECV6      = 0x050B,
    OS_SYS_SOCKET_SEND_ASYNC6 = 0x050C,
    OS_SYS_SOCKET_GET_INFO   = 0x050D,
    OS_SYS_SOCKET_GET_OPTION = 0x050E,
    OS_SYS_SOCKET_SET_OPTION = 0x050F,
    OS_SYS_SOCKET_SHUTDOWN   = 0x0510,

    OS_SYS_DEVICE_OPEN       = 0x0600,
    OS_SYS_DEVICE_CONTROL    = 0x0601,
    OS_SYS_DEVICE_ENUMERATE  = 0x0602,

    OS_SYS_GPU_CREATE_CTX    = 0x0700,
    OS_SYS_GPU_ALLOC         = 0x0701,
    OS_SYS_GPU_MAP           = 0x0702,
    OS_SYS_GPU_SUBMIT        = 0x0703,
    OS_SYS_GPU_WAIT_FENCE    = 0x0704,
    OS_SYS_GPU_GET_INFO      = 0x0705,
    OS_SYS_DISPLAY_COMMIT    = 0x0710,
    OS_SYS_DISPLAY_GET_INFO  = 0x0711,
    OS_SYS_THEME_SETTINGS    = 0x0712,

    OS_SYS_FONT_CACHE        = 0x0730,
    OS_SYS_IMAGE_INFO        = 0x0731,
    OS_SYS_IMAGE_DECODE      = 0x0732,

    OS_SYS_AUDIO_OPEN        = 0x0720,
    OS_SYS_AUDIO_CONTROL     = 0x0721,
    OS_SYS_AUDIO_SETTINGS    = 0x0722,

    OS_SYS_CLOCK_GET         = 0x0900,
    OS_SYS_TIMER_CREATE      = 0x0901,
    OS_SYS_CLOCK_SET         = 0x0902,
    OS_SYS_RANDOM_GET        = 0x0903,
    OS_SYS_EXCEPTION_RETURN  = 0x0910,
    OS_SYS_SIGNAL_ACTION     = 0x0911,
    OS_SYS_SIGNAL_MASK       = 0x0912,
    OS_SYS_SIGNAL_SEND       = 0x0913,
    OS_SYS_DEBUG_QUERY       = 0x0920,
    OS_SYS_DEBUG_WRITE       = 0x0921,
    /* SYSTEM_POWER(action)：0 表示重启，1 表示关机。 */
    OS_SYS_SYSTEM_POWER      = 0x0930,

    OS_SYS_INPUT_READ        = 0x0A00,

    OS_SYS_NET_ENUM_ADAPTER  = 0x0B03,
    OS_SYS_NET_GET_STATUS    = 0x0B00,
    OS_SYS_NET_SET_IPV4     = 0x0B01,
    OS_SYS_NET_SUBSCRIBE    = 0x0B02,

    OS_SYS_WINDOW_REGISTER_MANAGER = 0x0C00,
    OS_SYS_WINDOW_CREATE           = 0x0C01,
    OS_SYS_WINDOW_ENUMERATE        = 0x0C02,
    OS_SYS_WINDOW_MAP              = 0x0C03,
    OS_SYS_WINDOW_SET              = 0x0C04,
    OS_SYS_WINDOW_FOCUS            = 0x0C05,
    OS_SYS_WINDOW_INPUT_READ       = 0x0C06,
    OS_SYS_WINDOW_INPUT_DISPATCH   = 0x0C07,
    OS_SYS_WINDOW_EVENT_READ       = 0x0C08,
    OS_SYS_WINDOW_UPDATE           = 0x0C09,
    OS_SYS_WINDOW_TITLE            = 0x0C0A,
    OS_SYS_WINDOW_CONTROL_TREE     = 0x0C0B,
    OS_SYS_CLIPBOARD_SET           = 0x0C0C,
    OS_SYS_CLIPBOARD_GET           = 0x0C0D,
    OS_SYS_DESKTOP_WALLPAPER       = 0x0C0E,
};

#define OS_SYSCALL_ABI_VERSION 1u

/* OS_SYS_EXCEPTION_RETURN 是信号恢复入口。 */
#define OS_SYS_SIGNAL_RETURN OS_SYS_EXCEPTION_RETURN

/*
 * x86_64 原始 ABI：
 *   RAX = 系统调用号
 *   RDI、RSI、RDX、R10、R8、R9 = 参数 0..5
 *   RAX = 有符号结果；负值为稳定的错误码
 *   RCX 和 R11 会被破坏。
 *
 * 核心调用参数（ABI 版本 1）：
 *   THREAD_EXIT(status)：线程退出
 *   PROCESS_EXIT(status)：进程退出
 *   PROCESS_EXEC(path, argv, envp, os_exec_fd_map_t *)：执行程序
 *   THREAD_CREATE(target_process, os_thread_create_t *, os_handle_t *,
 *                 可选的 os_thread_stack_t *)：创建线程
 *   THREAD_CONTEXT(operation, fs_base)：GET_FS 时返回当前 FS 基址
 *   THREAD_SIGNAL(thread_handle, signal)：向线程发送信号
 *   PROCESS_CREATE(flags, os_handle_t *, 可选名称)：创建进程
 *   PROCESS_FORK()：父进程返回子进程 PID，子进程返回 0
 *   PROCESS_WAIT(pid, options, int32_t *status)：返回子进程 PID
 *   VM_MAP(os_vm_map_args_t *)：映射虚拟内存
 *   VM_UNMAP(address, length)：取消虚拟内存映射
 *   VM_PROTECT(address, length, protection)：修改内存保护属性
 *   VM_SHARE(os_vm_share_args_t *)：返回共享区句柄
 *   VM_SYNC(address, length, flags)：同步内存映射
 *   VM_ADVISE(address, length, advice)：提供虚拟内存使用建议
 *   HANDLE_CLOSE(handle)：关闭句柄
 *   HANDLE_DUP(handle, flags, os_handle_t *)：复制句柄
 *   HANDLE_GET_FLAGS(handle)：返回句柄标志
 *   HANDLE_SET_FLAGS(handle, flags)：设置句柄标志
 *   WAIT_ONE(handle, timeout_ns, os_wait_result_t *)：等待单个对象
 *   WAIT_MANY(os_wait_many_t *)：等待多个对象
 *   FUTEX_WAIT(address, expected_u32, timeout_ns, flags)：等待 futex 值
 *   FUTEX_WAKE(address, maximum_count, flags)：唤醒 futex 等待者
 *   DEVICE_OPEN(os_device_open_t *)：打开设备
 *   DEVICE_CONTROL(handle, os_device_control_t *)：控制设备
 *   IO_SUBMIT(os_io_submit_t *)：返回请求 ID
 *   IO_CANCEL(request_id)：取消请求
 *   FILE_ENUMERATE(os_file_enumerate_t *)：枚举文件
 *   FILE_SEEK(os_file_seek_t *)：移动文件偏移
 *   FILE_STAT(os_file_stat_t *)：查询文件信息
 *   FILE_TRUNCATE(os_file_truncate_t *)：截断文件
 *   FILE_REMOVE(os_file_path_op_t *)：删除文件
 *   FILE_MKDIR(os_file_path_op_t *)：创建目录
 *   FILE_FSTAT(os_file_handle_stat_t *)：通过句柄查询文件信息
 *   FILE_RENAME(os_file_rename_t *)：重命名文件
 *   PIPE_CREATE(os_pipe_create_t *)：创建管道
 *   PIPE_READ(handle, buffer, length, timeout_ns, output_bytes)：读取管道
 *   PIPE_WRITE(handle, buffer, length, timeout_ns, output_bytes)：写入管道
 *   DISPLAY_COMMIT(os_display_commit_t *)：提交显示内容
 *   DISPLAY_GET_INFO(os_display_info_t *)：查询显示信息
 *   FONT_CACHE(os_font_cache_request_t *)：管理字体缓存
 *   IMAGE_INFO(os_image_info_t *)：查询图像信息
 *   IMAGE_DECODE(os_image_decode_t *)：解码图像
 *   INPUT_READ(os_input_event_t *, timeout_ns)：读取输入事件
 *   WINDOW_CREATE(os_window_create_t *)：创建窗口
 *   WINDOW_ENUMERATE(os_window_enumerate_t *)：枚举窗口
 *   WINDOW_MAP(os_window_map_t *)：映射窗口缓冲区
 *   WINDOW_EVENT_READ(os_window_event_read_t *)：读取窗口事件
 *   CLOCK_GET(clock_id, os_timespec_t *)：读取时钟
 *   CLOCK_SET(os_clock_set_t *)：设置时钟
 *   RANDOM_GET(buffer, length)：获取随机数据
 * target_process 可设为 OS_INVALID_HANDLE，以选择调用进程。
 */
