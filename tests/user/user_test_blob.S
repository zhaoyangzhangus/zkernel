#ifdef __ELF__
    .section .rdata,"a",@progbits
#else
    .section .rdata,"dr"
#endif
    .p2align 4
    .globl user_runtime_blob_start
    .globl user_runtime_blob_end
    .globl user_runtime_child
    .globl user_runtime_timer_helper
    .globl user_runtime_vm_worker
    .globl user_runtime_cow_worker
    .globl user_runtime_uaccess_worker
    .globl user_runtime_wait_race_worker

/*
 * 这段代码不会在内核地址直接执行，而是作为无重定位的测试载荷复制进 ELF
 * 的用户代码段。它验证匿名映射、UAccess 懒缺页、COW 子进程句柄、mprotect、
 * munmap，以及最后的线程退出调度。
 */
    .set SYS_THREAD_EXIT,    0x0000
    .set SYS_THREAD_CREATE,  0x0002
    .set SYS_PROCESS_CREATE, 0x0003
    .set SYS_PROCESS_EXEC,   0x0004
    .set SYS_VM_MAP,         0x0100
    .set SYS_VM_UNMAP,       0x0101
    .set SYS_VM_PROTECT,     0x0102
    .set SYS_VM_SHARE,       0x0103
    .set SYS_HANDLE_CLOSE,   0x0200
    .set SYS_WAIT_ONE,       0x0201
    .set SYS_WAIT_MANY,      0x0202
    .set SYS_FUTEX_WAIT,     0x0203
    .set SYS_FUTEX_WAKE,     0x0204
    .set FUTEX_FLAG_SHARED,  1
    .set SYS_PORT_CREATE,     0x0400
    .set SYS_PORT_SEND,       0x0401
    .set SYS_FILE_OPEN,      0x0300
    .set SYS_FILE_READ,      0x0301
    .set SYS_FILE_WRITE,     0x0302
    .set SYS_FILE_FSYNC,     0x0303
    .set SYS_SOCKET_CREATE,  0x0500
    .set SYS_SOCKET_BIND,    0x0501
    .set SYS_SOCKET_CONNECT, 0x0502
    .set SYS_SOCKET_LISTEN,  0x0503
    .set SYS_SOCKET_ACCEPT,  0x0504
    .set SYS_SOCKET_SEND,    0x0505
    .set SYS_SOCKET_RECV,    0x0506
    .set SYS_GPU_CREATE_CTX, 0x0700
    .set SYS_GPU_ALLOC,      0x0701
    .set SYS_GPU_MAP,        0x0702
    .set SYS_GPU_SUBMIT,     0x0703
    .set SYS_GPU_WAIT_FENCE, 0x0704
    .set SYS_DISPLAY_COMMIT,  0x0710
    .set SYS_DEBUG_QUERY,    0x0920
    .set VM_ARGS_ADDRESS,    0x00402000
    .set STACK_ARGS_ADDRESS, 0x00402030
    .set THREAD_ARGS_ADDRESS,0x00402060
    .set TIMER_ARGS_ADDRESS, 0x004020C0
    .set VM_WORKER_ENTRY_ADDRESS, 0x004020F0
    .set UACCESS_WORKER_ENTRY_ADDRESS,0x004021F8
    .set FUTEX_WORD_ADDRESS, 0x004020A0
    .set CHILD_MARK_ADDRESS, 0x004020A4
    .set STAGE_MARK_ADDRESS, 0x004020A8
    .set TIMER_STOP_ADDRESS, 0x004020AC
    .set RESULT_MARK_ADDRESS,0x004020B0
    .set COW_PASS_MARK_ADDRESS,0x004020BC
    .set VM_CONCURRENT_PASS_ADDRESS,0x004021F0
    .set UACCESS_START_ADDRESS,0x004021F1
    .set UACCESS_PASS_ADDRESS,0x004021F2
    .set WAIT_RACE_PASS_ADDRESS,0x004021F3
    .set WAIT_RACE_START_ADDRESS,0x004021F4
    .set WAIT_RACE_GO_ADDRESS,0x004021F5
    .set WAIT_RACE_ACK_ADDRESS,0x004021F6
    .set UACCESS_ADDRESS,0x0000000069000000
    .set UACCESS_PORT_HANDLE_ADDRESS,0x004023D0
    .set UACCESS_THREAD_HANDLE_ADDRESS,0x004023D8
    .set UACCESS_THREAD_ARGS_ADDRESS,0x00402440
    .set WAIT_RACE_THREAD_HANDLE_ADDRESS,0x004023C0
    .set WAIT_RACE_THREAD_ARGS_ADDRESS,0x00402480
    .set WAIT_RACE_ACK_WORD_ADDRESS,0x004023B0
    /* 限制竞争测试的执行时间，同时避免仅因远端 vCPU 需要多个宿主时间片就导致 TCG 跨 CPU
     * 唤醒测试失败。 */
    .set WAIT_RACE_TIMEOUT_NS, 10000000
    .set WAIT_RACE_ACK_TIMEOUT_NS, 100000000
    /* 为多 vCPU 模拟器上的串口调试输出留出余量。 */
    .set THREAD_WAIT_TIMEOUT_NS, 5000000000
    .set EXEC_PIN_ACK_ADDRESS, 0x004021F7
    .set EXEC_PIN_EXEC_ACK_ADDRESS, 0x004021F8
    .set COW_SYNC_ADDRESS,0x0000000067000000
    .set COW_PRIVATE_ADDRESS,0x0000000068000000
    .set COW_PROGRESS_ADDRESS,0x0000000067000004
    .set COW_PROCESS_HANDLE_ADDRESS,0x004023E0
    .set COW_THREAD_HANDLE_ADDRESS,0x004023E8
    .set COW_SHARED_HANDLE_ADDRESS,0x004023F0
    .set COW_THREAD_ARGS_ADDRESS,0x00402400
    .set FILE_PATH_ADDRESS,  0x00402100
    .set FAT32_PATH_ADDRESS,  0x00402180
    .set FILE_BUFFER_ADDRESS,0x00402140
    .set FILE_HANDLE_ADDRESS,0x00402130
    .set FILE_BYTES_ADDRESS, 0x00402138
    .set SOCKET_A_HANDLE_ADDRESS, 0x004021A0
    .set SOCKET_B_HANDLE_ADDRESS, 0x004021A8
    .set SOCKET_BUFFER_ADDRESS,   0x004021C0
    .set SOCKET_SOURCE_ADDRESS,   0x004021D0
    .set SOCKET_SOURCE_PORT,      0x004021D4
    .set GPU_CTX_ARGS_ADDRESS,    0x00402200
    .set GPU_ALLOC_ARGS_ADDRESS,  0x00402220
    .set GPU_SUBMIT_ARGS_ADDRESS, 0x00402240
    .set GPU_CTX_HANDLE_ADDRESS,  0x00402280
    .set GPU_ALLOC_HANDLE_ADDRESS,0x00402288
    .set GPU_MAP_ARGS_ADDRESS,    0x004022A0
    .set DISPLAY_ARGS_ADDRESS,    0x00402300

