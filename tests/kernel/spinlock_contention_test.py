"""Actual shared lock header: contention traffic, try-lock and publication."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
program=r'''
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
static atomic_uint attempts,polls,waiting;
static _Bool test_acquire(atomic_uint* p,unsigned* expected,unsigned value,memory_order a,memory_order b){
 atomic_fetch_add(&attempts,1);
 return atomic_compare_exchange_strong_explicit(p,expected,value,a,b);
}
static unsigned test_load(atomic_uint* p,memory_order order){
 atomic_fetch_add(&polls,1);return atomic_load_explicit(p,order);
}
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#pragma push_macro("atomic_load_explicit")
#undef atomic_compare_exchange_strong_explicit
#undef atomic_load_explicit
#define atomic_compare_exchange_strong_explicit test_acquire
#define atomic_load_explicit test_load
#include <kernel/spinlock.h>
#pragma pop_macro("atomic_load_explicit")
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
static spinlock_t lock;
static uint64_t protected_value,checksum;
static void* worker(void* unused){
 (void)unused;atomic_fetch_add(&waiting,1);
 for(int i=0;i<20000;i++){
  spinlock_lock(&lock);
  assert(checksum==protected_value*17);
  ++protected_value;checksum=protected_value*17;
  spinlock_unlock(&lock);
 }
 return 0;
}
int main(void){
 spinlock_init(&lock);assert(spinlock_try_lock(&lock));
 assert(!spinlock_try_lock(&lock) && atomic_load(&lock.state)==1);
 spinlock_unlock(&lock);assert(spinlock_try_lock(&lock));spinlock_unlock(&lock);
 atomic_store(&attempts,0);atomic_store(&polls,0);
 spinlock_lock(&lock);pthread_t t[4];
 for(unsigned i=0;i<4;i++)assert(!pthread_create(&t[i],0,worker,0));
 while(atomic_load(&waiting)<4 || atomic_load(&attempts)<5 || atomic_load(&polls)<20000)sched_yield();
 assert(atomic_load(&attempts)==5);
 spinlock_unlock(&lock);
 for(unsigned i=0;i<4;i++)assert(!pthread_join(t[i],0));
 assert(protected_value==80000 && checksum==1360000 && atomic_load(&lock.state)==0);
 puts("Shared spinlock: 80000 protected/publication updates, try-lock, 20000+ held-lock reads with four failed acquire attempts passed");
}
'''
# 下方的命名空间/后端模板也会初始化无关的包装器测试夹具。
# 真实 VFS 协议由 vfs_vnode_preempt_test 执行，而不是由这些原始自旋锁流量模板执行。
source = (root/'kernel/fs/vfs/core.c').read_text()
wrappers = source[source.index('static void vfs_lock('):source.index('size_t vfs_string_length(')]
vfs_program = program.replace('spinlock_lock(&lock);', 'vfs_lock();').replace(
    'spinlock_unlock(&lock);', 'vfs_unlock();')
vfs_program = vfs_program.replace('static uint64_t protected_value,checksum;',
    '#define g_vfs_lock lock\n' + wrappers + '\nstatic uint64_t protected_value,checksum;')
vfs_program = vfs_program.replace('Shared spinlock:', 'Production VFS lock:')
port_source = (root/'kernel/ipc/port.c').read_text()
port_wrappers = port_source[port_source.index('static void message_port_lock('):
                            port_source.index('typedef struct message_port_receive_context')]
port_program = program.replace('static spinlock_t lock;',
    'typedef struct { spinlock_t lock; } message_port_t;\n'
    'static message_port_t test_port;\n' + port_wrappers)
port_program = port_program.replace('spinlock_lock(&lock);', 'message_port_lock(&test_port);')
port_program = port_program.replace('spinlock_unlock(&lock);', 'message_port_unlock(&test_port);')
port_program = port_program.replace('&lock', '&test_port.lock')
port_program = port_program.replace('Shared spinlock:', 'Production message-port lock:')
completion_source = (root/'kernel/ipc/completion.c').read_text()
completion_wrappers = completion_source[completion_source.index('static void completion_port_lock('):
                                        completion_source.index('typedef struct completion_port_wait_context')]
completion_program = program.replace('static spinlock_t lock;',
    'typedef struct { spinlock_t lock; } completion_port_t;\n'
    'static completion_port_t test_port;\n' + completion_wrappers)
completion_program = completion_program.replace('spinlock_lock(&lock);', 'completion_port_lock(&test_port);')
completion_program = completion_program.replace('spinlock_unlock(&lock);', 'completion_port_unlock(&test_port);')
completion_program = completion_program.replace('&lock', '&test_port.lock')
completion_program = completion_program.replace('Shared spinlock:', 'Production completion-port lock:')
backend_source = (root/'kernel/fs/vfs/internal.h').read_text()
backend_wrappers = backend_source[backend_source.index('static inline void vfs_backend_lock('):
                                  backend_source.index('static inline void vfs_vnode_lock(')]
backend_program = program.replace('static spinlock_t lock;',
    'typedef struct { spinlock_t io_lock; } vfs_backend_node_t;\n'
    'static vfs_backend_node_t test_backend;\n'
    'static _Thread_local unsigned preempt_depth;\n'
    'static void sched_preempt_disable(void) { ++preempt_depth; }\n'
    'static void sched_preempt_enable(void) { assert(preempt_depth); --preempt_depth; }\n'
    + backend_wrappers)
backend_program = backend_program.replace('spinlock_lock(&lock);',
    'vfs_backend_lock(&test_backend); assert(preempt_depth==1);')
backend_program = backend_program.replace('spinlock_unlock(&lock);',
    'vfs_backend_unlock(&test_backend); assert(!preempt_depth);')
backend_program = backend_program.replace('&lock', '&test_backend.io_lock')
# 共享 try-lock 检查有意直接使用原始锁，因此释放操作也必须绕过后端包装器对应的抢占保护。
begin = backend_program.index('int main(void){')
end = backend_program.index(' atomic_store(&attempts,0);', begin)
raw_begin = program.index('int main(void){')
raw_end = program.index(' atomic_store(&attempts,0);', raw_begin)
backend_program = (backend_program[:begin] +
    program[raw_begin:raw_end].replace('&lock', '&test_backend.io_lock') +
    backend_program[end:])
backend_program = backend_program.replace('static _Thread_local unsigned preempt_depth;\n', '')
backend_program = backend_program.replace('static _Bool test_acquire(',
    'static _Thread_local unsigned preempt_depth;\n'
    'static _Thread_local _Bool enforce_guard;\nstatic _Bool test_acquire(')
backend_program = backend_program.replace(' atomic_fetch_add(&attempts,1);',
    ' if(enforce_guard)assert(preempt_depth);\n atomic_fetch_add(&attempts,1);')
backend_program = backend_program.replace(' atomic_fetch_add(&polls,1);',
    ' if(enforce_guard)assert(preempt_depth);\n atomic_fetch_add(&polls,1);')
backend_program = backend_program.replace('(void)unused;atomic_fetch_add(&waiting,1);',
    '(void)unused;enforce_guard=1;atomic_fetch_add(&waiting,1);')
backend_program = backend_program.replace(' atomic_store(&attempts,0);',
    ' enforce_guard=1;\n atomic_store(&attempts,0);')
backend_program = backend_program.replace('puts("Shared spinlock:',
    'sched_preempt_disable(); sched_preempt_disable();\n'
    'vfs_backend_lock(&test_backend); assert(preempt_depth==3);\n'
    'vfs_backend_unlock(&test_backend); assert(preempt_depth==2);\n'
    'sched_preempt_enable(); sched_preempt_enable(); assert(!preempt_depth);\n'
    'puts("Production VFS backend lock (nested preemption balanced):')
nativefs_programs = []
for file, prefix, end in (
    ('journal', 'journal', 'static void bytes_zero('),
    ('litefs', 'litefs', 'static bool litefs_storage_valid('),
):
    native_source = (root/f'kernel/fs/nativefs/{file}.c').read_text()
    native_wrappers = native_source[native_source.index(f'static void {prefix}_lock('):
                                    native_source.index(end)]
    fixture = port_program.replace(port_wrappers, native_wrappers)
    fixture = fixture.replace('message_port_t', f'{prefix}_t')
    fixture = fixture.replace('message_port_lock', f'{prefix}_lock')
    fixture = fixture.replace('message_port_unlock', f'{prefix}_unlock')
    fixture = fixture.replace('Production message-port lock:', f'Production {prefix} lock:')
    nativefs_programs.append(fixture)
handle_source = (root/'kernel/object/handle.c').read_text()
handle_wrappers = handle_source[handle_source.index('static void handle_lock('):
                                handle_source.index('static handle_t make_handle(')]
handle_program = backend_program.replace(backend_wrappers,
    'typedef struct { spinlock_t lock; } handle_chunk_t;\n'
    'static atomic_uintptr_t g_handle_waiting_lock;\n' + handle_wrappers)
handle_program = handle_program.replace('vfs_backend_lock(&test_backend)',
                                        'handle_lock(&test_backend.io_lock)')
handle_program = handle_program.replace('vfs_backend_unlock(&test_backend)',
                                        'handle_unlock(&test_backend.io_lock)')
handle_program = handle_program.replace('Production VFS backend lock', 'Production handle table lock')
# 句柄锁先尝试快速获取，失败后再进入自旋获取；每个等待者有两次失败尝试。
handle_program = handle_program.replace('atomic_load(&attempts)<5', 'atomic_load(&attempts)<9')
handle_program = handle_program.replace('atomic_load(&attempts)==5', 'atomic_load(&attempts)==9')
handle_program = handle_program.replace('four failed acquire attempts',
                                      'four waiters and eight failed try/lock attempts')
# 两个包装器入口共用同一保护和诊断发布逻辑。
# 同时覆盖表测试夹具中其他情况下未使用的块包装器。
handle_program = handle_program.replace('puts("Production handle table lock',
    'handle_chunk_t chunk; spinlock_init(&chunk.lock);\n'
    'chunk_lock(&chunk); assert(preempt_depth==1 && !atomic_load(&g_handle_waiting_lock));\n'
    'chunk_unlock(&chunk); assert(!preempt_depth);\n'
    'puts("Production handle table lock')
timer_source = (root/'kernel/time/hrtimer.c').read_text()
timer_wrappers = timer_source[timer_source.index('static void timer_global_lock('):
                              timer_source.index('static void timer_global_initialize(')]
timer_program = backend_program.replace(backend_wrappers,
    'static spinlock_t test_timer_global_lock;\n'
    '#define g_timer_lock test_timer_global_lock\n' + timer_wrappers)
timer_program = timer_program.replace('static vfs_backend_node_t test_backend;\n', '')
timer_program = timer_program.replace('&test_backend.io_lock', '&test_timer_global_lock')
timer_program = timer_program.replace('vfs_backend_lock(&test_backend)', 'timer_global_lock()')
timer_program = timer_program.replace('vfs_backend_unlock(&test_backend)', 'timer_global_unlock()')
timer_program = timer_program.replace('puts("Production VFS backend lock',
    'puts("Production timer global lock')
io_source = (root/'kernel/io/request.c').read_text()
io_wrappers = io_source[io_source.index('static void io_registry_lock('):
                        io_source.index('static void io_registry_insert_tail(')]
io_program = vfs_program.replace(wrappers,
    '#define g_io_registry_lock lock\n' + io_wrappers)
io_program = io_program.replace('vfs_lock()', 'io_registry_lock()')
io_program = io_program.replace('vfs_unlock()', 'io_registry_unlock()')
io_program = io_program.replace('Production VFS lock:', 'Production I/O registry lock:')
wait_source = (root/'kernel/sync/wait.c').read_text()
wait_wrappers = wait_source[wait_source.index('static uint64_t wait_lock('):
                            wait_source.index('static void wait_insert_tail(')]
wait_program = backend_program.replace(backend_wrappers,
    '#define OS_WAIT_HOST_TEST\n' + wait_wrappers)
wait_program = wait_program.replace('vfs_backend_lock(&test_backend)',
                                    'wait_lock(&test_backend.io_lock)')
wait_program = wait_program.replace('vfs_backend_unlock(&test_backend)',
                                    'wait_unlock(&test_backend.io_lock, 0U)')
wait_program = wait_program.replace('Production VFS backend lock', 'Production wait lock')
mutex_source = (root/'kernel/sync/mutex.c').read_text()
mutex_wrappers = mutex_source[mutex_source.index('static void pi_lock('):
                              mutex_source.index('static kmutex_t *mutex_from_owner_node(')]
# PI 锁已经采用抢占保护，复用带嵌套深度断言的模板。
mutex_program = backend_program.replace(backend_wrappers,
    '#define g_pi_lock test_backend.io_lock\n' + mutex_wrappers)
mutex_program = mutex_program.replace('vfs_backend_lock(&test_backend)', 'pi_lock()')
mutex_program = mutex_program.replace('vfs_backend_unlock(&test_backend)', 'pi_unlock()')
mutex_program = mutex_program.replace('Production VFS backend lock', 'Production mutex PI lock')
service_source = (root/'kernel/service.c').read_text()
service_wrappers = service_source[service_source.index('static void service_lock('):
                                  service_source.index('bool service_manager_init(')]
service_program = vfs_program.replace(wrappers,
    '#define g_services test_services\n' + service_wrappers)
service_program = service_program.replace('static spinlock_t lock;',
    'static struct { spinlock_t lock; } test_services;')
service_program = service_program.replace('&lock', '&test_services.lock')
service_program = service_program.replace('vfs_lock()', 'service_lock()')
service_program = service_program.replace('vfs_unlock()', 'service_unlock()')
service_program = service_program.replace('Production VFS lock:', 'Production service lock:')
resource_source = (root/'kernel/resource.c').read_text()
resource_wrappers = resource_source[resource_source.index('static void resource_lock('):
                                    resource_source.index('static void job_destroy(')]
resource_program = vfs_program.replace(wrappers, resource_wrappers)
resource_program = resource_program.replace('vfs_lock()', 'resource_lock(&lock)')
resource_program = resource_program.replace('vfs_unlock()', 'resource_unlock(&lock)')
resource_program = resource_program.replace('Production VFS lock:', 'Production resource lock:')
net_source = (root/'kernel/net/core/core.c').read_text()
net_wrappers = net_source[net_source.index('static void net_cache_lock('):
                          net_source.index('void net_arp_cache_init(')]
# 网络缓存锁也需要验证抢占保护和嵌套深度。
net_program = backend_program.replace(backend_wrappers, net_wrappers)
net_program = net_program.replace('vfs_backend_lock(&test_backend)',
                                  'net_cache_lock(&test_backend.io_lock)')
net_program = net_program.replace('vfs_backend_unlock(&test_backend)',
                                  'net_cache_unlock(&test_backend.io_lock)')
net_program = net_program.replace('Production VFS backend lock', 'Production ARP/NDP cache lock')
socket_source = (root/'kernel/net/socket.c').read_text()
socket_wrappers = socket_source[socket_source.index('void socket_lock('):
                                socket_source.index('void socket_initialize_globals(')]
socket_program = backend_program.replace(backend_wrappers, socket_wrappers)
socket_program = socket_program.replace('vfs_backend_lock(&test_backend)',
                                        'socket_lock(&test_backend.io_lock)')
socket_program = socket_program.replace('vfs_backend_unlock(&test_backend)',
                                        'socket_unlock(&test_backend.io_lock)')
socket_program = socket_program.replace('Production VFS backend lock', 'Production socket lock')
rcu_source = (root/'kernel/sync/rcu.c').read_text()
rcu_wrappers = rcu_source[rcu_source.index('static void rcu_lock('):
                          rcu_source.index('bool rcu_init(')]
rcu_programs = []
for gate in (False, True):
    fixture = backend_program.replace(backend_wrappers, '#define g_rcu test_rcu\n' + rcu_wrappers)
    fixture = fixture.replace('static vfs_backend_node_t test_backend;',
        'static struct { spinlock_t lock; spinlock_t gate; } test_rcu;')
    target = 'gate' if gate else 'lock'
    fixture = fixture.replace('&test_backend.io_lock', '&test_rcu.' + target)
    fixture = fixture.replace('vfs_backend_lock(&test_backend)',
        'rcu_gate_lock()' if gate else 'rcu_lock()')
    fixture = fixture.replace('vfs_backend_unlock(&test_backend)',
        'rcu_gate_unlock()' if gate else 'rcu_unlock()')
    fixture = fixture.replace('puts("Production VFS backend lock',
        '(void)rcu_lock; (void)rcu_unlock; (void)rcu_gate_lock; (void)rcu_gate_unlock;\n'
        'puts("Production RCU ' + ('gate' if gate else 'callback') + ' lock')
    rcu_programs.append(fixture)
package_source = (root/'kernel/package.c').read_text()
package_wrappers = package_source[package_source.index('static void package_lock('):
                                  package_source.index('static bool package_id_equal(')]
package_program = port_program.replace(port_wrappers, package_wrappers)
package_program = package_program.replace('message_port_t', 'package_registry_t')
package_program = package_program.replace('message_port_lock', 'package_lock')
package_program = package_program.replace('message_port_unlock', 'package_unlock')
package_program = package_program.replace('Production message-port lock:', 'Production package registry lock:')
power_source = (root/'kernel/power.c').read_text()
power_wrappers = power_source[power_source.index('static void power_lock('):
                              power_source.index('bool power_manager_init(')]
power_program = vfs_program.replace(wrappers, '#define g_power test_power\n' + power_wrappers)
power_program = power_program.replace('static spinlock_t lock;',
    'static struct { spinlock_t lock; } test_power;')
power_program = power_program.replace('&lock', '&test_power.lock')
power_program = power_program.replace('vfs_lock()', 'power_lock()')
power_program = power_program.replace('vfs_unlock()', 'power_unlock()')
power_program = power_program.replace('static atomic_uint attempts,polls,waiting;',
    'static _Thread_local unsigned preempt_depth;\n'
    'static void sched_preempt_disable(void) { ++preempt_depth; }\n'
    'static void sched_preempt_enable(void) { assert(preempt_depth); --preempt_depth; }\n'
    'static atomic_uint attempts,polls,waiting;')
power_program = power_program.replace(' atomic_fetch_add(&attempts,1);',
    ' assert(preempt_depth); atomic_fetch_add(&attempts,1);')
power_program = power_program.replace(' atomic_fetch_add(&polls,1);',
    ' assert(preempt_depth); atomic_fetch_add(&polls,1);')
# 此管理器没有公开的 try-lock 包装器。将共享头文件中的原始 try-lock 检查与生产环境
# 的受保护获取路径分开。
power_program = power_program.replace(' spinlock_init(&test_power.lock);',
    ' spinlock_init(&test_power.lock);sched_preempt_disable();')
power_program = power_program.replace(' atomic_store(&attempts,0);',
    ' sched_preempt_enable(); assert(!preempt_depth); atomic_store(&attempts,0);')
power_program = power_program.replace('power_unlock();assert(spinlock_try_lock(&test_power.lock));power_unlock();',
    'spinlock_unlock(&test_power.lock);assert(spinlock_try_lock(&test_power.lock));spinlock_unlock(&test_power.lock);')
power_program = power_program.replace(' return 0;\n}',
    ' assert(!preempt_depth); return 0;\n}')
power_program = power_program.replace('puts("Production VFS lock:',
    'assert(!preempt_depth);\n'
    'for(unsigned nesting=0;nesting<32;++nesting) {\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_disable();\n'
    ' power_lock(); assert(preempt_depth==nesting+1);\n'
    ' power_unlock(); assert(preempt_depth==nesting);\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_enable();\n'
    '}\n'
    'puts("Production VFS lock (nested preemption balanced):')
power_program = power_program.replace('Production VFS lock:', 'Production power manager lock:')
power_program = power_program.replace('Production VFS lock (nested preemption balanced):',
    'Production power manager lock (nested preemption balanced):')
watchdog_source = (root/'kernel/watchdog.c').read_text()
watchdog_wrappers = watchdog_source[watchdog_source.index('static void watchdog_lock('):
                                    watchdog_source.index('bool watchdog_manager_init(')]
watchdog_program = vfs_program.replace(wrappers, '#define g_watchdog test_watchdog\n' + watchdog_wrappers)
watchdog_program = watchdog_program.replace('static spinlock_t lock;',
    'static struct { spinlock_t lock; } test_watchdog;')
watchdog_program = watchdog_program.replace('&lock', '&test_watchdog.lock')
watchdog_program = watchdog_program.replace('vfs_lock()', 'watchdog_lock()')
watchdog_program = watchdog_program.replace('vfs_unlock()', 'watchdog_unlock()')
watchdog_program = watchdog_program.replace('static atomic_uint attempts,polls,waiting;',
    'static _Thread_local unsigned preempt_depth;\n'
    'static void sched_preempt_disable(void) { ++preempt_depth; }\n'
    'static void sched_preempt_enable(void) { assert(preempt_depth); --preempt_depth; }\n'
    'static atomic_uint attempts,polls,waiting;')
watchdog_program = watchdog_program.replace(' atomic_fetch_add(&attempts,1);',
    ' assert(preempt_depth); atomic_fetch_add(&attempts,1);')
watchdog_program = watchdog_program.replace(' atomic_fetch_add(&polls,1);',
    ' assert(preempt_depth); atomic_fetch_add(&polls,1);')
watchdog_program = watchdog_program.replace('spinlock_try_lock(&test_watchdog.lock)',
    'watchdog_try_lock()')
watchdog_program = watchdog_program.replace(' return 0;\n}',
    ' assert(!preempt_depth); return 0;\n}')
watchdog_program = watchdog_program.replace('puts("Production VFS lock:',
    'watchdog_lock(); assert(!watchdog_try_lock()); watchdog_unlock();\n'
    'assert(watchdog_try_lock()); watchdog_unlock();\n'
    'assert(!preempt_depth);\n'
    'for(unsigned nesting=0;nesting<32;++nesting) {\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_disable();\n'
    ' watchdog_lock(); assert(preempt_depth==nesting+1);\n'
    ' assert(!watchdog_try_lock() && preempt_depth==nesting+1);\n'
    ' watchdog_unlock(); assert(preempt_depth==nesting);\n'
    ' assert(watchdog_try_lock() && preempt_depth==nesting+1);\n'
    ' watchdog_unlock(); assert(preempt_depth==nesting);\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_enable();\n'
    '}\n'
    'puts("Production watchdog lock (nested preemption balanced):')
device_source = (root/'kernel/drivers/core/device.c').read_text()
device_registry_wrappers = device_source[device_source.index('static void device_lock('):
                                         device_source.index('static void device_state_lock(')]
device_registry_program = vfs_program.replace(wrappers,
    '#define g_device_lock lock\n' + device_registry_wrappers)
device_registry_program = device_registry_program.replace('vfs_lock()', 'device_lock()')
device_registry_program = device_registry_program.replace('vfs_unlock()', 'device_unlock()')
device_registry_program = device_registry_program.replace('Production VFS lock:', 'Production device registry lock:')
device_registry_program = device_registry_program.replace('static atomic_uint attempts,polls,waiting;',
    'static _Thread_local unsigned preempt_depth;\n'
    'static void sched_preempt_disable(void) { ++preempt_depth; }\n'
    'static void sched_preempt_enable(void) { assert(preempt_depth); --preempt_depth; }\n'
    'static atomic_uint attempts,polls,waiting;')
device_registry_program = device_registry_program.replace(' atomic_fetch_add(&attempts,1);',
    ' assert(preempt_depth); atomic_fetch_add(&attempts,1);')
device_registry_program = device_registry_program.replace(' atomic_fetch_add(&polls,1);',
    ' assert(preempt_depth); atomic_fetch_add(&polls,1);')
device_registry_program = device_registry_program.replace(' spinlock_init(&lock);',
    ' spinlock_init(&lock);sched_preempt_disable();')
device_registry_program = device_registry_program.replace('device_unlock();assert(spinlock_try_lock(&lock));device_unlock();',
    'spinlock_unlock(&lock);assert(spinlock_try_lock(&lock));spinlock_unlock(&lock);')
device_registry_program = device_registry_program.replace(' atomic_store(&attempts,0);',
    ' sched_preempt_enable(); assert(!preempt_depth); atomic_store(&attempts,0);')
device_registry_program = device_registry_program.replace(' return 0;\n}',
    ' assert(!preempt_depth); return 0;\n}')
device_registry_program = device_registry_program.replace('puts("Production device registry lock:',
    'assert(!preempt_depth);\n'
    'for(unsigned nesting=0;nesting<32;++nesting) {\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_disable();\n'
    ' device_lock(); assert(preempt_depth==nesting+1);\n'
    ' device_unlock(); assert(preempt_depth==nesting);\n'
    ' for(unsigned i=0;i<nesting;++i)sched_preempt_enable();\n'
    '}\n'
    'puts("Production device registry lock (nested preemption balanced):')
device_state_wrappers = device_source[device_source.index('static void device_state_lock('):
                                      device_source.index('static bool device_wait_io_idle(')]
device_state_program = device_registry_program.replace(
    '#define g_device_lock lock\n' + device_registry_wrappers, device_state_wrappers)
device_state_program = device_state_program.replace('static spinlock_t lock;',
    'typedef struct { spinlock_t state_lock; } device_t;\nstatic device_t test_device;')
device_state_program = device_state_program.replace('&lock', '&test_device.state_lock')
device_state_program = device_state_program.replace('device_lock()', 'device_state_lock(&test_device)')
device_state_program = device_state_program.replace('device_unlock()', 'device_state_unlock(&test_device)')
device_state_program = device_state_program.replace('Production device registry lock', 'Production device state lock')
dma_source = (root/'kernel/drivers/core/dma.c').read_text()
dma_wrappers = dma_source[dma_source.index('static void dma_lock('):
                          dma_source.index('static void dma_initialize(')]
dma_program = device_registry_program.replace(
    '#define g_device_lock lock\n' + device_registry_wrappers,
    '#define g_dma_lock lock\n' + dma_wrappers)
dma_program = dma_program.replace('device_lock()', 'dma_lock()')
dma_program = dma_program.replace('device_unlock()', 'dma_unlock()')
dma_program = dma_program.replace('Production device registry lock', 'Production DMA registry lock')
iommu_source = (root/'kernel/drivers/iommu/core.c').read_text()
iommu_wrappers = iommu_source[iommu_source.index('static void iommu_lock('):
                              iommu_source.index('static bool iommu_wait_register(')]
iommu_program = device_registry_program.replace(
    '#define g_device_lock lock\n' + device_registry_wrappers,
    '#define g_iommu_lock lock\n' + iommu_wrappers)
iommu_program = iommu_program.replace('device_lock()', 'iommu_lock()')
iommu_program = iommu_program.replace('device_unlock()', 'iommu_unlock()')
iommu_program = iommu_program.replace('Production device registry lock', 'Production IOMMU lock')
desktop_programs = []
for owner, filename, boundary in (
        ('input', 'kernel/drivers/input/core.c', 'static uint64_t input_timestamp('),
        ('display', 'kernel/drivers/display/core.c', 'static void display_clear_pending('),
        ('telemetry', 'kernel/trace.c', 'bool telemetry_init('),
        ('block', 'kernel/block/queue.c', 'static bool block_request_completed(')):
    owner_source = (root/filename).read_text()
    owner_wrappers = owner_source[owner_source.index(f'static void {owner}_lock('):
                                  owner_source.index(boundary)]
    owner_wrappers = owner_wrappers.replace(f'&g_{owner}.lock', '&lock')
    owner_wrappers = owner_wrappers.replace(f'&g_{owner}_lock', '&lock')
    fixture = device_registry_program.replace(
        '#define g_device_lock lock\n' + device_registry_wrappers, owner_wrappers)
    fixture = fixture.replace('device_lock()', f'{owner}_lock()')
    fixture = fixture.replace('device_unlock()', f'{owner}_unlock()')
    fixture = fixture.replace('Production device registry lock', f'Production {owner} lock')
    desktop_programs.append(fixture)
network_programs = []
for filename, owner, typename, boundary in (
        ('kernel/drivers/net/queue.c', 'e1000_queue', 'e1000_software_queue_t', 'void e1000_queue_init('),
        ('kernel/net/core/firewall.c', 'net_firewall', 'net_firewall_t', 'static bool net_firewall_prefix_matches('),
        ('kernel/drivers/audio/core.c', 'audio', 'audio_stream_t', 'static void audio_copy_bytes('),
        ('kernel/drivers/bluetooth/core.c', 'bt', 'bt_controller_t', 'static bool bt_contains_key(')):
    owner_source = (root/filename).read_text()
    owner_wrappers = owner_source[owner_source.index(f'static void {owner}_lock('):
                                  owner_source.index(boundary)]
    fixture = device_registry_program.replace(
        '#define g_device_lock lock\n' + device_registry_wrappers, owner_wrappers)
    fixture = fixture.replace('static spinlock_t lock;',
        f'typedef struct {{ spinlock_t lock; }} {typename};\nstatic {typename} test_queue;')
    fixture = fixture.replace('&lock', '&test_queue.lock')
    fixture = fixture.replace('device_lock()', f'{owner}_lock(&test_queue)')
    fixture = fixture.replace('device_unlock()', f'{owner}_unlock(&test_queue)')
    fixture = fixture.replace('Production device registry lock', f'Production {owner} lock')
    network_programs.append(fixture)
nvme_source = (root/'kernel/drivers/nvme/core.c').read_text()
nvme_wrappers = nvme_source[nvme_source.index('void nvme_lock('):
                            nvme_source.index('static uint32_t nvme_read32(')]
nvme_program = device_registry_program.replace(
    '#define g_device_lock lock\n' + device_registry_wrappers, nvme_wrappers)
nvme_program = nvme_program.replace('device_lock()', 'nvme_lock(&lock)')
nvme_program = nvme_program.replace('device_unlock()', 'nvme_unlock(&lock)')
nvme_program = nvme_program.replace('Production device registry lock', 'Production NVMe lock')
nvme_program = nvme_program.replace(' nvme_lock(&lock); assert(preempt_depth==nesting+1);',
    ' nvme_lock(&lock); assert(preempt_depth==nesting+1);\n'
    ' assert(!nvme_try_lock(&lock) && preempt_depth==nesting+1);')
nvme_program = nvme_program.replace(' nvme_unlock(&lock); assert(preempt_depth==nesting);',
    ' nvme_unlock(&lock); assert(preempt_depth==nesting);\n'
    ' assert(nvme_try_lock(&lock) && preempt_depth==nesting+1);\n'
    ' nvme_unlock(&lock); assert(preempt_depth==nesting);')
assert 'assert(!nvme_try_lock(&lock) && preempt_depth==nesting+1);' in nvme_program
assert 'assert(nvme_try_lock(&lock) && preempt_depth==nesting+1);' in nvme_program
gpu_source = (root/'kernel/drivers/gpu/core/core.c').read_text()
gpu_programs = []
for owner, boundary in (('gpu_context', 'static inline __attribute__((always_inline)) void gpu_fence_lock('),
                        ('gpu_fence', 'static bool gpu_fence_is_signaled(')):
    owner_wrappers = gpu_source[gpu_source.index(f'static inline __attribute__((always_inline)) void {owner}_lock('):
                                gpu_source.index(boundary)]
    fixture = device_registry_program.replace(
        '#define g_device_lock lock\n' + device_registry_wrappers, owner_wrappers)
    declaration = f'typedef struct {{ spinlock_t lock; }} {owner}_t;\nstatic {owner}_t test_queue;'
    if owner == 'gpu_context':
        declaration += '''
static _Thread_local unsigned debug_stage;
static void gpu_context_debug_set(unsigned value,gpu_context_t *context) {
    if(value==1)assert(context && !debug_stage);
    else if(value==2)assert(context && debug_stage==1 && preempt_depth);
    else assert(!context && debug_stage==2 && preempt_depth);
    debug_stage=value;
}
'''
    fixture = fixture.replace('static spinlock_t lock;', declaration)
    fixture = fixture.replace('&lock', '&test_queue.lock')
    fixture = fixture.replace('device_lock()', f'{owner}_lock(&test_queue)')
    fixture = fixture.replace('device_unlock()', f'{owner}_unlock(&test_queue)')
    fixture = fixture.replace('Production device registry lock', f'Production {owner} lock')
    gpu_programs.append(fixture)
# VFS 现在使用共享的受保护状态协议，而不是 spinlock_lock。
# 将真实的命名空间/后端/vnode 和可抢占等待门禁放在一起。
subprocess.run(['python3', str(root/'tests/kernel/vfs_vnode_preempt_test.py')], check=True)
for fixture in (program, port_program, completion_program,
                *nativefs_programs, handle_program, timer_program, io_program, wait_program,
                mutex_program, service_program, resource_program, net_program, socket_program,
                *rcu_programs, package_program, power_program, watchdog_program,
                device_registry_program, device_state_program, dma_program, iommu_program,
                *desktop_programs, *network_programs, nvme_program, *gpu_programs):
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory);(path/'test.c').write_text(fixture)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-I',str(root/'include'),'-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True,timeout=20)
        if fixture == nvme_program:
            leaked = fixture.replace('        sched_preempt_enable();\n', '')
            assert leaked != fixture
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                            '-pthread','-I',str(root/'include'),
                            '-fsanitize=address,undefined','-fno-pie','-no-pie',
                            '-x','c','-','-o',str(path/'test')],
                           input=leaked,text=True,check=True)
            failure=subprocess.run([str(path/'test')],text=True,capture_output=True,timeout=20)
            assert failure.returncode != 0 and 'preempt_depth' in failure.stderr
            print('Regression control: NVMe failed try-lock without guard restoration fails nested-depth gate')
        if fixture in (power_program, device_state_program, dma_program, iommu_program):
            unguarded = fixture.replace('    sched_preempt_disable();\n', '').replace(
                '    sched_preempt_enable();\n', '')
            assert unguarded != fixture
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                            '-pthread','-I',str(root/'include'),
                            '-fsanitize=address,undefined','-fno-pie','-no-pie',
                            '-x','c','-','-o',str(path/'test')],
                           input=unguarded,text=True,check=True)
            failure=subprocess.run([str(path/'test')],text=True,capture_output=True,timeout=20)
            assert failure.returncode != 0 and 'preempt_depth' in failure.stderr
            name = ('power' if fixture == power_program else
                    'device state' if fixture == device_state_program else
                    'DMA registry' if fixture == dma_program else 'IOMMU')
            print(f'Regression control: unguarded {name} lock fails the acquisition-boundary preemption gate')
