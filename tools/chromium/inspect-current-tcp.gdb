set pagination off
set confirm off
python
import sys
import time
sys.path.insert(0, 'tools/chromium')
from guest_inspection import run

def snapshot():
    import gdb
    def value(expression, kind):
        address = int(gdb.parse_and_eval(expression))
        return gdb.Value(address).cast(gdb.lookup_type(kind).pointer())
    capacity = int(gdb.parse_and_eval('*(unsigned int*)&g_registry_capacity'))
    if not 0 < capacity <= 1048576:
        raise RuntimeError('Invalid socket registry capacity')
    print('TCP_SNAPSHOT host_monotonic=%.6f capacity=%d' % (time.monotonic(), capacity))
    nic = value('&g_e1000', 'e1000_state_t').dereference()
    print('NIC_RX descriptors=%d frames=%d irq=%d queues=%s' % (
        int(nic['rtl_rx_descriptor_count']), int(nic['rtl_rx_frame_count']),
        int(nic['rtl_irq_count']),
        [(int(nic['software_queues'][i]['count']),
          int(nic['software_queues'][i]['dropped']))
         for i in range(nic['software_queues'].type.sizeof //
                        nic['software_queues'][0].type.sizeof)]))
    if int(nic['rtl_rx_ring']):
        # DMA mappings are page-rounded, not a descriptor count.
        ring_size = gdb.lookup_global_symbol('system_inspection_rtl_ring_type').type.sizeof // gdb.lookup_type('rtl8126_descriptor_t').sizeof
        if not 0 < ring_size <= 4096:
            raise RuntimeError('Invalid NIC descriptor ring geometry')
        ready = sum(not (int(nic['rtl_rx_ring'][i]['options1']) & (1 << 31))
                    for i in range(ring_size))
        print('NIC_RX_RING ready=%d capacity=%d next=%d' % (
            ready, ring_size, int(nic['rtl_rx_next'])))
    if int(nic['rtl_tx_ring']):
        ring_size = gdb.lookup_global_symbol('system_inspection_rtl_ring_type').type.sizeof // gdb.lookup_type('rtl8126_descriptor_t').sizeof
        clean, next_slot, pending = (int(nic[name]) for name in
                                   ('rtl_tx_clean', 'rtl_tx_next', 'rtl_tx_pending'))
        if (not 0 < ring_size <= 4096 or not 0 <= clean < ring_size or
                not 0 <= next_slot < ring_size or not 0 <= pending <= ring_size or
                next_slot != (clean + pending) % ring_size):
            raise RuntimeError('Invalid NIC TX producer/completion geometry')
        owned = sum(bool(int(nic['rtl_tx_ring'][i]['options1']) & (1 << 31))
                    for i in range(ring_size))
        print('NIC_TX_RING submitted=%d pending=%d hardware_owned=%d clean=%d next=%d' % (
            int(nic['rtl_tx_count']), pending, owned, clean, next_slot))
    for symbol, kind in (('g_tcp_connections', 'socket_tcp_connection_t'),
                         ('g_tcp6_connections', 'socket_tcp6_connection_t')):
        table = value('*(unsigned long*)&'+symbol, kind)
        for index in range(capacity):
            entry = table[index]
            if not int(entry['socket']):
                continue
            socket = entry['socket'].dereference()
            private = gdb.Value(int(socket['protocol_state'])).cast(
                gdb.lookup_type('socket_private_t').pointer()).dereference()
            if symbol == 'g_tcp_connections':
                address = int(private['peer_address'])
                peer = '.'.join(str((address >> shift) & 255) for shift in (24, 16, 8, 0))
            else:
                peer = ':'.join('%02x%02x' % (int(private['peer_address6'][i]),
                                             int(private['peer_address6'][i + 1]))
                                for i in range(0, 16, 2))
            fields = ('local_port', 'peer_port', 'tcp_state', 'stream_count',
                      'tcp_next_sequence', 'tcp_rx_future_count',
                      'tcp_send_next_sequence', 'tcp_send_unacknowledged',
                      'tcp_send_pending', 'tcp_send_length', 'tcp_send_retransmits',
                      'tcp_peer_window', 'tcp_receive_scale', 'tcp_peer_scale',
                      'tcp_scale_offered', 'tcp_scale_negotiated',
                      'tcp_error', 'closed', 'peer_closed')
            print(symbol+' socket=%#x peer=%s ' % (int(entry['socket']), peer) +
                  ' '.join('%s=%d'%(name,int(private[name])) for name in fields))
            # 同时给出容量，避免把已收满但应用尚未读取误判为下载吞吐不足。
            receive_capacity = (int(private['stream_capacity']) if int(private['stream_dynamic'])
                                else private['stream_buffer'].type.sizeof)
            receive_count = int(private['stream_count'])
            if not 0 <= receive_count <= receive_capacity:
                raise RuntimeError('Invalid TCP receive buffer occupancy')
            print('TCP_RECEIVE_BUFFER port=%d bytes=%d capacity=%d full=%d' % (
                int(private['local_port']), receive_count, receive_capacity,
                int(receive_count == receive_capacity)))
            if int(private['tcp_rx_future_count']):
                size = int(private['stream_capacity']) if int(private['stream_dynamic']) else private['stream_buffer'].type.sizeof
                start = int(private['stream_write'])
                if not 0 < size <= 4194304 or size & (size - 1) or not 0 <= start < size:
                    raise RuntimeError('Invalid receive ring geometry')
                bits = bytes(gdb.selected_inferior().read_memory(
                    int(private['tcp_rx_present']), (size + 7) // 8))
                runs = []
                run_start = None
                for offset in range(size + 1):
                    slot = (start + offset) % size
                    present = offset < size and bool(bits[slot >> 3] & (1 << (slot & 7)))
                    if present and run_start is None:
                        run_start = offset
                    elif not present and run_start is not None:
                        runs.append((run_start, offset))
                        run_start = None
                print('TCP_RX_RETAINED port=%d ranges=%s bytes=%d' % (
                    int(private['local_port']), runs[:32], sum(b-a for a,b in runs)))

run(snapshot)
end
