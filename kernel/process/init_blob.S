#ifdef __ELF__
    .section .rdata,"a",@progbits
#else
    .section .rdata,"dr"
#endif
#ifndef OS_AUTOSTART_CHROME
#define OS_AUTOSTART_CHROME 0
#endif
    .p2align 4
    .globl user_init_blob_start
    .globl user_init_blob_end
    .globl user_init_service

/*
 * 这是随内核编译的最小用户态 /init 映像。
 *
 * 它故意只使用稳定 UAPI：创建进程、创建线程、等待对象和关闭句柄。
 * 用户态只保留设备、日志、音频、图形 Shell 和网络等客户服务；窗口
 * 服务器和合成器属于 Ring0。启动自测模式会等待服务退出，常驻模式
 * 则保留服务生命周期并让它们阻塞等待工作。
 */
    .set SYS_THREAD_EXIT,     0x0000
    .set SYS_PROCESS_EXEC,    0x0004
    .set SYS_PROCESS_CREATE,  0x0003
    .set SYS_THREAD_CREATE,   0x0002
    .set SYS_HANDLE_CLOSE,    0x0200
    .set SYS_WAIT_ONE,        0x0201
    .set SYS_DEVICE_OPEN,     0x0600
    .set SYS_DEVICE_CONTROL,  0x0601
    .set SYS_DEVICE_ENUMERATE, 0x0602
    .set SYS_AUDIO_OPEN,       0x0720
    .set SYS_AUDIO_CONTROL,    0x0721

    .set K_ETIMEDOUT,          -110
    .set INIT_WAIT_SLICE_NS,   1000000000
    .set INIT_WAIT_RETRIES,    4

    .set INIT_STAGE,           0x00401000
    .set INIT_RESULT,          0x00401008
    .set INIT_SERVICE_OK,      0x00401010
    .set INIT_PERSISTENT,      0x00401040
    .set INIT_PERSISTENT_WORD, 0x00401044
    .set INIT_PROCESS_HANDLE,  0x00401018
    .set INIT_THREAD_HANDLE,   0x00401020
    .set INIT_WAIT_RESULT,     0x00401030
    .set INIT_THREAD_ARGS,     0x00401050
    .set INIT_DEVICE_OPEN,     0x00401090
    .set INIT_DEVICE_OPEN_HANDLE, 0x004010A8
    .set INIT_DEVICE_CONTROL,  0x004010C0
    .set INIT_DEVICE_INFO,      0x00401100
    .set INIT_DEVICE_ENUMERATE, 0x00401140
    .set INIT_AUDIO_PATH,      0x00401170
    .set INIT_DEVICED_PATH,    0x00401500
    .set INIT_LOGD_PATH,       0x00401520
    .set INIT_CRASHD_PATH,     0x00401540
    .set INIT_CHROME_PATH,     0x00401560
    .set INIT_AUDIO_OPEN,       0x00401180
    .set INIT_AUDIO_OPEN_HANDLE, 0x004011A8
    .set INIT_AUDIO_CONTROL,   0x004011C0
    .set INIT_AUDIO_BUFFER,    0x00401200
    .set INIT_AUDIO_STATS,     0x00401400
    .set INIT_SHELL_PATH,      0x00401480
    .set INIT_NETMGR_PATH,     0x004014C0
    .set INIT_DEVICE_ID,        0x4C4954454F530001
    .set INIT_DEVICE_CLASS,     0x0600

    .set SERVICE_STACK_0,      0x00007FFFFFEF0000
    .set SERVICE_STACK_1,      0x00007FFFFFEE0000
    .set SERVICE_STACK_2,      0x00007FFFFFED0000
    .set SERVICE_STACK_3,      0x00007FFFFFEC0000
    .set SERVICE_STACK_4,      0x00007FFFFFEB0000
    .set SERVICE_STACK_5,      0x00007FFFFFEA0000
    .set SERVICE_STACK_6,      0x00007FFFFFE90000
    .set SERVICE_STACK_7,      0x00007FFFFFE80000

