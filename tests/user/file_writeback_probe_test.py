"""在宿主执行真实探针，并确认它能发现第二次回写丢失。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/runtime/libc_test.c').read_text()
start = source.index('static int test_file_writeback(void)')
end = source.index('static int test_virtual_memory(void)', start)
probe = source[start:end]
prefix = r'''
#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
static int libc_test_detail_fail(const char *stage){
 fprintf(stderr,"FAIL %s\n",stage);return 0;
}
'''
negative = r'''
static ssize_t stale_pread(int fd,void *data,size_t size,off_t offset){
 static unsigned reads;
 ssize_t count=pread(fd,data,size,offset);++reads;
 if(reads==20&&count>0)((unsigned char*)data)[0]-=96;
 return count;
}
#define pread stale_pread
'''
with tempfile.TemporaryDirectory(prefix='file-writeback-probe-') as directory:
    base = Path(directory)
    probe = probe.replace('"/writeback-XXXXXX"',
                          '"' + str(base / 'writeback-XXXXXX') + '"')
    for stale in (False, True):
        path = base / 'probe.c'
        path.write_text(prefix + (negative if stale else '') + probe +
                        '\nint main(void){return !test_file_writeback();}\n')
        binary = str(base / 'probe')
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path), '-o', binary], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=20)
        if stale:
            assert result.returncode == 1 and 'writeback-repeat-data' in result.stderr, result
        else:
            assert result.returncode == 0, result.stderr
        assert not list(base.glob('writeback-*')), '探针必须清理临时文件'
print('file writeback probe: real mmap/msync/pread, repeated writes, full file/EOF, cleanup; stale second write rejected')
