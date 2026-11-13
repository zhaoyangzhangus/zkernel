"""Actual pointer sort against an independent oracle, comparison bounds and guards."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
start = source.index('static void queue_pointer_sift(')
code = source[start:source.index('static kstatus_t wait_on_sorted_queues(', start)]
for original, replacement in [
    ('(uintptr_t)queues[child] < (uintptr_t)queues[child + 1U]',
     'before(queues[child],queues[child+1U])'),
    ('(uintptr_t)queue >= (uintptr_t)queues[child]', '!before(queue,queues[child])'),
    ('(uintptr_t)queues[ordered - 1U] <= (uintptr_t)queues[ordered]',
     '!before(queues[ordered],queues[ordered-1U])'),
    ('(uintptr_t)queues[position - 1U] > (uintptr_t)queue',
     'before(queue,queues[position-1U])'),
]:
    assert original in code
    code = code.replace(original, replacement)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned id; } wait_queue_t;
static size_t comparisons;
static bool before(wait_queue_t *a,wait_queue_t *b) {
 ++comparisons;return (uintptr_t)a<(uintptr_t)b;
}
''' + code + r'''
static int oracle(const void *a,const void *b) {
 uintptr_t left=(uintptr_t)*(wait_queue_t *const*)a,right=(uintptr_t)*(wait_queue_t *const*)b;
 return left<right?-1:left>right?1:0;
}
static wait_queue_t tokens[4096];
static wait_queue_t *arena[4098],*expected[4096];
static unsigned cases;
static void check(size_t count) {
 arena[0]=&tokens[13];arena[count+1]=&tokens[23];
 memcpy(expected,arena+1,count*sizeof(*expected));qsort(expected,count,sizeof(*expected),oracle);
 comparisons=0;queue_pointer_sort(arena+1,count);
 assert(!memcmp(expected,arena+1,count*sizeof(*expected)));
 assert(arena[0]==&tokens[13] && arena[count+1]==&tokens[23]);
 size_t logarithm=0;
 for(size_t n=count;n>1;n=(n+1)/2)++logarithm;
 if(count>16)assert(comparisons<=count*(2*logarithm+3));
 ++cases;
}
int main(void) {
 queue_pointer_sort(0,0);
 // 对三个值组成、长度不超过 9 且包含重复项的所有序列进行穷举。
 for(size_t length=0;length<=9;++length) {
  unsigned combinations=1;for(size_t i=0;i<length;++i)combinations*=3;
  for(unsigned sequence=0;sequence<combinations;++sequence) {
   unsigned encoded=sequence;
   for(size_t i=0;i<length;++i) {arena[i+1]=&tokens[encoded%3];encoded/=3;}
   check(length);
  }
 }
 unsigned rng=17;
 for(size_t count=1;count<=4096;++count) {
  for(unsigned mode=0;mode<5;++mode) {
   for(size_t i=0;i<count;++i) {
    rng=rng*1664525U+1013904223U;
    unsigned index=mode==0?(unsigned)i:mode==1?(unsigned)(count-i-1):
     mode==2?7:mode==3?rng%17:rng%4096;
    arena[i+1]=&tokens[index];
   }
   check(count);
   if(count>16 && (mode==0 || mode==2))assert(comparisons==count-1);
  }
 }
 for(size_t i=0;i<1024;++i)arena[i+1]=&tokens[1023-i];
 check(1024);
 printf("PASS: %u sort oracle cases, duplicates/small/large, guards and O(n log n) bound; reversed 1024 comparisons 523776 -> %zu\n",cases,comparisons);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-wait-sort-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