/*
 * 父进程使用展开后的直线代码，不在创建第一个子进程后再通过 CALL
 * 写父进程的 COW 栈。这样服务管理器本身不会把调度器的 COW 栈路径和
 * 服务启动路径耦合在一起；服务线程仍然使用正常的独立用户栈。
 */
    .macro INIT_RUN_SERVICE stage, stack, id, name
    movl $\stage, %edi
    movb %dil, INIT_STAGE
    movabsq $\stack, %rax
    movq %rax, INIT_THREAD_ARGS+16
    movl $\id, INIT_THREAD_ARGS+32

    movl $SYS_PROCESS_CREATE, %eax
    xorl %edi, %edi
    movq $INIT_PROCESS_HANDLE, %rsi
    movq $\name, %rdx
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@
    movq INIT_PROCESS_HANDLE, %r12
    testq %r12, %r12
    jz .Linit_service_fail\@

    movl $SYS_THREAD_CREATE, %eax
    movq %r12, %rdi
    movq $INIT_THREAD_ARGS, %rsi
    movq $INIT_THREAD_HANDLE, %rdx
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@
    movq INIT_THREAD_HANDLE, %r13
    testq %r13, %r13
    jz .Linit_service_fail\@

    /* 常驻模式下由 /init 持有服务生命周期，不等待子进程退出。 */
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_wait\@
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@
    incl INIT_SERVICE_OK
    .if \stage == 0x10
    jmp .Linit_service_done_0
    .elseif \stage == 0x20
    jmp .Linit_service_done_1
    .elseif \stage == 0x30
    jmp .Linit_service_done_2
    .elseif \stage == 0x40
    jmp .Linit_service_done_3
    .elseif \stage == 0x60
    jmp .Linit_service_done_5
    .elseif \stage == 0x70
    jmp .Linit_service_done_6
    .else
    jmp .Linit_service_done_6
    .endif

.Linit_service_wait\@:

    movl $INIT_WAIT_RETRIES, %r14d
.Linit_service_thread_wait_retry\@:
    movl $SYS_WAIT_ONE, %eax
    movq %r13, %rdi
    movq $INIT_WAIT_SLICE_NS, %rsi
    movq $INIT_WAIT_RESULT, %rdx
    syscall
    testq %rax, %rax
    jns .Linit_service_thread_wait_ok\@
    cmpq $K_ETIMEDOUT, %rax
    jne .Linit_service_fail\@
    decl %r14d
    jnz .Linit_service_thread_wait_retry\@
    jmp .Linit_service_fail\@
.Linit_service_thread_wait_ok\@:
    cmpq $0, INIT_WAIT_RESULT+8
    movq INIT_WAIT_RESULT+8, %rax
    jne .Linit_service_fail\@
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@

    movl $INIT_WAIT_RETRIES, %r14d
.Linit_service_process_wait_retry\@:
    movl $SYS_WAIT_ONE, %eax
    movq %r12, %rdi
    movq $INIT_WAIT_SLICE_NS, %rsi
    movq $INIT_WAIT_RESULT, %rdx
    syscall
    testq %rax, %rax
    jns .Linit_service_process_wait_ok\@
    cmpq $K_ETIMEDOUT, %rax
    jne .Linit_service_fail\@
    decl %r14d
    jnz .Linit_service_process_wait_retry\@
    jmp .Linit_service_fail\@
.Linit_service_process_wait_ok\@:
    cmpq $0, INIT_WAIT_RESULT+8
    movq INIT_WAIT_RESULT+8, %rax
    jne .Linit_service_fail\@
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail\@
    incl INIT_SERVICE_OK
    .if \stage == 0x10
    jmp .Linit_service_done_0
    .elseif \stage == 0x20
    jmp .Linit_service_done_1
    .elseif \stage == 0x30
    jmp .Linit_service_done_2
    .elseif \stage == 0x40
    jmp .Linit_service_done_3
    .elseif \stage == 0x70
    jmp .Linit_service_done_6
    .else
    jmp .Linit_service_done_6
    .endif

