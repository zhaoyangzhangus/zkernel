set pagination off
target remote /tmp/system-observers.sock
set $registry = &g_process_registry
add-symbol-file build/process-inspection-types.o 0
python
import gdb
import os

def cast(address, kind):
    return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()

def word(process, address):
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    for shift in (39, 30, 21, 12):
        pte = int.from_bytes(bytes(gdb.selected_inferior().read_memory(
            0xffff800000000000 + table + ((address >> shift) & 511) * 8, 8)), 'little')
        if not pte & 1:
            raise RuntimeError('Unmapped observer address; verify running Chrome symbols')
        if shift in (30, 21) and pte & 128:
            physical = (pte & mask & ~((1 << shift) - 1)) + (address & ((1 << shift) - 1))
            break
        table = pte & mask
    else:
        physical = table + (address & 4095)
    return int.from_bytes(bytes(gdb.selected_inferior().read_memory(
        0xffff800000000000 + physical, 8)), 'little')

registry = int(gdb.parse_and_eval('$registry'))
offset = int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
node = int(cast(registry, 'list_head_t')['next'])
seen = set()
browser = None
pid = int(os.environ.get('OS_BROWSER_PID', '125'))
while node != registry and node not in seen and len(seen) < 4096:
    seen.add(node)
    process = cast(node - offset, 'process_t')
    if int(process['pid']) == pid:
        browser = process
        break
    node = int(process['global_node']['next'])
if browser is None:
    raise RuntimeError('Browser PID not present')
# 调用方根据正在运行的精确 Chrome 二进制计算链表地址。
# libc++ 链表节点依次包含前驱、后继和 observer 指针。observer 的 Itanium ABI 虚表中，
# 此方法前有两个析构函数槽位。
head = int(os.environ['OS_OBSERVER_LIST'], 0)
node = word(browser, head + 8)
seen = set()
while node != head and node not in seen and len(seen) < 64:
    seen.add(node)
    observer = word(browser, node + 16)
    vtable = word(browser, observer)
    method = word(browser, vtable + 16)
    print('OBSERVER index=%d object=%#x vtable=%#x method=%#x' % (
        len(seen) - 1, observer, vtable, method))
    node = word(browser, node + 8)
if node != head:
    raise RuntimeError('Observer list did not terminate; do not interpret partial data')
end
detach
quit
