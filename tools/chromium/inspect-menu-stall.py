import gdb

def cast(address, name):
    return gdb.Value(address).cast(gdb.lookup_type(name).pointer()).dereference()

def offset(name, field):
    return int(gdb.parse_and_eval('(unsigned long)&((%s*)0)->%s' % (name, field)))

cpus = int(gdb.parse_and_eval('$cpu_states'))
cpu_type = gdb.lookup_type('scheduler_cpu_t')
for index in range(4):
    cpu = cast(cpus+index*cpu_type.sizeof, 'scheduler_cpu_t')
    current = cpu['queue']['current']
    print('MENU_CPU cpu=%d current_tid=%d runnable=%d clock_ns=%d' %
          (index, int(current['tid']) if int(current) else 0,
           int(cpu['queue']['nr_running']), int(cpu['queue']['clock_ns'])))

registry = int(gdb.parse_and_eval('$registry'))
node = int(cast(registry, 'list_head_t')['next'])
seen = set()
while node != registry and node not in seen:
    seen.add(node)
    process = cast(node-offset('process_t', 'global_node'), 'process_t')
    if int(process['pid']) == 125:
        head = int(process['threads'].address)
        entry = int(process['threads']['next'])
        visited = set()
        while entry != head and entry not in visited:
            visited.add(entry)
            thread = cast(entry-offset('thread_t', 'process_node'), 'thread_t')
            if int(thread['tid']) == 126:
                stack = int(thread['arch']['switch_ctx']['rsp'])
                active = int(thread['arch']['syscall_active'])
                print('MENU_MAIN state=%d cpu=%d syscall_active=%d waiter=%#x saved_rsp=%#x' %
                      (int(thread['state']), int(thread['owner_cpu']), active,
                       int(thread['blocked_waiter']), stack))
                # entry.S 会在栈顶预留精确大小的 arch_trap_frame_t。
                # 只有处于用户系统调用期间时，此信息才有意义。
                if active:
                    top = int(thread['kernel_stack_top'])
                    frame = cast(top-gdb.lookup_type('arch_trap_frame_t').sizeof,
                                 'arch_trap_frame_t')
                    print('MENU_SYSCALL number=%d arg0=%#x arg1=%#x arg2=%#x arg3=%#x rip=%#x rsp=%#x' %
                          (int(frame['rax']), int(frame['rdi']), int(frame['rsi']),
                           int(frame['rdx']), int(frame['r10']),
                           int(frame['rip']), int(frame['rsp'])))
                if int(thread['blocked_waiter']):
                    waiter = thread['blocked_waiter'].dereference()
                    print('MENU_WAITER state=%d deadline_tsc=%#x subscriptions=%d' %
                          (int(waiter['state']), int(waiter['deadline_tsc']),
                           int(waiter['subscription_count'])))
                if stack:
                    gdb.execute('x/24ag %#x' % stack)
                break
            entry = int(cast(entry, 'list_head_t')['next'])
        break
    node = int(cast(node, 'list_head_t')['next'])
server = cast(int(gdb.parse_and_eval('$window_state')), 'window_server_state_t')
print('MENU_WINDOWS focus=%d' % int(server['focused_identifier']))
for index in range(int(server['count'])):
    window = server['windows'][index]
    if int(window):
        print('MENU_WINDOW id=%d events=%d pending=%d' %
              (int(window['identifier']), int(window['event_count']),
               int(window['event_wake_pending'])))

def anonymous(address, name):
    symbol = next(s for s in gdb.lookup_static_symbols(name)
                  if s.type.code == gdb.TYPE_CODE_STRUCT)
    return gdb.Value(int(gdb.parse_and_eval(address))).cast(symbol.type.pointer()).dereference()

input_state = anonymous('$input_state', 'g_input')
work = anonymous('$deferred_state', 'g_deferred')
print('MENU_INPUT count=%d pending=%d dropped=%d lock=%d' %
      (int(input_state['count']), int(input_state['pending']),
       int(input_state['dropped']), int(input_state['lock']['state'])))
print('MENU_DEFERRED usb_queued=%d work_count=%d critical_ready=%d worker_state=%d' %
      (int(gdb.parse_and_eval('*(unsigned char*)$usb_queued')),
       int(work['count']), int(work['critical_ready']), int(work['worker']['state'])))