user_runtime_blob_start:
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movq $VM_ARGS_ADDRESS, %r12

    /* 阶段码由内核在失败后读取，避免多核/时序问题只留下一个笼统的 FAIL。 */
    movb $1, STAGE_MARK_ADDRESS
    /* 合法范围内但未映射的指针必须返回 K_EACCES，不能让内核崩溃。 */
    movl $SYS_VM_MAP, %eax
    movabsq $0x0000700000000000, %rdi
    syscall
    cmpq $-13, %rax
    jne .Lruntime_failure

    movb $2, STAGE_MARK_ADDRESS
    /* VM_MAP(&args)，最终地址由内核写回 args.address。 */
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 8(%r12), %rbx
    testq %rbx, %rbx
    jz .Lruntime_failure

    movl $SYS_VM_MAP, %eax
    movq $STACK_ARGS_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq STACK_ARGS_ADDRESS+8, %r13
    testq %r13, %r13
    jz .Lruntime_failure
    leaq 4096(%r13), %rax
    movq %rax, THREAD_ARGS_ADDRESS+16
    movq %rax, TIMER_ARGS_ADDRESS+16

    movl $48, 0(%r12)
    movw $1, 4(%r12)
    movq $4096, 8(%r12)
    movl $0, 16(%r12)
    movl $0, 20(%r12)
    movl $SYS_VM_SHARE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 24(%r12), %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movq %r14, COW_SHARED_HANDLE_ADDRESS
    movl $48, 0(%r12)
    movw $1, 4(%r12)
    movq $COW_SYNC_ADDRESS, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq %r14, 32(%r12)
    movl $3, 40(%r12)
    movl $6, 44(%r12)
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $COW_SYNC_ADDRESS, 8(%r12)
    jne .Lruntime_failure
    movl $48, 0(%r12)
    movw $1, 4(%r12)
    movq $COW_PRIVATE_ADDRESS, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq $0, 32(%r12)
    movl $3, 40(%r12)
    movl $5, 44(%r12)
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $COW_PRIVATE_ADDRESS, 8(%r12)
    jne .Lruntime_failure
    movb $0x11, COW_PRIVATE_ADDRESS
    movb $0x11, COW_SYNC_ADDRESS
    movb $0, COW_PROGRESS_ADDRESS
    mfence

    movb $3, STAGE_MARK_ADDRESS
    /* 输出句柄位于尚未触页的匿名映射，copy_to_user 必须解决合法缺页。 */
    movl $SYS_PROCESS_CREATE, %eax
    xorl %edi, %edi
    movq %rbx, %rsi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq (%rbx), %r15
    testq %r15, %r15
    jz .Lruntime_failure
    movq %r15, COW_PROCESS_HANDLE_ADDRESS
    movb $4, STAGE_MARK_ADDRESS
    /* 为子线程建立独立用户栈，并通过 THREAD_CREATE 发布线程。 */
    /*
     * 辅助线程保持 Ring3 可抢占运行；父线程等待永不触发的空进程对象，
     * 必须由 LAPIC 定时中断扫描超时队列并返回 K_ETIMEDOUT。
     */
    movb $5, STAGE_MARK_ADDRESS
    movl $0, TIMER_STOP_ADDRESS
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $TIMER_ARGS_ADDRESS, %rsi
    movq %rbx, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq (%rbx), %r14
    testq %r14, %r14
    jz .Lruntime_failure

    movb $6, STAGE_MARK_ADDRESS
    movl $SYS_WAIT_ONE, %eax
    movq %r15, %rdi
    movl $1000000, %esi
    leaq 16(%rbx), %rdx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    cmpq $-110, %rax
    jne .Lruntime_failure
    movl $1, TIMER_STOP_ADDRESS
    movb $7, STAGE_MARK_ADDRESS
    movl $SYS_WAIT_ONE, %eax
    movq %r14, %rdi
    movl $1000000000, %esi
    movq $FILE_HANDLE_ADDRESS+16, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $0, FILE_HANDLE_ADDRESS+24
    jne .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $0x81, STAGE_MARK_ADDRESS
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $THREAD_ARGS_ADDRESS, %rsi
    movq %rbx, %rdx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure

    movb $0x82, STAGE_MARK_ADDRESS
    movq (%rbx), %r14
    testq %r14, %r14
    jz .Lruntime_failure

    /* 第一个子线程退出时应唤醒对象等待，并返回退出码 0。 */
    movb $0x83, STAGE_MARK_ADDRESS
    movl $SYS_WAIT_ONE, %eax
    movq %r14, %rdi
    movq $THREAD_WAIT_TIMEOUT_NS, %rsi
    leaq 16(%rbx), %rdx
    xorl %r10d, %r10d
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure

    movb $0x84, STAGE_MARK_ADDRESS
    cmpl $0, 16(%rbx)
    jne .Lruntime_failure

    movb $0x85, STAGE_MARK_ADDRESS
    movq 24(%rbx), %rax
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    jne .Lruntime_failure

    movb $0x86, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure

    /* 第二个线程继续覆盖 Futex 的真实阻塞与唤醒路径。 */
    movb $9, STAGE_MARK_ADDRESS
    movl $0, FUTEX_WORD_ADDRESS
    movb $0, CHILD_MARK_ADDRESS
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $THREAD_ARGS_ADDRESS, %rsi
    movq %rbx, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq (%rbx), %r14
    testq %r14, %r14
    jz .Lruntime_failure

    /* 父线程等待，子线程写值并唤醒；提前唤醒时允许标准 EAGAIN 结果。 */
    movl $SYS_FUTEX_WAIT, %eax
    movq $FUTEX_WORD_ADDRESS, %rdi
    xorl %esi, %esi
    movq $5000000000, %rdx
    xorl %r10d, %r10d
    syscall
    /* 诊断该次等待的真实返回值；成功路径随后会被最终状态覆盖。 */
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    jz .Lfutex_returned
    cmpq $-11, %rax
    jne .Lruntime_failure
