#!/usr/bin/env python3
"""Measure production cluster lookup on a synthetic fragmented FAT chain."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/nativefs/file_lifecycle.c').read_text()
begin = source.index('static BOOLEAN locate_file_cluster(')
function = source[begin:source.index('static BOOLEAN count_file_clusters(', begin)]
scan = '''        UINT32 hints = file->SeekIndexValid;
        while (hints != 0U) {
            UINT32 i = (UINT32)__builtin_ctz(hints);
            hints &= hints - 1U;'''
assert scan in function
old_scan = '''        for (UINT32 i = 0U; i < 32U; ++i) {
            ++hint_visits;
            if ((file->SeekIndexValid & (1U << i)) == 0U) continue;'''
spacing_begin = function.index('    UINT64 cluster_bytes')
spacing_end = function.index('    if (stride == 0U) stride = 1U;', spacing_begin)
spacing_end += len('    if (stride == 0U) stride = 1U;\n')
spacing = function[spacing_begin:spacing_end]
reference = function.replace(scan, old_scan).replace(spacing, '')
reference = reference.replace('    if (!file->CursorValid) {',
                              spacing + '\n    if (!file->CursorValid) {', 1)
reference = reference.replace('UINT32 start = file->FirstCluster;', 'UINT32 start;')
reference = reference.replace('    for (UINT32 hops = 0;', '''    if (!file->CursorValid || logical_cluster < file->CursorLogicalStart) {
        start = file->FirstCluster;
        logical = 0U;
    }
    for (UINT32 hops = 0;''', 1)
reference = reference.replace('locate_file_cluster(', 'reference_locate_file_cluster(', 1)
reference = reference.replace('    UINT64 cluster_bytes',
                              '    ++spacing_calculations;\n    UINT64 cluster_bytes')
function = function.replace('    UINT64 cluster_bytes',
                            '    ++spacing_calculations;\n    UINT64 cluster_bytes')
function = function.replace('while (hints != 0U) {',
                            'while (hints != 0U) { ++hint_visits;')
function = function.replace('for (UINT32 i = 0; i < file->SeekCacheCount; ++i) {',
                            'for (UINT32 i = 0; i < file->SeekCacheCount; ++i) { ++hint_visits;')
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int BOOLEAN;
typedef struct { UINT32 ClusterCount,BytesPerSector,SectorsPerCluster; BOOLEAN Fat3Available; } OS_FAT32;
typedef struct { OS_FAT32 *FileSystem; UINT32 FirstCluster;
 BOOLEAN CursorValid; UINT32 CursorLogicalStart,CursorPhysicalStart,CursorLength;
 UINT32 SeekCacheCount,SeekCacheNext,SeekLogical[8],SeekPhysical[8],SeekLength[8];
 UINT64 Size; UINT32 SeekIndexValid;
 UINT32 SeekIndexLogical[32],SeekIndexPhysical[32],SeekIndexLength[32];
} OS_FAT32_FILE;
static unsigned long walks;
static unsigned long hint_visits;
static unsigned long spacing_calculations;
static unsigned extent_mode;
static BOOLEAN fat32_cluster_valid(const OS_FAT32*f,UINT32 c){
 return c>=2 && c<=f->ClusterCount+1;
}
static UINT32 fat32_extent_length(const OS_FAT32*f,UINT32 c,UINT32 maximum){
 (void)f;(void)maximum;return extent_mode ? 4U-(c-2U)%8U : 1U;
}
static BOOLEAN fat32_read_next_cluster(const OS_FAT32*f,UINT32 c,UINT32*out){
 ++walks;*out=extent_mode ? c+(((c-2U)%8U==3U)?5U:1U) : c+2;
 return fat32_cluster_valid(f,*out);
}
'''
main = r'''
int main(void){
 /* All one-bit masks, dense/empty masks and random masks. Compare every
  * resulting cache byte and physical result with the original ascending
  * 32-slot scan, including ties and unordered logical hints. */
 UINT32 random=19;
 unsigned long visits=0,reference_visits=0,calculations=0,reference_calculations=0;
 OS_FAT32 small_fs={.ClusterCount=128,.BytesPerSector=512,.SectorsPerCluster=8};
 for(unsigned trial=0;trial<20000;++trial) {
  small_fs.BytesPerSector=trial%11 ? 512 : 0;
  small_fs.SectorsPerCluster=trial%17 ? 8 : 0;
  random=random*1664525U+1013904223U;
  UINT32 mask=trial<32 ? 1U<<trial : (trial==32 ? 0U :
                         trial==33 ? UINT32_MAX : random);
  OS_FAT32_FILE actual={.FileSystem=&small_fs,.FirstCluster=2,
   .Size=64*4096,.CursorValid=1,.CursorLogicalStart=0,
   .CursorPhysicalStart=2,.CursorLength=1,.SeekIndexValid=mask,
   .SeekCacheCount=trial%9,.SeekCacheNext=trial%8};
  for(unsigned i=0;i<32;++i) {
   random=random*1664525U+1013904223U;
   actual.SeekIndexLogical[i]=(random>>8)%64;
   actual.SeekIndexPhysical[i]=2+2*actual.SeekIndexLogical[i];
   actual.SeekIndexLength[i]=1;
  }
  for(unsigned i=0;i<8;++i) {
   random=random*1664525U+1013904223U;
   actual.SeekLogical[i]=(random>>8)%64;
   actual.SeekPhysical[i]=2+2*actual.SeekLogical[i];
   actual.SeekLength[i]=1;
  }
  if(trial%7==0)actual.CursorValid=0;
  if(trial%13==0)actual.FirstCluster=0;
  if(trial%5==0) {
   actual.CursorLogicalStart=32;actual.CursorPhysicalStart=66;actual.CursorLength=0;
  }
  OS_FAT32_FILE expected=actual;
  UINT32 target=trial%64,a=0xdeadbeef,b=a;
  hint_visits=spacing_calculations=0;
  BOOLEAN ok=locate_file_cluster(&actual,target,&a);
  visits+=hint_visits;
  calculations+=spacing_calculations;
  hint_visits=spacing_calculations=0;
  BOOLEAN previous_ok=reference_locate_file_cluster(&expected,target,&b);
  reference_visits+=hint_visits;
  reference_calculations+=spacing_calculations;
  assert(ok==previous_ok && a==b && !memcmp(&actual,&expected,sizeof(actual)));
  if(ok)assert(a==2+2*target);
 }
 assert(visits<reference_visits);
 assert(calculations<reference_calculations);
 printf("Checkpoint-spacing calculations: %lu -> %lu; exact output/cache-state equivalence including invalidation/zero geometry passed\n",reference_calculations,calculations);
 printf("20000 sparse/dense/unordered index states: exact old-scan equivalence; hint probes %lu -> %lu\n",reference_visits,visits);
 OS_FAT32_FILE sparse={.FileSystem=&small_fs,.FirstCluster=2,
  .Size=64*4096,.CursorValid=1,.CursorLogicalStart=0,
  .CursorPhysicalStart=2,.CursorLength=1,.SeekIndexValid=1U<<31};
 sparse.SeekIndexLogical[31]=63;sparse.SeekIndexPhysical[31]=128;sparse.SeekIndexLength[31]=1;
 UINT32 sparse_out;hint_visits=spacing_calculations=0;
 assert(locate_file_cluster(&sparse,63,&sparse_out)&&sparse_out==128&&hint_visits==1);
 assert(spacing_calculations==0);
 puts("single valid high-bit index: 32 probes -> 1; zero mask never invokes ctz");
 walks=0;
 OS_FAT32 fs={.ClusterCount=32768,.BytesPerSector=512,.SectorsPerCluster=8};
 OS_FAT32_FILE file={.FileSystem=&fs,.FirstCluster=2,.Size=8192ULL*4096};
 UINT32 out;
 for(UINT32 i=0;i<8192;++i){
  assert(locate_file_cluster(&file,i,&out));assert(out==2+2*i);
 }
 printf("sequential 8192 lookups: %lu FAT links\n",walks);
 assert(walks==8191);
 walks=0;file.CursorValid=0;
 for(UINT32 i=0;i<128;++i){
  UINT32 target=(i%2)?4096:8191;
  assert(locate_file_cluster(&file,target,&out));assert(out==2+2*target);
 }
 printf("alternating 128 lookups: %lu FAT links\n",walks);
 assert(walks<=12287);
 /* A resize/reopen invalidates CursorValid: stale hints must not survive. */
 file.CursorValid=0;
 file.SeekIndexValid=UINT32_MAX;
 for(unsigned i=0;i<32;++i) {
  file.SeekIndexLogical[i]=4096;file.SeekIndexPhysical[i]=999;file.SeekIndexLength[i]=1;
 }
 file.SeekCacheCount=8;file.SeekCacheNext=7;
 for(unsigned i=0;i<8;++i){
  file.SeekLogical[i]=4096;file.SeekPhysical[i]=999;file.SeekLength[i]=1;
 }
 assert(locate_file_cluster(&file,4096,&out));assert(out==8194);
 assert(file.SeekCacheCount==1 && file.SeekCacheNext==1);
 /* Exercise eviction and backward seeks, checking every resolved cluster. */
 UINT32 seed=17;
 walks=0;
 for(unsigned i=0;i<2000;++i){
  seed=seed*1664525U+1013904223U;
  UINT32 target=(seed>>8)%8192;
  assert(locate_file_cluster(&file,target,&out));assert(out==2+2*target);
  assert(file.SeekCacheCount<=8 && file.SeekCacheNext<8);
 }
 printf("randomized 2000 lookups: %lu FAT links\n",walks);
 /* Preserve the current random-read baseline while extending seek caching.
  * Sequential/alternating tests alone conceal repeated long chain walks. */
 assert(walks<1715559UL/4);
 /* Empty/small files, large sizes and changed interval widths must never
  * reuse physical checkpoints after cursor invalidation. */
 const UINT64 sizes[]={0,1,4096,8192ULL*4096,UINT64_MAX};
 for(unsigned mode=0;mode<sizeof(sizes)/sizeof(sizes[0]);++mode) {
  file.Size=sizes[mode]; file.CursorValid=0;
  for(unsigned i=0;i<300;++i) {
   seed=seed*1664525U+1013904223U;
   UINT32 target=(seed>>8)%8192;
   assert(locate_file_cluster(&file,target,&out));assert(out==2+2*target);
  }
 }
 puts("sparse index: size boundaries and stale-index invalidation passed");
 extent_mode=1;file.Size=8192ULL*4096;file.CursorValid=0;
 for(unsigned i=0;i<2000;++i) {
  seed=seed*1664525U+1013904223U;
  UINT32 target=(seed>>8)%8192;
  assert(locate_file_cluster(&file,target,&out));
  assert(out==2+target+(target/4)*4);
 }
 puts("mixed contiguous extents: 2000 random physical mappings passed");
 /* With both hint caches populated, repeated hits must preserve every cache
  * byte, issue no FAT reads, and avoid all 32+8 hint probes. */
 assert(locate_file_cluster(&file,8191,&out));
 assert(file.SeekCacheCount==8);
 OS_FAT32_FILE before=file;
 unsigned long old_walks=walks;
 hint_visits=0;
 for(unsigned i=0;i<100000;++i) {
  UINT32 target=8188+i%4;
  assert(locate_file_cluster(&file,target,&out));
  assert(out==2+target+(target/4)*4);
  assert(!memcmp(&file,&before,sizeof(file)));
 }
 assert(hint_visits==0 && walks==old_walks);
 puts("100000 current-extent hits: zero hint probes or FAT links, exact cache preservation passed");
 hint_visits=0;
 assert(locate_file_cluster(&file,8192,&out));
 assert(out==2+8192+(8192/4)*4 && hint_visits>0);
 hint_visits=0;
 assert(locate_file_cluster(&file,8187,&out));
 assert(out==2+8187+(8187/4)*4 && hint_visits>0);
 puts("outside-extent forward/backward requests retain seek-hint scans");
 /* Inclusive low/exclusive high boundaries, empty files, physical range
  * rejection and logical positions near UINT32_MAX retain their guards. */
 file=(OS_FAT32_FILE){.FileSystem=&fs,.FirstCluster=2,.CursorValid=1,
  .CursorLogicalStart=UINT32_MAX-3,.CursorPhysicalStart=2,.CursorLength=4};
 for(unsigned i=0;i<4;++i) {
  assert(locate_file_cluster(&file,UINT32_MAX-3+i,&out)&&out==2+i);
 }
 file.CursorPhysicalStart=fs.ClusterCount+2;
 assert(!locate_file_cluster(&file,UINT32_MAX-3,&out));
 file.FirstCluster=0;
 assert(!locate_file_cluster(&file,UINT32_MAX-3,&out));
 file=(OS_FAT32_FILE){.FileSystem=&fs,.FirstCluster=2,.Size=8192ULL*4096,
  .CursorValid=1,.CursorLogicalStart=100,.CursorPhysicalStart=202,.CursorLength=0};
 assert(locate_file_cluster(&file,101,&out));
 assert(out==2+101+(101/4)*4);
 puts("current-extent bounds, empty file, invalid physical cluster and zero-length fallback passed");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fat-seek-') as directory:
    binary = pathlib.Path(directory) / 'test'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', str(binary)],
                   input=harness + function + reference + main, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
    # 负对照只移除当前区间的快速命中路径，必须触发重复探测断言。
    guard = function.index('    if (file->CursorValid &&')
    guard_end = function.index('    if (!file->CursorValid) {', guard)
    previous = function[:guard] + function[guard_end:]
    control = pathlib.Path(directory) / 'previous'
    control_main = main.replace('assert(hint_visits==0 && walks==old_walks);',
                               'if(hint_visits!=0 || walks!=old_walks)return 42;')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', str(control)],
                   input=harness + previous + reference + control_main, text=True, check=True)
    result = subprocess.run([str(control)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 42, result.stdout + result.stderr
    print('Negative control rejects the previous repeated hint scans')
