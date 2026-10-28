"""执行实际保存替换探针，验证连续替换、旧句柄和删除后的内容。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/runtime/libc_test.c').read_text()
start = source.index('static int test_file_replace(')
probe = source[start:source.index('static int test_file_writeback(', start)]
prefix = r'''
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
static int libc_test_detail_fail(const char *s){fprintf(stderr,"FAIL %s\n",s);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='file-replace-probe-') as directory:
    base = Path(directory)
    probe = probe.replace('"/save-check-XXXXXX"', '"' + directory + '/save-check-XXXXXX"')
    for broken in (False, True):
        # 假装替换成功但没有提交目录变更，探针必须发现新打开的文件仍是旧内容。
        negative = '#define rename(a,b) ((void)(a),(void)(b),0)\n' if broken else ''
        code = prefix + negative + probe + '\nint main(void){return !test_file_replace(false);}\n'
        binary = str(base / 'probe')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        if broken:
            assert result.returncode != 0 and 'replace-new-inode' in result.stderr, result
        else:
            assert result.returncode == 0, result
        assert not list(base.glob('save-check-*')), '探针遗留文件'
print('PASS: actual replacement probe, three saves, retained old inode, enumeration and unlink; fake successful rename rejected')