.Lfutex_returned:
    cmpl $1, FUTEX_WORD_ADDRESS
    jne .Lruntime_failure
    cmpb $0xA5, CHILD_MARK_ADDRESS
    jne .Lruntime_failure

    /* Futex 唤醒与线程发布 DEAD 可并发；WAIT_MANY(ANY) 统一收敛该竞态。 */
    movb $10, STAGE_MARK_ADDRESS
    movq %r14, 48(%rbx)           /* os_handle_t 句柄数组[1] */
    movl $48, 64(%rbx)            /* hdr.size：结构体大小 */
    movw $1, 68(%rbx)             /* hdr.version：版本 */
    movw $0, 70(%rbx)             /* hdr.flags：标志 */
    movl $1, 72(%rbx)             /* count：数量 */
    movl $0, 76(%rbx)             /* wait_flags = ANY：等待任意对象 */
    leaq 48(%rbx), %rax
    movq %rax, 80(%rbx)           /* handles：句柄数组 */
    movq $1000000000, 88(%rbx)    /* timeout_ns：超时纳秒数 */
    movl $0, 96(%rbx)
    movl $0, 100(%rbx)
    movq $0, 104(%rbx)
    movl $SYS_WAIT_MANY, %eax
    leaq 64(%rbx), %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpl $0, 96(%rbx)
    jne .Lruntime_failure
    cmpq $0, 104(%rbx)
    jne .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $0xA1, STAGE_MARK_ADDRESS
    movb $0x5A, 8(%rbx)
    cmpb $0x5A, 8(%rbx)
    jne .Lruntime_failure

    movl $SYS_VM_PROTECT, %eax
    movq %rbx, %rdi
    movl $4096, %esi
    movl $1, %edx                 /* OS_VM_READ：可读 */
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movl $SYS_VM_UNMAP, %eax
    movq %rbx, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* 主线程与 VM worker 并发操作同一地址空间的另一段固定映射。 */
    movb $0xA2, STAGE_MARK_ADDRESS
    /* A2 进度标记：高字节为 CHILD_MARK；低字节仍是 0xA5 ABI 标记。 */
    movb $0, CHILD_MARK_ADDRESS+1
    movb $0, CHILD_MARK_ADDRESS+2
    movb $0, CHILD_MARK_ADDRESS+3
    movq THREAD_ARGS_ADDRESS+8, %rax
    movq %rax, VM_WORKER_ENTRY_ADDRESS+8
    movq VM_WORKER_ENTRY_ADDRESS, %rax
    movq %rax, THREAD_ARGS_ADDRESS+8
    movq $1, THREAD_ARGS_ADDRESS+32
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $THREAD_ARGS_ADDRESS, %rsi
    movq $FILE_HANDLE_ADDRESS, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq FILE_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movl $32, %r15d
.Lmain_vm_loop:
    movq $0x62000000, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq $0, 32(%r12)
    movl $3, 40(%r12)
    movl $5, 44(%r12)
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 8(%r12), %r13
    movb $0xA5, (%r13)
    movl $SYS_VM_PROTECT, %eax
    movq %r13, %rdi
    movl $4096, %esi
    movl $1, %edx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_VM_PROTECT, %eax
    movq %r13, %rdi
    movl $4096, %esi
    movl $3, %edx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_VM_UNMAP, %eax
    movq %r13, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    incb CHILD_MARK_ADDRESS+1
    decl %r15d
    jnz .Lmain_vm_loop
    movl $SYS_WAIT_ONE, %eax
    movq %r14, %rdi
    movl $1000000000, %esi
    movq $FILE_HANDLE_ADDRESS+16, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $0, FILE_HANDLE_ADDRESS+24
    jne .Lruntime_failure
    cmpb $0xD2, VM_CONCURRENT_PASS_ADDRESS
    jne .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* 并发 usercopy/munmap：copy_from_user 必须只返回成功或受控错误。 */
    movb $0xA3, STAGE_MARK_ADDRESS
    movb $0, UACCESS_START_ADDRESS
    movb $0, UACCESS_PASS_ADDRESS
    movl $SYS_PORT_CREATE, %eax
    movl $1, %edi
    movl $32, %esi
    movq $UACCESS_PORT_HANDLE_ADDRESS, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq UACCESS_PORT_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movl $48, UACCESS_THREAD_ARGS_ADDRESS
    movw $1, UACCESS_THREAD_ARGS_ADDRESS+4
    movw $0, UACCESS_THREAD_ARGS_ADDRESS+6
    movq $0x00400000, %rax
    addq $user_runtime_uaccess_worker-user_runtime_blob_start, %rax
    movq %rax, UACCESS_THREAD_ARGS_ADDRESS+8
    movq THREAD_ARGS_ADDRESS+16, %rax
    movq %rax, UACCESS_THREAD_ARGS_ADDRESS+16
    movq $0, UACCESS_THREAD_ARGS_ADDRESS+24
    movq $0, UACCESS_THREAD_ARGS_ADDRESS+32
    movl $0, UACCESS_THREAD_ARGS_ADDRESS+40
    movl $0, UACCESS_THREAD_ARGS_ADDRESS+44
    movb $0xA4, STAGE_MARK_ADDRESS
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $UACCESS_THREAD_ARGS_ADDRESS, %rsi
    movq $UACCESS_THREAD_HANDLE_ADDRESS, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq UACCESS_THREAD_HANDLE_ADDRESS, %r15
    testq %r15, %r15
    jz .Lruntime_failure
    movb $1, UACCESS_START_ADDRESS
    movb $0xA5, STAGE_MARK_ADDRESS
    movl $128, %r13d
.Luaccess_send_loop:
    movl $SYS_PORT_SEND, %eax
    movq UACCESS_PORT_HANDLE_ADDRESS, %rdi
    movq $UACCESS_ADDRESS, %rsi
    movl $64, %edx
    syscall
    cmpq $0, %rax
    je .Luaccess_send_ok
    cmpq $-13, %rax
    je .Luaccess_send_ok
    cmpq $-11, %rax
    jne .Lruntime_failure
