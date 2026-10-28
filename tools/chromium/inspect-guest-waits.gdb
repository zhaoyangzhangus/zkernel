set pagination off
set confirm off
python
import sys
sys.path.insert(0, 'tools/chromium')
from guest_inspection import run

def snapshot():
    import gdb
    import os
    from guest_memory import read_user
    selected_tids = {int(value) for value in os.environ.get('OS_INSPECT_TIDS', '').split(',') if value}
    def offset(kind, field):
        return int(gdb.parse_and_eval('(unsigned long)&((%s*)0)->%s' % (kind,field)))
    def cast(address, kind):
        return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()
    def user_word(process, address):
        root = int(process['vm'].dereference()['root_table']['value'])
        return int.from_bytes(read_user(gdb.selected_inferior(), root, address, 8), 'little')
    registry=int(gdb.parse_and_eval('&g_process_registry'))
    cpu_states=int(gdb.parse_and_eval('&g_cpus'))
    hz=int(cast(int(gdb.parse_and_eval('&x86_boot_cpu_features')),'x86_cpu_features_t')['tsc_hz'])
    clocks=[]
    for cpu in range(int(gdb.parse_and_eval('*(unsigned int*)&g_cpu_count'))):
        state=cast(cpu_states+cpu*gdb.lookup_type('scheduler_cpu_t').sizeof,'scheduler_cpu_t')
        clock=int(state['queue']['clock_ns'])
        clocks.append(clock)
        print('cpu=%d clock_ns=%d runnable=%d current=%#x' %
              (cpu,clock,int(state['queue']['nr_running']),int(state['queue']['current'])))
    approx_tsc=max(clocks)*hz//1000000000
    print('last_tick_tsc_estimate=%#x hz=%d (not a live rdtsc)' % (approx_tsc,hz))
    node=int(cast(registry,'list_head_t')['next'])
    seen=set()
    while node != registry and node not in seen:
        seen.add(node)
        process=cast(node-offset('process_t','global_node'),'process_t')
        pid=int(process['pid'])
        name=process['name'].string()
        if 'chrome' in name:
            head=int(process['threads'].address)
            link=int(process['threads']['next'])
            visited=set()
            while link != head and link not in visited:
                visited.add(link)
                thread=cast(link-offset('thread_t','process_node'),'thread_t')
                waiter_address=int(thread['blocked_waiter'])
                text='pid=%d name=%s tid=%d state=%d cpu=%d class=%d prio=%d fs=%#x' % (pid,name,int(thread['tid']),int(thread['state']),int(thread['owner_cpu']),int(thread['sched_class']),int(thread['rt_priority']),int(thread['arch']['fs_base']))
                if waiter_address:
                    waiter=cast(waiter_address,'waiter_t')
                    text+=' waiter_state=%d deadline=%#x subscriptions=%d' % (int(waiter['state']),int(waiter['deadline_tsc']),int(waiter['subscription_count']))
                    deadline=int(waiter['deadline_tsc'])
                    if deadline != 0xffffffffffffffff and hz:
                        text+=' deadline_minus_last_tick_ms=%.3f' % ((deadline-approx_tsc)*1000/hz)
                    sources=[]
                    for i in range(min(int(waiter['subscription_count']),32)):
                        queue=int(waiter['subscriptions'][i]['queue'])
                        symbol=gdb.execute('info symbol %#x' % queue,to_string=True).strip()
                        try:
                            header=cast(queue-offset('object_header_t','signal_waiters'),'object_header_t')
                            ops=int(header['ops'])
                            if 0xffffffff80000000 <= ops < 0xffffffff90000000:
                                symbol='object:'+header['ops'].dereference()['type_name'].string()
                        except gdb.error:
                            pass
                        sources.append('%#x:%s' % (queue,symbol))
                    text+=' queues='+str(sources)
                print(text)
                # A blocked syscall keeps its entry frame at the kernel stack top.
                # Read it without changing CR3 or the stopped CPU's registers.
                try:
                    frame=cast(int(thread['kernel_stack_top'])-gdb.lookup_type('arch_trap_frame_t').sizeof,'arch_trap_frame_t')
                    if (int(frame['cs']) == 0x23 and int(thread['state']) == 1
                            and 0 <= int(frame['vector']) < 0x100):
                        print('  runnable_trap vector=%#x rip=%#x rsp=%#x rdi=%#x rsi=%#x rax=%#x' %
                              tuple(int(frame[k]) for k in ('vector','rip','rsp','rdi','rsi','rax')))
                    if int(frame['cs']) == 0x23 and int(frame['vector']) == 0x100:
                        print('  syscall=%d rip=%#x rsp=%#x rbp=%#x args=%s' %
                              (int(frame['rax']),int(frame['rip']),int(frame['rsp']),
                               int(frame['rbp']),','.join('%#x' % int(frame[k])
                               for k in ('rdi','rsi','rdx','r10','r8','r9'))))
                        if len(visited) == 1 or int(thread['tid']) in selected_tids:
                            if int(frame['rbp']):
                                fp=int(frame['rbp'])
                                chain=[]
                                try:
                                    for depth in range(24):
                                        if not 0 < fp < 0x800000000000:
                                            break
                                        chain.append(user_word(process,fp+8))
                                        next_fp=user_word(process,fp)
                                        if next_fp <= fp:
                                            break
                                        fp=next_fp
                                except (gdb.error,ValueError):
                                    pass
                                print('  user_frame_chain='+','.join('%#x' % address for address in chain))
                            sp=int(frame['rsp'])
                            try:
                                print('  main_user_stack='+','.join('%#x' % user_word(process,sp+i*8) for i in range(128)))
                            except (gdb.error, ValueError) as error:
                                print('  user_stack_unavailable='+str(error))
                except gdb.error:
                    pass
                link=int(cast(link,'list_head_t')['next'])
        node=int(cast(node,'list_head_t')['next'])

run(snapshot)
end
