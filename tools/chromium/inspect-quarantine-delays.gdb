set pagination off
python
import gdb, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_symbols
try:
    executable = str(validate_chrome_symbols())
except RuntimeError as error:
    raise gdb.GdbError('Refusing mismatched Chrome symbols: ' + str(error))
end
target remote /tmp/system-keys.sock
set $registry = &g_process_registry
add-symbol-file build/process-inspection-types.o 0
python
import gdb
import subprocess

def cast(address, kind):
    return gdb.Value(address).cast(gdb.lookup_type(kind).pointer()).dereference()
def word(process, address):
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    for shift in (39, 30, 21, 12):
        pte = int.from_bytes(bytes(gdb.selected_inferior().read_memory(
            0xffff800000000000 + table + ((address >> shift) & 511) * 8, 8)), 'little')
        if not pte & 1:
            raise RuntimeError('Unmapped diagnostic address; verify running Chrome build')
        if shift in (30, 21) and pte & 128:
            physical = (pte & mask & ~((1 << shift) - 1)) + (address & ((1 << shift) - 1))
            break
        table = pte & mask
    else:
        physical = table + (address & 4095)
    return int.from_bytes(bytes(gdb.selected_inferior().read_memory(
        0xffff800000000000 + physical, 8)), 'little')

symbols = {}
# 此客户机加载器将规范 Chrome PIE 映射到 0x40000000。符号必须来自精确暂存的可执行文件；
# 其他构建正在运行时不要使用此脚本。
with subprocess.Popen(['nm', '-an', executable],
                      stdout=subprocess.PIPE, text=True) as process:
    for line in process.stdout:
        fields = line.split()
        if len(fields) == 3:
            # 对旧版暂存二进制分类，但不更改其地址。
            name = fields[2].replace('g_liteos_quarantine_', 'g_platform_quarantine_').replace(
                'g_liteos_sampled_free_', 'g_platform_sampled_free_')
            if 'g_platform_quarantine_' in name or 'g_platform_sampled_free_' in name:
                symbols[name] = int(fields[0], 16) + 0x40000000
registry = int(gdb.parse_and_eval('$registry'))
offset = int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
link = int(cast(registry, 'list_head_t')['next'])
seen = set()
while link != registry and link not in seen:
    seen.add(link)
    process = cast(link - offset, 'process_t')
    if int(process['pid']) == 125:
        for prefix, labels in (
                ('g_platform_quarantine_', ('total', 'prepare', 'lock_wait', 'lock_hold', 'batch_free', 'return_buffer', 'remaining')),
                ('g_platform_sampled_free_', ('total', 'init', 'metadata', 'zap', 'quarantine'))):
            guard = next(address for name, address in symbols.items() if prefix in name and 'timing_guard' in name)
            record = next(address for name, address in symbols.items() if prefix in name and 'slow_us' in name)
            publishing = word(process, guard) & 255
            print('%s publishing=%d record=%#x' % (prefix, publishing, record))
            if publishing == 0:
                for index, label in enumerate(labels):
                    print('%s %s us=%d' % (prefix, label, word(process, record + index * 8)))
        break
    link = int(process['global_node']['next'])
else:
    raise RuntimeError('Canonical browser pid 125 is not running')
end
detach
quit