.Luaccess_send_ok:
    decl %r13d
    jnz .Luaccess_send_loop
    movb $0xA6, STAGE_MARK_ADDRESS
    movl $SYS_WAIT_ONE, %eax
    movq %r15, %rdi
    movl $1000000000, %esi
    movq $FILE_HANDLE_ADDRESS+16, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $0, FILE_HANDLE_ADDRESS+24
    jne .Lruntime_failure
    cmpb $0xA7, UACCESS_PASS_ADDRESS
    jne .Lruntime_failure
    movb $0xA7, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r15, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movb $0xA8, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq UACCESS_PORT_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* 等待/超时竞争：worker 与主线程重复制造 futex 唤醒和短超时竞态。 */
    movb $0xA9, STAGE_MARK_ADDRESS
    movb $0, WAIT_RACE_PASS_ADDRESS
    movb $0, WAIT_RACE_START_ADDRESS
    movb $0, WAIT_RACE_GO_ADDRESS
    movb $0, WAIT_RACE_ACK_ADDRESS
    movl $0, WAIT_RACE_ACK_WORD_ADDRESS
    movl $48, WAIT_RACE_THREAD_ARGS_ADDRESS
    movw $1, WAIT_RACE_THREAD_ARGS_ADDRESS+4
    movw $0, WAIT_RACE_THREAD_ARGS_ADDRESS+6
    movq $0x00400000, %rax
    addq $user_runtime_wait_race_worker-user_runtime_blob_start, %rax
    movq %rax, WAIT_RACE_THREAD_ARGS_ADDRESS+8
    movq THREAD_ARGS_ADDRESS+16, %rax
    movq %rax, WAIT_RACE_THREAD_ARGS_ADDRESS+16
    movq $0, WAIT_RACE_THREAD_ARGS_ADDRESS+24
    movq $0, WAIT_RACE_THREAD_ARGS_ADDRESS+32
    movl $0, WAIT_RACE_THREAD_ARGS_ADDRESS+40
    movl $0, WAIT_RACE_THREAD_ARGS_ADDRESS+44
    movb $0xAA, STAGE_MARK_ADDRESS
    movl $SYS_THREAD_CREATE, %eax
    xorl %edi, %edi
    movq $WAIT_RACE_THREAD_ARGS_ADDRESS, %rsi
    movq $WAIT_RACE_THREAD_HANDLE_ADDRESS, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq WAIT_RACE_THREAD_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movb $1, WAIT_RACE_START_ADDRESS
    movb $0xAB, STAGE_MARK_ADDRESS
    movl $1, %r13d
.Lwait_race_loop:
    movl $0, FUTEX_WORD_ADDRESS
    movb %r13b, WAIT_RACE_GO_ADDRESS
    movl $SYS_FUTEX_WAIT, %eax
    movq $FUTEX_WORD_ADDRESS, %rdi
    xorl %esi, %esi
    movl $WAIT_RACE_TIMEOUT_NS, %edx
    xorl %r10d, %r10d
    syscall
    cmpq $0, %rax
    je .Lwait_race_result_ok
    cmpq $-11, %rax
    je .Lwait_race_result_ok
    cmpq $-110, %rax
    jne .Lruntime_failure
.Lwait_race_result_ok:
    /* 等待工作线程发布序号，而不是在远端 vCPU 已可运行时消耗依赖宿主速度的自旋预算。 */
    movl %r13d, %esi
    decl %esi
    movl $SYS_FUTEX_WAIT, %eax
    movq $WAIT_RACE_ACK_WORD_ADDRESS, %rdi
    movl $WAIT_RACE_ACK_TIMEOUT_NS, %edx
    xorl %r10d, %r10d
    syscall
    cmpq $0, %rax
    je .Lwait_race_ack_returned
    cmpq $-11, %rax
    jne .Lruntime_failure
.Lwait_race_ack_returned:
    cmpl %r13d, WAIT_RACE_ACK_WORD_ADDRESS
    jne .Lruntime_failure
    incl %r13d
    cmpl $65, %r13d
    jne .Lwait_race_loop
    movb $0xAC, STAGE_MARK_ADDRESS
    movl $SYS_WAIT_ONE, %eax
    movq %r14, %rdi
    movl $1000000000, %esi
    movq $FILE_HANDLE_ADDRESS+16, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $0, FILE_HANDLE_ADDRESS+24
    jne .Lruntime_failure
    cmpb $0xB7, WAIT_RACE_PASS_ADDRESS
    jne .Lruntime_failure
    movb $0xAD, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movq VM_WORKER_ENTRY_ADDRESS+8, %rax
    movq %rax, THREAD_ARGS_ADDRESS+8

    movb $16, STAGE_MARK_ADDRESS
    movl $48, COW_THREAD_ARGS_ADDRESS
    movw $1, COW_THREAD_ARGS_ADDRESS+4
    movw $0, COW_THREAD_ARGS_ADDRESS+6
    movq $0x00400000, %rax
    addq $user_runtime_cow_worker-user_runtime_blob_start, %rax
    movq %rax, COW_THREAD_ARGS_ADDRESS+8
    movq THREAD_ARGS_ADDRESS+16, %rax
    movq %rax, COW_THREAD_ARGS_ADDRESS+16
    movq $0, COW_THREAD_ARGS_ADDRESS+24
    movq $0, COW_THREAD_ARGS_ADDRESS+32
    movl $0, COW_THREAD_ARGS_ADDRESS+40
    movl $0, COW_THREAD_ARGS_ADDRESS+44
    movl $SYS_THREAD_CREATE, %eax
    movq COW_PROCESS_HANDLE_ADDRESS, %rdi
    movq $COW_THREAD_ARGS_ADDRESS, %rsi
    movq $COW_THREAD_HANDLE_ADDRESS, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq COW_THREAD_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    /* 在共享 COW 页面上等待，避免调度延迟导致父进程在子进程获得首个时间片前放弃握手。 */
    movl $SYS_FUTEX_WAIT, %eax
    movq $COW_SYNC_ADDRESS, %rdi
    movl $0x11, %esi
    movq $1000000000, %rdx
    movl $FUTEX_FLAG_SHARED, %r10d
    syscall
    testq %rax, %rax
    jz .Lcow_parent_ready
    cmpq $-11, %rax
    je .Lcow_parent_ready
    jmp .Lruntime_failure