.Linit_service_fail\@:
    movq %rax, INIT_RESULT
    movl $SYS_HANDLE_CLOSE, %eax
    movq INIT_THREAD_HANDLE, %rdi
    testq %rdi, %rdi
    jz .Linit_service_skip_thread\@
    syscall
.Linit_service_skip_thread\@:
    movl $SYS_HANDLE_CLOSE, %eax
    movq INIT_PROCESS_HANDLE, %rdi
    testq %rdi, %rdi
    jz .Linit_service_skip_process\@
    syscall
.Linit_service_skip_process\@:
    movq INIT_RESULT, %rax
    jmp .Linit_failure
    .endm

user_init_blob_start:
    movb $1, INIT_STAGE
    cmpb $0, INIT_PERSISTENT
    je .Linit_bootstrap_services
    INIT_RUN_SERVICE 0x10, SERVICE_STACK_0, 1, INIT_DEVICED_PATH

.Linit_bootstrap_services:

    /* 第一个服务：deviced，负责未来的设备枚举和设备策略。 */
    INIT_RUN_SERVICE 0x10, SERVICE_STACK_0, 1, INIT_DEVICED_PATH
.Linit_service_done_0:

    /* 第二个服务：logd，负责日志汇聚和崩溃记录。 */
    INIT_RUN_SERVICE 0x20, SERVICE_STACK_1, 2, INIT_LOGD_PATH
.Linit_service_done_1:

    /* 第三个服务：crashd，负责故障转储和恢复策略。 */
    INIT_RUN_SERVICE 0x30, SERVICE_STACK_2, 3, INIT_CRASHD_PATH
.Linit_service_done_2:
    INIT_RUN_SERVICE 0x40, SERVICE_STACK_3, 4, INIT_AUDIO_PATH
.Linit_service_done_3:
    /* 第五个服务：用户态图形 Shell；窗口服务器已经属于内核。 */
#if OS_AUTOSTART_CHROME
    INIT_RUN_SERVICE 0x60, SERVICE_STACK_5, 6, INIT_SHELL_PATH
#else
    /*
     * 交互式桌面由 Ring0 launcher 提供。关闭 Chrome 自启动时不能再
     * 创建 chrome-start 进程，否则它仍会在进入桌面后占满缺页加载路径。
     * 保留阶段标签，让后续网络服务和 /init 的服务计数保持不变。
     */
    jmp .Linit_service_done_5
#endif
.Linit_service_done_4:
.Linit_service_done_5:
    /* 第六个服务：用户态网络管理器。 */
    INIT_RUN_SERVICE 0x70, SERVICE_STACK_6, 7, INIT_NETMGR_PATH
.Linit_service_done_6:

    movl $6, INIT_SERVICE_OK
    movb $0xA5, INIT_STAGE
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_exit_all

    /* 常驻 /init 在没有管理事件时睡眠，避免把服务监督器变成忙等线程。 */
.Linit_supervisor_wait:
    movl $0x0203, %eax
    movq $INIT_PERSISTENT_WORD, %rdi
    movl $1, %esi
    movq $-1, %rdx
    xorl %r10d, %r10d
    syscall
    jmp .Linit_supervisor_wait

.Linit_service_exit_all:
    xorl %edi, %edi
    movl $SYS_THREAD_EXIT, %eax
    syscall
    ud2

/* 服务启动失败时记录最后一个 UAPI 返回值并退出 /init。 */
.Linit_failure:
    movq %rax, INIT_RESULT
    /* 保留触发失败的服务阶段，便于 /init 启动自测报告具体故障点。 */
    movl $1, %edi
    movl $SYS_THREAD_EXIT, %eax
    syscall
    ud2

/* 所有服务线程共享这个只读入口，RDI 保留服务编号供后续服务扩展。 */
user_init_service:
    /* deviced 验证设备句柄、查询快照并关闭句柄。 */
    cmpq $1, %rdi
    je .Linit_service_device
    cmpq $3, %rdi
    je .Linit_service_crashd
    cmpq $4, %rdi
    je .Linit_service_audio_exec
    cmpq $6, %rdi
    je .Linit_service_shell_exec
    cmpq $7, %rdi
    je .Linit_service_netmgr_exec
    cmpq $8, %rdi
    je .Linit_service_chrome_exec
    jmp .Linit_service_exit

