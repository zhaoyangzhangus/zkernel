set pagination off
set max-value-size unlimited
target remote /tmp/system-network.sock
set $net_address = &g_e1000
set $work_address = &g_deferred
set $connections_address = &g_tcp_connections
set $queued_address = &g_e1000_poll_queued
set $lifecycle_address = &g_e1000_lifecycle_lock
# 只使用正在运行的匹配内核的 DWARF，不使用过期的辅助对象。
python
import gdb
def cast(address, name):
    return gdb.Value(int(address)).cast(gdb.lookup_type(name).pointer()).dereference()
net = cast(gdb.parse_and_eval('$net_address'), 'e1000_state_t')
work_type = next(s.type for s in gdb.lookup_static_symbols('g_deferred')
                 if s.type.code == gdb.TYPE_CODE_STRUCT)
work = gdb.Value(int(gdb.parse_and_eval('$work_address'))).cast(work_type.pointer()).dereference()
print('NETWORK queued=%d lifecycle=%d rx_clean=%d' % (
    int(gdb.parse_and_eval('*(unsigned char*)$queued_address')),
    int(gdb.parse_and_eval('*(unsigned int*)$lifecycle_address')),
    int(net['rx_clean'])))
if int(net['rx_ring']):
    print('RX descriptor_status=%#x' % int(net['rx_ring'][int(net['rx_clean'])]['status']))
    # core_internal.h 中的 E1000_RING_COUNT 为 16，不是软件队列深度。
    print('RX ready_slots=%s' % [i for i in range(16)
        if int(net['rx_ring'][i]['status']) & 1])
if int(net['mmio']) and not int(net['io_mode']):
    # 有意排除 ICR，因为读取它会清除待处理中断。
    for name,offset in [('IMS',0xd0),('RDH',0x2810),('RDT',0x2818),
                        ('TDH',0x3810),('TDT',0x3818)]:
        address=int(net['mmio'])+offset
        value=int.from_bytes(bytes(gdb.selected_inferior().read_memory(address,4)),'little')
        print('NIC %s=%#x' % (name,value))
for i in range(int(net['software_queue_count'])):
    q=net['software_queues'][i]
    print('RX_QUEUE index=%d count=%d dropped=%d lock=%d' % (
        i,int(q['count']),int(q['dropped']),int(q['lock']['state'])))
worker=work['worker']
print('DEFERRED count=%d critical_ready=%d burst=%d worker_state=%d cpu=%d rsp=%#x' % (
    int(work['count']),int(work['critical_ready']),int(work['critical_burst']),
    int(worker['state']),int(worker['owner_cpu']),int(worker['arch']['switch_ctx']['rsp'])))
gdb.execute('x/24ag %#x' % int(worker['arch']['switch_ctx']['rsp']))
for i in range(min(int(work['count']),16)):
    capacity=work['items'].type.range()[1]+1
    item=work['items'][(int(work['head'])+i)%capacity]
    print('WORK',gdb.execute('info symbol %#x'%int(item['function']),to_string=True).strip())
connections=gdb.Value(int(gdb.parse_and_eval('$connections_address'))).cast(
    gdb.lookup_type('socket_tcp_connection_t').pointer())
for i in range(64):
    entry=connections[i]
    if not int(entry['socket']): continue
    sock=entry['socket'].dereference()
    private=cast(sock['protocol_state'],'socket_private_t')
    print('TCP local_port=%d peer_port=%d socket=%#x lock=%d state=%d rx_seq=%u tx_seq=%u buffered=%d pending=%d failed=%d closed=%d peer_closed=%d' % (
        int(private['local_port']),int(private['peer_port']),int(entry['socket']),
        int(sock['lock']['state']),int(private['tcp_state']),int(private['tcp_next_sequence']),
        int(private['tcp_send_next_sequence']),int(private['stream_count']),
        int(private['tcp_send_pending']),int(private['tcp_send_failed']),
        int(private['closed']),int(private['peer_closed'])))
end
thread apply all bt 8
detach
quit