.Lcow_parent_ready:
    cmpb $0x11, COW_PRIVATE_ADDRESS
    jne .Lruntime_failure
    movb $0x5A, COW_PRIVATE_ADDRESS
    movb $0x33, COW_SYNC_ADDRESS
    mfence
    movl $SYS_WAIT_ONE, %eax
    movq %r14, %rdi
    movl $1000000000, %esi
    movq $FILE_HANDLE_ADDRESS+16, %rdx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $0, FILE_HANDLE_ADDRESS+24
    jne .Lruntime_failure
    cmpb $0x5A, COW_PRIVATE_ADDRESS
    jne .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq COW_PROCESS_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_VM_UNMAP, %eax
    movq $COW_PRIVATE_ADDRESS, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_VM_UNMAP, %eax
    movq $COW_SYNC_ADDRESS, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq COW_SHARED_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movb $0xC0, COW_PASS_MARK_ADDRESS

    movb $12, STAGE_MARK_ADDRESS
    /* 磁盘文件验证在根文件系统挂载后的 VFS 自测中完成；这里保留
     * 进程/线程/虚拟内存/网络并发测试，避免引导前访问未挂载的根盘。 */
    jmp .Lruntime_skip_disk_files
    movl $SYS_FILE_OPEN, %eax
    movq $FILE_PATH_ADDRESS, %rdi
    movl $1, %esi                 /* VFS_OPEN_READ：只读打开 */
    xorl %edx, %edx               /* mode：模式 */
    movq $FILE_HANDLE_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq FILE_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure

    movl $SYS_FILE_READ, %eax
    movq %r14, %rdi
    movq $FILE_BUFFER_ADDRESS, %rsi
    movl $5, %edx
    movq $FILE_BYTES_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $5, FILE_BYTES_ADDRESS
    jne .Lruntime_failure
    cmpb $0x7F, FILE_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'E', FILE_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'L', FILE_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'F', FILE_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    cmpb $1, FILE_BUFFER_ADDRESS+4
    jne .Lruntime_failure

    /* VM_MAP 文件句柄：缺页时应复用 vnode 的页缓存。 */
    movq $0x64000000, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq %r14, 32(%r12)
    movl $1, 40(%r12)             /* OS_VM_READ：可读 */
    movl $5, 44(%r12)             /* OS_VM_PRIVATE | OS_VM_FIXED：私有且固定映射 */
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 8(%r12), %r13
    cmpb $0x7F, (%r13)
    jne .Lruntime_failure
    movl $SYS_VM_UNMAP, %eax
    movq %r13, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* 私有可写文件映射：写入 shadow 后，重新读取磁盘 ELF 仍应不变。 */
    movq $0x65000000, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq %r14, 32(%r12)
    movl $3, 40(%r12)             /* OS_VM_READ | OS_VM_WRITE：可读写 */
    movl $5, 44(%r12)             /* OS_VM_PRIVATE | OS_VM_FIXED：私有且固定映射 */
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 8(%r12), %r13
    cmpb $0x7F, (%r13)
    jne .Lruntime_failure
    movb $'X', (%r13)
    movl $SYS_VM_UNMAP, %eax
    movq %r13, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_FILE_OPEN, %eax
    movq $FILE_PATH_ADDRESS, %rdi
    movl $1, %esi
    xorl %edx, %edx
    movq $FILE_HANDLE_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq FILE_HANDLE_ADDRESS, %r14
    movl $SYS_FILE_READ, %eax
    movq %r14, %rdi
    movq $FILE_BUFFER_ADDRESS, %rsi
    movl $5, %edx
    movq $FILE_BYTES_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $5, FILE_BYTES_ADDRESS
    jne .Lruntime_failure
    cmpb $0x7F, FILE_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'E', FILE_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'L', FILE_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'F', FILE_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    cmpb $1, FILE_BUFFER_ADDRESS+4
    jne .Lruntime_failure

    movl $SYS_FILE_FSYNC, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $13, STAGE_MARK_ADDRESS
    movl $SYS_FILE_OPEN, %eax
    movq $FAT32_PATH_ADDRESS, %rdi
    movl $1, %esi                 /* VFS_OPEN_READ：只读打开 */
    xorl %edx, %edx
    movq $FILE_HANDLE_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq FILE_HANDLE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure

    movl $SYS_FILE_READ, %eax
    movq %r14, %rdi
    movq $FILE_BUFFER_ADDRESS, %rsi
    movl $5, %edx
    movq $FILE_BYTES_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    cmpq $5, FILE_BYTES_ADDRESS
    jne .Lruntime_failure
    cmpb $0x7F, FILE_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'E', FILE_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'L', FILE_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'F', FILE_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    cmpb $1, FILE_BUFFER_ADDRESS+4
    jne .Lruntime_failure

    movl $SYS_FILE_FSYNC, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    .Lruntime_skip_disk_files:
    /* UDP socket ABI：创建、绑定、connect、loopback send/recv 和关闭句柄。 */
    /* VM_SHARE：创建匿名共享段，使用共享句柄映射并验证写入路径。 */
    movb $14, STAGE_MARK_ADDRESS
    movl $32, 0(%r12)
    movw $1, 4(%r12)
    movq $4096, 8(%r12)
    movl $0, 16(%r12)
    movl $0, 20(%r12)
    movl $SYS_VM_SHARE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 24(%r12), %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movl $48, 0(%r12)
    movq $0x66000000, 8(%r12)
    movq $4096, 16(%r12)
    movq $0, 24(%r12)
    movq %r14, 32(%r12)
    movl $3, 40(%r12)
    movl $6, 44(%r12)
    movl $SYS_VM_MAP, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq 8(%r12), %r13
    movb $0x5A, (%r13)
    movl $SYS_VM_UNMAP, %eax
    movq %r13, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* 开始 UDP 回环测试。 */
    movb $0x40, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_CREATE, %eax
    movl $2, %edi
    movl $2, %esi
    xorl %edx, %edx
    movq $SOCKET_A_HANDLE_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_SOCKET_CREATE, %eax
    movl $2, %edi
    movl $2, %esi
    xorl %edx, %edx
    movq $SOCKET_B_HANDLE_ADDRESS, %r10
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movl $SYS_SOCKET_BIND, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $12000, %edx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_SOCKET_BIND, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $12001, %edx
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_SOCKET_CONNECT, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $12001, %edx
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $'n', SOCKET_BUFFER_ADDRESS
    movb $'e', SOCKET_BUFFER_ADDRESS+1
    movb $'t', SOCKET_BUFFER_ADDRESS+2
    movb $'!', SOCKET_BUFFER_ADDRESS+3
    movl $SYS_SOCKET_SEND, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    movl $SYS_SOCKET_RECV, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    movq $SOCKET_SOURCE_ADDRESS, %r10
    movq $SOCKET_SOURCE_PORT, %r8
    movq $1000000000, %r9
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    cmpb $'n', SOCKET_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'e', SOCKET_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'t', SOCKET_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'!', SOCKET_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    cmpl $0x7F000001, SOCKET_SOURCE_ADDRESS
    jne .Lruntime_failure
    cmpw $12000, SOCKET_SOURCE_PORT
    jne .Lruntime_failure
    /* UDP 回环接收完成；下一段测试 TCP。 */
    movb $0x4F, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* TCP stream ABI：监听、连接建立、accept、双向字节流和句柄关闭。 */
    /* 开始设置 TCP 监听端和客户端。 */
    movb $0x50, STAGE_MARK_ADDRESS
    movq $0, RESULT_MARK_ADDRESS
    movl $SYS_SOCKET_CREATE, %eax
    movl $2, %edi
    movl $1, %esi
    xorl %edx, %edx
    xorl %r8d, %r8d
    movq $SOCKET_A_HANDLE_ADDRESS, %r10
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x51, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_CREATE, %eax
    movl $2, %edi
    movl $1, %esi
    xorl %edx, %edx
    xorl %r8d, %r8d
    movq $SOCKET_B_HANDLE_ADDRESS, %r10
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x52, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_BIND, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $13000, %edx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x53, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_LISTEN, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movl $4, %esi
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x54, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_BIND, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $13001, %edx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x55, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_CONNECT, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movl $0x7F000001, %esi
    movl $13000, %edx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x56, STAGE_MARK_ADDRESS
    movl $SYS_SOCKET_ACCEPT, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    movq $1000000000, %rsi
    movq $SOCKET_SOURCE_ADDRESS, %rdx
    xorl %r10d, %r10d
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movq SOCKET_SOURCE_ADDRESS, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movb $'t', SOCKET_BUFFER_ADDRESS
    movb $'c', SOCKET_BUFFER_ADDRESS+1
    movb $'p', SOCKET_BUFFER_ADDRESS+2
    movb $'!', SOCKET_BUFFER_ADDRESS+3
    movl $SYS_SOCKET_SEND, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    movl $SYS_SOCKET_RECV, %eax
    movq %r14, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    movq $SOCKET_SOURCE_ADDRESS, %r10
    movq $SOCKET_SOURCE_PORT, %r8
    movq $1000000000, %r9
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    cmpb $'t', SOCKET_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'c', SOCKET_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'p', SOCKET_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'!', SOCKET_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    cmpl $0x7F000001, SOCKET_SOURCE_ADDRESS
    jne .Lruntime_failure
    cmpw $13001, SOCKET_SOURCE_PORT
    jne .Lruntime_failure
    movb $'a', SOCKET_BUFFER_ADDRESS
    movb $'c', SOCKET_BUFFER_ADDRESS+1
    movb $'k', SOCKET_BUFFER_ADDRESS+2
    movb $'!', SOCKET_BUFFER_ADDRESS+3
    movl $SYS_SOCKET_SEND, %eax
    movq %r14, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    movl $SYS_SOCKET_RECV, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    movq $SOCKET_BUFFER_ADDRESS, %rsi
    movl $4, %edx
    movq $SOCKET_SOURCE_ADDRESS, %r10
    movq $SOCKET_SOURCE_PORT, %r8
    movq $1000000000, %r9
    syscall
    cmpq $4, %rax
    jne .Lruntime_failure
    cmpb $'a', SOCKET_BUFFER_ADDRESS
    jne .Lruntime_failure
    cmpb $'c', SOCKET_BUFFER_ADDRESS+1
    jne .Lruntime_failure
    cmpb $'k', SOCKET_BUFFER_ADDRESS+2
    jne .Lruntime_failure
    cmpb $'!', SOCKET_BUFFER_ADDRESS+3
    jne .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq SOCKET_B_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movl $SYS_HANDLE_CLOSE, %eax
    movq SOCKET_A_HANDLE_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* GPU ABI：创建上下文、分配命令缓冲、提交并等待自动创建的 fence。 */
    movb $0x60, STAGE_MARK_ADDRESS
    movl $24, GPU_CTX_ARGS_ADDRESS
    movw $1, GPU_CTX_ARGS_ADDRESS+4
    movq $GPU_CTX_ARGS_ADDRESS, %rdi
    movq $GPU_CTX_HANDLE_ADDRESS, %rsi
    movl $SYS_GPU_CREATE_CTX, %eax
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq GPU_CTX_HANDLE_ADDRESS, %r12
    testq %r12, %r12
    jz .Lruntime_failure

    movb $0x61, STAGE_MARK_ADDRESS
    movl $24, GPU_ALLOC_ARGS_ADDRESS
    movw $1, GPU_ALLOC_ARGS_ADDRESS+4
    movq $8192, GPU_ALLOC_ARGS_ADDRESS+8
    movl $SYS_GPU_ALLOC, %eax
    movq $GPU_ALLOC_ARGS_ADDRESS, %rdi
    movq $GPU_ALLOC_HANDLE_ADDRESS, %rsi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq GPU_ALLOC_HANDLE_ADDRESS, %r13
    testq %r13, %r13
    jz .Lruntime_failure

    /* GPU_MAP：把同一分配的真实物理页映射到用户空间，并实际写入触发缺页映射。 */
    movb $0x62, STAGE_MARK_ADDRESS
    movl $48, GPU_MAP_ARGS_ADDRESS
    movw $1, GPU_MAP_ARGS_ADDRESS+4
    movq %r13, GPU_MAP_ARGS_ADDRESS+8
    movq $0, GPU_MAP_ARGS_ADDRESS+16
    movq $0, GPU_MAP_ARGS_ADDRESS+24
    movq $4096, GPU_MAP_ARGS_ADDRESS+32
    movl $3, GPU_MAP_ARGS_ADDRESS+40
    movl $0, GPU_MAP_ARGS_ADDRESS+44
    movl $SYS_GPU_MAP, %eax
    movq $GPU_MAP_ARGS_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq GPU_MAP_ARGS_ADDRESS+16, %r15
    testq %r15, %r15
    jz .Lruntime_failure
    movb $0xA5, (%r15)
    movl $SYS_VM_UNMAP, %eax
    movq %r15, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $0x63, STAGE_MARK_ADDRESS
    movl $56, GPU_SUBMIT_ARGS_ADDRESS
    movw $1, GPU_SUBMIT_ARGS_ADDRESS+4
    movq %r12, GPU_SUBMIT_ARGS_ADDRESS+8
    movq %r13, GPU_SUBMIT_ARGS_ADDRESS+16
    movq $0, GPU_SUBMIT_ARGS_ADDRESS+24
    movq $1, GPU_SUBMIT_ARGS_ADDRESS+32
    movq $0, GPU_SUBMIT_ARGS_ADDRESS+40
    movq $64, GPU_SUBMIT_ARGS_ADDRESS+48
    movl $SYS_GPU_SUBMIT, %eax
    movq $GPU_SUBMIT_ARGS_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq GPU_SUBMIT_ARGS_ADDRESS+24, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movb $0x64, STAGE_MARK_ADDRESS
    movl $SYS_GPU_WAIT_FENCE, %eax
    movq %r14, %rdi
    movl $1, %esi
    movl $1000000000, %edx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure

    /* 后续复用 r14 保存 DISPLAY_COMMIT 的 release fence，先关闭 GPU fence。 */
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    /* DISPLAY_COMMIT：提交 1x1 XRGB8888 帧，验证自动创建 release fence。 */
    movb $0x65, STAGE_MARK_ADDRESS
    movl $88, DISPLAY_ARGS_ADDRESS
    movw $1, DISPLAY_ARGS_ADDRESS+4
    movl $0, DISPLAY_ARGS_ADDRESS+8
    movl $0, DISPLAY_ARGS_ADDRESS+12
    movq %r13, DISPLAY_ARGS_ADDRESS+16
    movq $0, DISPLAY_ARGS_ADDRESS+24
    movq $4, DISPLAY_ARGS_ADDRESS+32
    movl $1, DISPLAY_ARGS_ADDRESS+40
    movl $1, DISPLAY_ARGS_ADDRESS+44
    movl $1, DISPLAY_ARGS_ADDRESS+48
    movl $0, DISPLAY_ARGS_ADDRESS+52
    movq $0, DISPLAY_ARGS_ADDRESS+56
    movq $0, DISPLAY_ARGS_ADDRESS+64
    movq $0, DISPLAY_ARGS_ADDRESS+72
    movq $1, DISPLAY_ARGS_ADDRESS+80
    movl $SYS_DISPLAY_COMMIT, %eax
    movq $DISPLAY_ARGS_ADDRESS, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movq DISPLAY_ARGS_ADDRESS+72, %r14
    testq %r14, %r14
    jz .Lruntime_failure
    movb $0x66, STAGE_MARK_ADDRESS
    movl $SYS_GPU_WAIT_FENCE, %eax
    movq %r14, %rdi
    movl $1, %esi
    movl $1000000000, %edx
    syscall
    movq %rax, RESULT_MARK_ADDRESS
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x67, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $0x68, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    movb $0x69, STAGE_MARK_ADDRESS
    movl $SYS_HANDLE_CLOSE, %eax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    js .Lruntime_failure

    movb $17, STAGE_MARK_ADDRESS
