"""Keep storage structure layout and limits stable through namespace migration."""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
for name in ('fat32.h', 'block_device.h', 'block_cache.h'):
    text = (ROOT / 'include/kernel' / name).read_text()
    assert ('LITE' + 'OS_') not in text, name
    assert ('lite' + 'os_') not in text, name

# Renaming must not silently disable the existing file-lifecycle ownership gate.
verifier = (ROOT / 'tools/verify-refactor-layout.sh').read_text()
rule = next(line for line in verifier.splitlines()
            if line.startswith('forbidden_text ') and
            '(close|read_path|write_path|truncate_path)' in line)
expression = re.search(r"forbidden_text '([^']+)'", rule).group(1)
for prefix in ('', 'lite' + 'os_'):
    for name in ('close', 'read_path', 'write_path', 'truncate_path'):
        declaration = f'BOOLEAN {prefix}fat32_{name}(void) {{}}\n'
        result = subprocess.run(['grep', '-Eq', expression], input=declaration,
                                text=True)
        assert result.returncode == 0, declaration
for declaration in ('BOOLEAN fat32_open(void) {}\n',
                    'BOOLEAN fat32_sync(void) {}\n'):
    result = subprocess.run(['grep', '-Eq', expression], input=declaration,
                            text=True)
    assert result.returncode == 1, declaration

# 保留重命名前的布局基线；FAT 运行时后来明确增加可睡眠的变更互斥锁。
# 只允许这一个新增字段的大小增量，不放宽其余字段、磁盘格式或资源上限检查。
SIZES = {
    'OS_FAT32': '352840 + sizeof(io_mutex_t)', 'OS_FAT32_FILE': 560, 'OS_BLOCK_DEVICE': 88,
    'OS_BLOCK_MANAGER': 2824, 'OS_BLOCK_CACHE_ENTRY': 4120,
    'OS_BLOCK_CACHE': 66024,
}
OFFSETS = {
    'OS_FAT32': {'Device': 0, 'BytesPerSector': 8, 'FatStartLba': 40,
                 'Cache': 80, 'OpenFileLock': 66104, 'MutationLock': 66108,
                 'MutationMutex': 66112,
                 'OpenFiles': '66112 + sizeof(io_mutex_t)',
                 'Mounted': '352832 + sizeof(io_mutex_t)'},
    'OS_FAT32_FILE': {'FileSystem': 8, 'FirstCluster': 16, 'Size': 24,
                      'DirectoryLba': 40, 'DirectoryOffset': 48,
                      'CursorValid': 52, 'SeekLogical': 76,
                      'SeekIndexValid': 172, 'SeekIndexLength': 432},
    'OS_BLOCK_DEVICE': {'Read': 48, 'Context': 72, 'Registered': 80},
    'OS_BLOCK_MANAGER': {'DeviceCount': 2816, 'Initialized': 2820},
    'OS_BLOCK_CACHE_ENTRY': {'Data': 24, 'LastUse': 16},
    'OS_BLOCK_CACHE': {'Entries': 8, 'Clock': 65928, 'WaitLock': 65936,
                       'Lock': 66016, 'Initialized': 66020},
}
code = '''
#include <stddef.h>
#include <stdio.h>
#include <kernel/fat32.h>
#define CHECK(c) _Static_assert((c), #c)
CHECK(OS_FAT32_PATH_LENGTH == 256);
CHECK(OS_FAT32_MAX_OPEN_FILES == 512);
CHECK(OS_BLOCK_DEVICE_COUNT == 32);
CHECK(OS_BLOCK_NAME_LENGTH == 32);
CHECK(OS_BLOCK_CACHE_ENTRY_COUNT == 16);
CHECK(OS_BLOCK_CACHE_MAX_SIZE == 4096);
'''
for name, size in SIZES.items():
    code += f'CHECK(sizeof({name}) == {size});\n'
for name, fields in OFFSETS.items():
    for field, offset in fields.items():
        code += f'CHECK(offsetof({name}, {field}) == {offset});\n'
code += 'int main(void) { puts("Storage layout: six structures, 30 offsets, explicit runtime mutex delta and unchanged resource limits"); }\n'
with tempfile.TemporaryDirectory(prefix='storage-layout-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
