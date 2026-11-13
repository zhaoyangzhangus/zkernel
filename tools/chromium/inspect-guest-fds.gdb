set pagination off
target remote /tmp/system-fds.sock
set $registry = &g_process_registry
add-symbol-file build/current-inspection-types.o 0
python
import gdb
def cast(address, kind):
    return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()
registry=int(gdb.parse_and_eval('$registry'))
offset=int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
node=int(cast(registry,'list_head_t')['next'])
seen=set()
while node != registry and node not in seen:
    seen.add(node)
    process=cast(node-offset,'process_t')
    name=process['name'].string()
    if 'chrome' in name:
        entries=process['linux_fds']['entries']
        counts={}
        file_paths={}
        low,high=entries.type.range()
        for index in range(low,high+1):
            kind=int(entries[index]['kind'])
            if kind: counts[kind]=counts.get(kind,0)+1
            if kind == 5 and int(entries[index]['private_data']):
                window=cast(int(entries[index]['private_data']), 'window_server_window_t')
                print('  window_fd=%d window=%#x queued_events=%d wake_pending=%s' %
                      (index,int(entries[index]['private_data']),
                       int(window['event_count']),str(window['event_wake_pending'])))
            if kind == 1 and int(entries[index]['file']):
                file=cast(int(entries[index]['file']),'file_t')
                vnode=file['vnode'].dereference()
                backend=int(vnode['fs_private'])
                if backend:
                    path=bytes(gdb.selected_inferior().read_memory(backend,256)).split(b'\0',1)[0].decode('utf-8','replace')
                    file_paths[path]=file_paths.get(path,0)+1
        print('pid=%d name=%s linux_fds=%d limit=%d kinds=%s' %
              (int(process['pid']),name,sum(counts.values()),high-low+1,counts))
        # 已解除链接的临时文件可能重用名称，但对应不同 vnode。
        # 按路径分组并不能证明这些描述符指向别名。
        print('  shared_memory_fds=%d distinct_file_paths=%d largest_path_groups=%s' %
              (sum(count for path,count in file_paths.items() if '.org.chromium.Chromium.' in path),
               len(file_paths),sorted(file_paths.items(),key=lambda item:item[1],reverse=True)[:5]))
    node=int(cast(node,'list_head_t')['next'])
end
detach
quit
