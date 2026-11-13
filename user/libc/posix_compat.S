/* 为早于 extern "C" 保护的旧版 System 头文件提供 C++ ABI 转发入口。交叉汇编器生成的对象中，
 * .set 别名仍会保持未定义，因此使用简短的尾调用桩，使每个修饰符号都成为真正的导出函数。 */

    .text

#ifdef __ELF__
    .type _Z5prctliz, @function
    .type _Z5unameP7utsname, @function
    .type _Z7eventfdji, @function
    .type _Z12eventfd_readiPm, @function
    .type _Z13eventfd_writeim, @function
    .type _Z14timerfd_createii, @function
    .type _Z15timerfd_settimeiiPK10itimerspecPS_, @function
    .type _Z15timerfd_gettimeiP10itimerspec, @function
    .type _Z7opendirPKc, @function
    .type _Z7readdirP8libc_dir, @function
    .type _Z9fdopendiri, @function
    .type _Z7sysinfoP7sysinfo, @function
    .type _Z11getpriorityij, @function
    .type _Z11setpriorityiji, @function
    .type _Z8getgrnamPKc, @function
    .type _Z8closedirP8libc_dir, @function
    .type _Z12inotify_initv, @function
    .type _Z13inotify_init1i, @function
    .type _Z17inotify_add_watchiPKcj, @function
    .type _Z16inotify_rm_watchii, @function
    .type _Z9getrlimitiP6rlimit, @function
    .type _Z9setrlimitiPK6rlimit, @function
    .type _Z9getrusageiP6rusage, @function
    .type _Z7fnmatchPKcS0_i, @function
    .type _Z6setjmpP14libc_jmp_state, @function
    .type _Z7longjmpP14libc_jmp_statei, @function
    .type _Z8swprintfPwmPKwz, @function
    .type _Z9vswprintfPwmPKwP13__va_list_tag, @function
    .type _Z8wcsftimePwmPKwPK2tm, @function
    .type _Z8getgrgidj, @function
    .type _Unwind_Resume, @function
    .type _Unwind_RaiseException, @function
    .type _Unwind_DeleteException, @function
    .type _Unwind_SetGR, @function
    .type _Unwind_SetIP, @function
    .type _Unwind_GetIP, @function
    .type _Unwind_GetRegionStart, @function
    .type _Unwind_GetLanguageSpecificData, @function
    .type _Unwind_Backtrace, @function
    .type _Unwind_GetCFA, @function
    .type _Unwind_GetIPInfo, @function
    .type _Unwind_GetDataRelBase, @function
    .type _Unwind_GetTextRelBase, @function
    .type _Unwind_FindEnclosingFunction, @function
    .type __extendhfsf2, @function
#endif

    .globl _Z5prctliz
_Z5prctliz:
    jmp prctl
    .globl _Z5unameP7utsname
_Z5unameP7utsname:
    jmp uname
    .globl _Z7eventfdji
_Z7eventfdji:
    jmp eventfd
    .globl _Z12eventfd_readiPm
_Z12eventfd_readiPm:
    jmp eventfd_read
    .globl _Z13eventfd_writeim
_Z13eventfd_writeim:
    jmp eventfd_write
    .globl _Z14timerfd_createii
_Z14timerfd_createii:
    jmp timerfd_create
    .globl _Z15timerfd_settimeiiPK10itimerspecPS_
_Z15timerfd_settimeiiPK10itimerspecPS_:
    jmp timerfd_settime
    .globl _Z15timerfd_gettimeiP10itimerspec
_Z15timerfd_gettimeiP10itimerspec:
    jmp timerfd_gettime
    .globl _Z7opendirPKc
_Z7opendirPKc:
    jmp opendir
    .globl _Z7readdirP8libc_dir
_Z7readdirP8libc_dir:
    jmp readdir
    .globl _Z9fdopendiri
_Z9fdopendiri:
    jmp fdopendir
    .globl _Z7sysinfoP7sysinfo
_Z7sysinfoP7sysinfo:
    jmp sysinfo
    .globl _Z11getpriorityij
_Z11getpriorityij:
    jmp getpriority
    .globl _Z11setpriorityiji
_Z11setpriorityiji:
    jmp setpriority

    .globl _Z8getgrnamPKc
_Z8getgrnamPKc:
    jmp getgrnam
    .globl _Z8closedirP8libc_dir
_Z8closedirP8libc_dir:
    jmp closedir
    .globl _Z12inotify_initv
_Z12inotify_initv:
    jmp inotify_init
    .globl _Z13inotify_init1i
_Z13inotify_init1i:
    jmp inotify_init1
    .globl _Z17inotify_add_watchiPKcj
_Z17inotify_add_watchiPKcj:
    jmp inotify_add_watch
    .globl _Z16inotify_rm_watchii
_Z16inotify_rm_watchii:
    jmp inotify_rm_watch
    .globl _Z9getrlimitiP6rlimit