.Lexec_pin_test_wait:
    cmpb $1, EXEC_PIN_ACK_ADDRESS
    jne .Lexec_pin_test_wait
    movl $SYS_DEBUG_QUERY, %eax
    movb $0xFF, STAGE_MARK_ADDRESS
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    cmpq $1, %rax
    jne .Lruntime_failure

    movb $18, STAGE_MARK_ADDRESS
.Lexec_pin_exec_wait:
    cmpb $1, EXEC_PIN_EXEC_ACK_ADDRESS
    jne .Lexec_pin_exec_wait
    movl $SYS_PROCESS_EXEC, %eax
    movq $0x004021A0, %rdi       /* /usr/apps/libc-test/libc-test：程序路径 */
    movq $0x004021D0, %rsi       /* { "/usr/apps/libc-test/libc-test", NULL }：参数数组 */
    xorl %edx, %edx              /* envp：环境变量 */
    xorl %r10d, %r10d             /* 不继承 libc 描述符映射 */
    syscall
    testq %rax, %rax
    js .Lruntime_failure
    /* 成功时返回路径已经切换到新 ELF；若回到这里则执行失败退出。 */
    movl $1, %edi
    xorl %eax, %eax
    syscall
    ud2

.Lruntime_failure:
    /* 保留触发失败的最后一个系统调用返回值，便于内核诊断多核竞态。 */
    movq %rax, RESULT_MARK_ADDRESS
    movl $1, %edi
    xorl %eax, %eax
    syscall                         /* THREAD_EXIT(1)：线程退出 */
    ud2

