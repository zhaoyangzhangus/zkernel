set pagination off
set max-value-size unlimited
target remote /tmp/system-network.sock
set $network_state = &g_e1000
set $work_state = &g_deferred
set $poll_queued_address = &g_e1000_poll_queued
set $lifecycle_address = &g_e1000_lifecycle_lock
# 只使用正在运行的匹配内核的 DWARF，不使用过期的辅助对象。
python
import gdb
net = gdb.Value(int(gdb.parse_and_eval('$network_state'))).cast(gdb.lookup_type('e1000_state_t').pointer()).dereference()
symbol = next((s for s in gdb.lookup_static_symbols('g_deferred')
               if s.type.code == gdb.TYPE_CODE_STRUCT), None)
if symbol is None:
    raise RuntimeError('Missing deferred debug type; do not assume queue empty')
work = gdb.Value(int(gdb.parse_and_eval('$work_state'))).cast(symbol.type.pointer()).dereference()
print('poll_queued=%d lifecycle_lock=%d rx_clean=%d' %
      (int(gdb.parse_and_eval('*(unsigned char*)$poll_queued_address')),
       int(gdb.parse_and_eval('*(unsigned int*)$lifecycle_address')),
       int(net['rx_clean'])))
if int(net['rx_ring']):
    print('rx_descriptor_status=%#x' % int(net['rx_ring'][int(net['rx_clean'])]['status']))
worker=work['worker']
print('deferred_count=%d worker_state=%d cpu=%d rsp=%#x critical_ready=%d' %
      (int(work['count']),int(worker['state']),int(worker['owner_cpu']),
       int(worker['arch']['switch_ctx']['rsp']),int(work['critical_ready'])))
gdb.execute('x/48ag %#x' % int(worker['arch']['switch_ctx']['rsp']))
for index in range(min(int(work['count']),16)):
    capacity=work['items'].type.range()[1]+1
    item=work['items'][(int(work['head'])+index)%capacity]
    print('queued_work',gdb.execute('info symbol %#x'%int(item['function']),to_string=True).strip())
end
thread apply all bt 6
detach
quit
