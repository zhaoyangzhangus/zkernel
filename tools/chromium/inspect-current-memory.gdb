set pagination off
set confirm off
python
import sys
import struct
import gdb
sys.path.insert(0, 'tools/chromium')
from guest_inspection import run

def snapshot():
    inferior = gdb.selected_inferior()
    def pointer(address, kind):
        return gdb.Value(address).cast(gdb.lookup_type(kind).pointer())
    def offset(kind, field):
        return int(gdb.parse_and_eval('(unsigned long)&((%s*)0)->%s' % (kind, field)))
    def members(head, kind, field, limit):
        address = int(head.address)
        node = int(head['next'])
        seen = set()
        displacement = offset(kind, field)
        while node != address:
            if not node or node in seen or len(seen) >= limit:
                raise RuntimeError('Invalid or over-limit ' + kind + ' list')
            seen.add(node)
            yield pointer(node - displacement, kind)
            node = int(pointer(node, 'list_head_t')['next'])

    # Unique vnode identity, not descriptor count: shared FDs and mappings
    # must not multiply file backing. Include unlinked, mapped-only files.
    nodes = {}
    capacity = int(gdb.parse_and_eval('*(unsigned int*)&g_vfs_node_capacity'))
    if not 0 < capacity <= 1048576:
        raise RuntimeError('Invalid vnode registry capacity')
    table = int(gdb.parse_and_eval('*(unsigned long*)&g_vfs_nodes'))
    raw = bytes(inferior.read_memory(table, capacity * 8))
    for index in range(capacity):
        address = struct.unpack_from('<Q', raw, index * 8)[0]
        if address:
            nodes[address] = pointer(address, 'vnode_t')

    stride = gdb.lookup_type('linux_fd_entry_t').sizeof
    file_offset = offset('linux_fd_entry_t', 'file')
    kind_offset = offset('linux_fd_entry_t', 'kind')
    file_kind = int(gdb.parse_and_eval('LINUX_FD_FILE'))
    file_ops = int(gdb.parse_and_eval('&g_vfs_vm_file_ops'))
    file_object = int(gdb.parse_and_eval('VM_OBJECT_FILE'))
    processes = []
    for process in members(pointer(int(gdb.parse_and_eval('&g_process_registry')),
                                   'list_head_t').dereference(),
                           'process_t', 'global_node', 16384):
        entries = process['linux_fds']['entries']
        count = entries.type.sizeof // stride
        if not 0 < count <= 65536:
            raise RuntimeError('Invalid FD table capacity')
        raw = bytes(inferior.read_memory(int(entries.address), entries.type.sizeof))
        for index in range(count):
            address = struct.unpack_from('<Q', raw, index * stride + file_offset)[0]
            if not address or raw[index * stride + kind_offset] != file_kind:
                continue
            vnode = pointer(address, 'file_t')['vnode']
            if int(vnode):
                nodes[int(vnode)] = vnode
        space = process['vm']
        if not int(space):
            continue
        processes.append((int(space['rss_pages']) / 256,
                          int(process['pid']), process['name'].string()))
        for area in members(space['area_list'], 'vm_area_t', 'ordered_node', 262144):
            obj = area['object']
            if (int(obj) and int(obj['type']) == file_object and
                    int(obj['u']['file']['ops']) == file_ops):
                address = int(obj['u']['file']['mapping'])
                if address:
                    nodes[address] = pointer(address, 'vnode_t')

    rows = []
    for vnode in nodes.values():
        address = int(vnode['fs_private'])
        if not address:
            continue
        backend = pointer(address, 'vfs_backend_node_t')
        capacity = int(backend['data_capacity'])
        pages = 0
        if int(vnode['page_cache']):
            pages = int(pointer(int(vnode['page_cache']),
                                'vfs_page_cache_t')['entry_count'])
        if capacity or pages:
            rows.append((capacity + pages * 4096, capacity, pages,
                         backend['path'].string()))
    print('MEMORY_INVENTORY unique_vnodes=%d backing_MiB=%.3f cache_MiB=%.3f' % (
        len(nodes), sum(row[1] for row in rows) / 1048576,
        sum(row[2] for row in rows) / 256))
    print('Backing capacity excludes allocator rounding/metadata; RSS can include shared pages.')
    for rss, pid, name in sorted(processes, reverse=True)[:24]:
        print('PROCESS pid=%d rss_MiB=%.3f name=%s' % (pid, rss, name))
    for _, capacity, pages, path in sorted(rows, reverse=True)[:24]:
        print('FILE backing_MiB=%.3f cache_MiB=%.3f path=%s' % (
            capacity / 1048576, pages / 256, path))

run(snapshot)
end