/* 子线程验证初始 RDI 参数，留下可由内核检查的标记后退出。 */
/* 保持用户态运行足够长时间，使 LAPIC 至少产生一次可抢占定时中断。 */
user_runtime_timer_helper:
    cmpq $0x5678, %rdi
    jne .Ltimer_failure
.Ltimer_spin:
    pause
    cmpl $0, TIMER_STOP_ADDRESS
    je .Ltimer_spin
    xorl %edi, %edi
    xorl %eax, %eax
    syscall
    ud2
.Ltimer_failure:
    movl $3, %edi
    xorl %eax, %eax
    syscall
    ud2

user_runtime_child:
    movb $0x5A, CHILD_MARK_ADDRESS
    cmpq $0x1234, %rdi
    jne .Lchild_failure
    /* 先发布可观察标记，再发布 futex 字；父线程看到字值变化时标记必已可见。 */
    movb $0xA5, CHILD_MARK_ADDRESS
    movl $1, FUTEX_WORD_ADDRESS
    mfence
    movl $SYS_FUTEX_WAKE, %eax
    movq $FUTEX_WORD_ADDRESS, %rdi
    movl $1, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Lchild_failure
    xorl %edi, %edi
    xorl %eax, %eax
    syscall
    ud2
.Lchild_failure:
    movl $2, %edi
    xorl %eax, %eax
    syscall
    ud2

user_runtime_cow_worker:
    movb $1, COW_PROGRESS_ADDRESS
    cmpb $0x11, COW_PRIVATE_ADDRESS
    jne .Lcow_child_private_read_failure
    movb $2, COW_PROGRESS_ADDRESS
    cmpb $0x11, COW_SYNC_ADDRESS
    jne .Lcow_child_shared_read_failure
    movb $3, COW_PROGRESS_ADDRESS
    movb $0xC3, COW_PRIVATE_ADDRESS
    movb $4, COW_PROGRESS_ADDRESS
    movb $0x22, COW_SYNC_ADDRESS
    mfence
    movb $5, COW_PROGRESS_ADDRESS
    movl $SYS_FUTEX_WAKE, %eax
    movq $COW_SYNC_ADDRESS, %rdi
    movl $1, %esi
    movl $FUTEX_FLAG_SHARED, %edx
    syscall
    testq %rax, %rax
    js .Lcow_child_futex_failure
    movb $6, COW_PROGRESS_ADDRESS
.Lcow_child_loop:
    cmpb $0x33, COW_SYNC_ADDRESS
    jne .Lcow_child_loop
    cmpb $0xC3, COW_PRIVATE_ADDRESS
    jne .Lcow_child_failure
    xorl %edi, %edi
    xorl %eax, %eax
    syscall
    ud2
.Lcow_child_private_read_failure:
    movb $0xE1, COW_PROGRESS_ADDRESS
    jmp .Lcow_child_failure
.Lcow_child_shared_read_failure:
    movb $0xE2, COW_PROGRESS_ADDRESS
    jmp .Lcow_child_failure
.Lcow_child_futex_failure:
    movb $0xE3, COW_PROGRESS_ADDRESS
.Lcow_child_failure:
    movl $5, %edi
    xorl %eax, %eax
    syscall
    ud2

/*
 * 与主线程共享同一个地址空间的 VM 压力线程。参数的最低位选择固定地址，
 * 每次循环都覆盖 map、缺页、protect 和 unmap，系统调用之间允许被抢占。
 */
user_runtime_vm_worker:
    movq %rdi, %r12
    andq $1, %r12
    shlq $21, %r12
    addq $0x60000000, %r12
    leaq -64(%rsp), %r13
    movl $64, %r15d
.Lvm_worker_loop:
    movl $48, 0(%r13)
    movw $1, 4(%r13)
    movw $0, 6(%r13)
    movq %r12, 8(%r13)
    movq $4096, 16(%r13)
    movq $0, 24(%r13)
    movq $0, 32(%r13)
    movl $3, 40(%r13)
    movl $5, 44(%r13)
    movl $SYS_VM_MAP, %eax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    js .Lvm_worker_failure
    movq 8(%r13), %r14
    movb $0x5A, (%r14)

    movl $SYS_VM_PROTECT, %eax
    movq %r14, %rdi
    movl $4096, %esi
    movl $1, %edx
    syscall
    testq %rax, %rax
    js .Lvm_worker_failure
    movl $SYS_VM_PROTECT, %eax
    movq %r14, %rdi
    movl $4096, %esi
    movl $3, %edx
    syscall
    testq %rax, %rax
    js .Lvm_worker_failure
    movl $SYS_VM_UNMAP, %eax
    movq %r14, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Lvm_worker_failure
    incb CHILD_MARK_ADDRESS+2
    decl %r15d
    jnz .Lvm_worker_loop

    /* A2 工作线程退出路径诊断：
     * 1 = 64/64 次循环完成
     * 2 = VM_CONCURRENT_PASS 已写入且 mfence 已完成
     * 3 = 即将调用 SYS_THREAD_EXIT
     * 4 = SYS_THREAD_EXIT 意外返回
     */
    movb $1, CHILD_MARK_ADDRESS+3
    movb $0xD2, VM_CONCURRENT_PASS_ADDRESS
    mfence

    movb $2, CHILD_MARK_ADDRESS+3
    xorl %edi, %edi
    xorl %eax, %eax

    movb $3, CHILD_MARK_ADDRESS+3
    syscall

    movb $4, CHILD_MARK_ADDRESS+3
    ud2
.Lvm_worker_failure:
    movl $4, %edi
    xorl %eax, %eax
    syscall
    ud2

user_runtime_uaccess_worker:
    leaq -64(%rsp), %r13
.Luaccess_worker_wait:
    cmpb $1, UACCESS_START_ADDRESS
    jne .Luaccess_worker_wait
    movl $128, %r15d
.Luaccess_worker_loop:
    movl $48, 0(%r13)
    movw $1, 4(%r13)
    movw $0, 6(%r13)
    movq $UACCESS_ADDRESS, 8(%r13)
    movq $4096, 16(%r13)
    movq $0, 24(%r13)
    movq $0, 32(%r13)
    movl $3, 40(%r13)
    movl $5, 44(%r13)
    movl $SYS_VM_MAP, %eax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    js .Luaccess_worker_failure
    movq 8(%r13), %r14
    movb $0xA5, (%r14)
    movl $SYS_VM_UNMAP, %eax
    movq %r14, %rdi
    movl $4096, %esi
    syscall
    testq %rax, %rax
    js .Luaccess_worker_failure
    decl %r15d
    jnz .Luaccess_worker_loop
    movb $0xA7, UACCESS_PASS_ADDRESS
    mfence
    xorl %edi, %edi
    xorl %eax, %eax
    syscall
    ud2
.Luaccess_worker_failure:
    movl $6, %edi
    xorl %eax, %eax
    syscall
    ud2

user_runtime_wait_race_worker:
    movl $1, %r15d
.Lwait_worker_start:
    cmpb $1, WAIT_RACE_START_ADDRESS
    jne .Lwait_worker_start
.Lwait_worker_loop:
    cmpl $65, %r15d
    je .Lwait_worker_done
    cmpb %r15b, WAIT_RACE_GO_ADDRESS
    jne .Lwait_worker_loop
    movl $1, FUTEX_WORD_ADDRESS
    movl $SYS_FUTEX_WAKE, %eax
    movq $FUTEX_WORD_ADDRESS, %rdi
    movl $1, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Lwait_worker_failure
    movl %r15d, WAIT_RACE_ACK_WORD_ADDRESS
    mfence
    movl $SYS_FUTEX_WAKE, %eax
    movq $WAIT_RACE_ACK_WORD_ADDRESS, %rdi
    movl $1, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js .Lwait_worker_failure
    movb %r15b, WAIT_RACE_ACK_ADDRESS
    incl %r15d
    jmp .Lwait_worker_loop
.Lwait_worker_done:
    movb $0xB7, WAIT_RACE_PASS_ADDRESS
    mfence
    xorl %edi, %edi
    xorl %eax, %eax
    syscall
    ud2
.Lwait_worker_failure:
    movl $7, %edi
    xorl %eax, %eax
    syscall
    ud2

user_runtime_blob_end:
