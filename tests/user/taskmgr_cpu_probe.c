/* 仅供客户机使用的 QA 负载：无系统调用循环也会测试计时器返回边界处的安全 SIGKILL。
 * 切勿在宿主机运行此可执行文件。 */
__attribute__((noreturn)) void _start(void) {
    for (;;) __asm__ volatile ("pause" : : : "memory");
}