_Z9getrlimitiP6rlimit:
    jmp getrlimit
    .globl _Z9setrlimitiPK6rlimit
_Z9setrlimitiPK6rlimit:
    jmp setrlimit
    .globl _Z9getrusageiP6rusage
_Z9getrusageiP6rusage:
    jmp getrusage
    .globl _Z7fnmatchPKcS0_i
_Z7fnmatchPKcS0_i:
    jmp fnmatch
    .globl _Z6setjmpP14libc_jmp_state
_Z6setjmpP14libc_jmp_state:
    jmp setjmp
    .globl _Z7longjmpP14libc_jmp_statei
_Z7longjmpP14libc_jmp_statei:
    jmp longjmp
    .globl _Z8swprintfPwmPKwz
_Z8swprintfPwmPKwz:
    jmp swprintf
    .globl _Z9vswprintfPwmPKwP13__va_list_tag
_Z9vswprintfPwmPKwP13__va_list_tag:
    jmp vswprintf
    .globl _Z8wcsftimePwmPKwPK2tm
_Z8wcsftimePwmPKwPK2tm:
    jmp wcsftime
    .globl _Z8getgrgidj
_Z8getgrgidj:
    jmp getgrgid

/* Chromium 构建时会禁用异常。libc++abi 仍包含 ABI 入口，因此在 System 实现真正的 DWARF 展开器前，
 * 提供最小的安全失败展开器接口。 */
    .globl _Unwind_RaiseException
_Unwind_RaiseException:
    movl $5, %eax
    ret
    .globl _Unwind_DeleteException
_Unwind_DeleteException:
    ret

/* System 当前没有 DWARF 展开器。保留 libc++abi/Rust ABI 接口以便链接，并安全失败：忽略寄存器写入，
 * 上下文查询返回零，回溯结果报告栈结束。 */
    .globl _Unwind_SetGR
_Unwind_SetGR:
    ret
    .globl _Unwind_SetIP
_Unwind_SetIP:
    ret
    .globl _Unwind_GetIP
_Unwind_GetIP:
    xorl %eax, %eax
    ret
    .globl _Unwind_GetRegionStart
_Unwind_GetRegionStart:
    xorl %eax, %eax
    ret
    .globl _Unwind_GetLanguageSpecificData
_Unwind_GetLanguageSpecificData:
    xorl %eax, %eax
    ret
    .globl _Unwind_Backtrace
_Unwind_Backtrace:
    movl $5, %eax
    ret
    .globl _Unwind_GetCFA
_Unwind_GetCFA:
    xorl %eax, %eax
    ret
    .globl _Unwind_GetIPInfo
_Unwind_GetIPInfo:
    xorl %eax, %eax
    ret
    .globl _Unwind_GetDataRelBase
_Unwind_GetDataRelBase:
    xorl %eax, %eax
    ret
    .globl _Unwind_GetTextRelBase
_Unwind_GetTextRelBase:
    xorl %eax, %eax
    ret
    .globl _Unwind_FindEnclosingFunction
_Unwind_FindEnclosingFunction:
    xorl %eax, %eax
    ret

    .globl _Unwind_Resume
_Unwind_Resume:
    movl $134, %edi
    jmp _exit

/* 不使用宿主编译器运行库构建 Chromium 半精度浮点工具时，GCC 会生成此辅助函数。输入为 xmm0 的低
 * 16 位，结果以 float 形式通过 xmm0 返回。 */
    .globl __extendhfsf2
__extendhfsf2:
    movd %xmm0, %eax
    andl $0xffff, %eax
    movl %eax, %ecx
    shrl $15, %ecx
    andl $1, %ecx
    shll $31, %ecx
    movl %eax, %edx
    shrl $10, %edx
    andl $31, %edx
    andl $0x3ff, %eax
    testl %edx, %edx
    jne .Lsystem_half_nonzero_exp
    testl %eax, %eax
    je .Lsystem_half_zero
.Lsystem_half_subnormal:
    cmpl $0x400, %eax
    jae .Lsystem_half_subnormal_normalized
    shll $1, %eax
    jmp .Lsystem_half_subnormal
.Lsystem_half_subnormal_normalized:
    andl $0x3ff, %eax
    shll $13, %eax
    orl $0x38800000, %eax
    orl %ecx, %eax
    movd %eax, %xmm0
    ret
.Lsystem_half_nonzero_exp:
    cmpl $31, %edx
    je .Lsystem_half_special
    addl $112, %edx
    shll $23, %edx
    shll $13, %eax
    orl %edx, %eax
    orl %ecx, %eax
    movd %eax, %xmm0
    ret
.Lsystem_half_special:
    movl $0x7f800000, %edx
    testl %eax, %eax
    je .Lsystem_half_special_value
    movl $0x7fc00000, %edx
.Lsystem_half_special_value:
    orl %edx, %ecx
    movd %ecx, %xmm0
    ret
.Lsystem_half_zero:
    movd %ecx, %xmm0
    ret