.Linit_service_device:
    /* 先枚举一个设备，验证 deviced 使用公开发现接口而非内核地址。 */
    movl $SYS_DEVICE_ENUMERATE, %eax
    movq $INIT_DEVICE_ENUMERATE, %rdi
    syscall
    testq %rax, %rax
    jns .Linit_device_enumerate_ok
    movl %eax, %edi
    jmp .Linit_service_device_status
.Linit_device_enumerate_ok:
    cmpq $32, INIT_DEVICE_ENUMERATE+32
    je .Linit_device_enumerate_done
    movl $0x7FFF, %edi
    jmp .Linit_service_device_status
.Linit_device_enumerate_done:

    movl $SYS_DEVICE_OPEN, %eax
    movq $INIT_DEVICE_OPEN, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    movq INIT_DEVICE_OPEN_HANDLE, %r12
    testq %r12, %r12
    jz .Linit_service_fail_direct

    movl $SYS_DEVICE_CONTROL, %eax
    movq %r12, %rdi
    movq $INIT_DEVICE_CONTROL, %rsi
    syscall
    testq %rax, %rax
    js .Linit_service_fail_with_handle

    movl $SYS_HANDLE_CLOSE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    cmpb $0, INIT_PERSISTENT
    jne .Linit_service_block
    jmp .Linit_service_exit

.Linit_service_device_status:
    movl $SYS_THREAD_EXIT, %eax
    syscall
    ud2

.Linit_service_crashd:
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_exit
    jmp .Linit_service_block

.Linit_service_audio_exec:
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_audio_exec_now
    jmp .Linit_service_block

.Linit_service_audio_exec_now:
    /* audiod 是独立 ELF 用户服务，由 exec 事务切换到自己的地址空间。 */
    movl $SYS_PROCESS_EXEC, %eax
    movq $INIT_AUDIO_PATH, %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    ud2

.Linit_service_shell_exec:
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_exit
#if OS_AUTOSTART_CHROME
    /* Chrome 复用 Shell 服务槽位，保持 /init 的六服务计数不变。 */
    movl $SYS_PROCESS_EXEC, %eax
    movq $INIT_CHROME_PATH, %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    ud2
#else
    /*
     * Ring0 桌面负责交互式应用启动。保持此服务槽位处于阻塞状态，
     * 这样 init 的六项服务计数不会改变。
     */
    jmp .Linit_service_block
#endif

.Linit_service_netmgr_exec:
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_exit
    movl $SYS_PROCESS_EXEC, %eax
    movq $INIT_NETMGR_PATH, %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    ud2

.Linit_service_chrome_exec:
    cmpb $0, INIT_PERSISTENT
    je .Linit_service_exit
    movl $SYS_PROCESS_EXEC, %eax
    movq $INIT_CHROME_PATH, %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Linit_service_fail_direct
    ud2

.Linit_service_block:
    /* 常驻服务没有工作时阻塞在共享状态字上，避免忙等占用 CPU。 */
.Linit_service_block_loop:
    movl $0x0203, %eax
    movq $INIT_PERSISTENT_WORD, %rdi
    movl $1, %esi
    movq $-1, %rdx
    xorl %r10d, %r10d
    syscall
    jmp .Linit_service_block_loop

.Linit_service_fail_with_handle:
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r12, %rdi
    syscall
.Linit_service_fail_direct:
    movl $1, %edi
    movl $SYS_THREAD_EXIT, %eax
    syscall
    ud2

.Linit_service_exit:
    cmpb $0, INIT_PERSISTENT
    jne .Linit_service_block
    xorl %edi, %edi
    movl $SYS_THREAD_EXIT, %eax
    syscall
    ud2

user_init_blob_end:
