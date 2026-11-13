set pagination off
target remote /tmp/system-memory.sock
python
import gdb
import json
import pathlib
import struct

inferior = gdb.selected_inferior()
def address(name):
    return int(gdb.parse_and_eval('(unsigned long long)&' + name))
def scalar(name):
    return struct.unpack('<Q', inferior.read_memory(address(name), 8).tobytes())[0]

# 与 page_memory_get_stats() 一致：256 个按缓存行对齐的有符号差值。
base = address('g_memory_page_deltas')
delta = sum(struct.unpack('<q', inferior.read_memory(base + cpu * 64, 8).tobytes())[0]
            for cpu in range(256))
total = scalar('g_memory_total_pages')
initial = scalar('g_memory_initial_free_pages')
free = min(total, max(0, initial + delta))
result = {'total_pages': total, 'free_pages': free,
          'allocated_since_initial_pages': -delta,
          'total_mib': total / 256, 'free_mib': free / 256,
          'verification': 'Read-only snapshot of guest physical allocator counters'}
pathlib.Path('build/chrome-physical-memory.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
end
detach
quit
