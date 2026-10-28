#!/usr/bin/env python3
"""Compile the production memory backend; count append allocations."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/backend.c').read_text()
start = source.index('struct {\n    uint64_t failures;')
end = source.index('kstatus_t vfs_fat32_backend_read(', start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int kstatus_t;
enum { K_OK=0, K_EINVAL=-1, K_EOVERFLOW=-2, K_ENOMEM=-3 };
typedef struct { uint8_t *data; uint64_t size, data_capacity; bool directory; } vfs_backend_node_t;
static unsigned allocations, fail;
static void *kmalloc(size_t n, int flags) {
    (void)flags; ++allocations;
    if (fail) { --fail; return NULL; }
    return malloc(n);
}
static void *kzalloc(size_t n,int flags){void *p=kmalloc(n,flags);if(p)memset(p,0,n);return p;}
static void kfree(void *p) { free(p); }
/* malloc-only policy fixture: real in-place growth is tested separately. */
static bool kmalloc_try_grow(void *p,size_t n){(void)p;(void)n;return false;}
static unsigned stats_calls;
static kstatus_t page_memory_get_stats(uint64_t *total, uint64_t *available) {
    ++stats_calls; *total=1048576; *available=524288; return K_OK;
}
void *liteos_memcpy(void *, const void *, size_t);
void *liteos_memmove(void *, const void *, size_t);
void *liteos_memset(void *, int, size_t);
''' + source[start:end].replace('memcpy(', 'liteos_memcpy(').replace('memmove(', 'liteos_memmove(').replace('memset(', 'liteos_memset(') + r'''
int main(void) {
    vfs_backend_node_t b={0}; uint64_t bytes=99;
    uint8_t block[4096]; memset(block, 0x5a, sizeof(block));
    for (unsigned i=0; i<1024; ++i)
        assert(vfs_memory_backend_write(&b, b.size, block, sizeof(block), &bytes)==K_OK);
    assert(allocations==11 && b.size==4194304);
    for (unsigned i=0; i<b.size; ++i) assert(b.data[i]==0x5a);
    unsigned before=allocations;
    assert(g_vfs_memory_allocation_failure.state==0 && stats_calls==0);
    assert(vfs_memory_backend_truncate(&b, 17)==K_OK);
    assert(b.data_capacity==17 && allocations==before+1);
    assert(vfs_memory_backend_truncate(&b, 8192)==K_OK);
    assert(allocations==before+2 && b.data_capacity==8192);
    for (unsigned i=17; i<8192; ++i) assert(b.data[i]==0);
    assert(vfs_memory_backend_write(&b, 10000, block, 1, &bytes)==K_OK);
    for (unsigned i=8192; i<10000; ++i) assert(b.data[i]==0);
    assert(b.data[10000]==0x5a);
    for (unsigned i=0;i<100;++i) b.data[i]=(uint8_t)i;
    assert(vfs_memory_backend_write(&b,1,b.data,99,&bytes)==K_OK);
    for (unsigned i=1;i<100;++i) assert(b.data[i]==i-1);
    assert(vfs_memory_backend_read(&b,1,b.data,99,&bytes)==K_OK && bytes==99);
    for (unsigned i=0;i<99;++i) assert(b.data[i]==i);
    b.data[0]=0x5a;
    assert(vfs_memory_backend_read(&b,b.size,NULL,0,&bytes)==K_OK && bytes==0);
    assert(vfs_memory_backend_write(&b, 20000, NULL, 0, &bytes)==K_OK);
    assert(b.size==10001 && bytes==0);
    uint8_t *old=b.data; fail=2;
    assert(vfs_memory_backend_write(&b, 8388608, block, 1, &bytes)==K_ENOMEM);
    assert(b.data==old && b.size==10001 && b.data[0]==0x5a);
    assert(g_vfs_memory_allocation_failure.state==2 && stats_calls==1);
    assert(g_vfs_memory_allocation_failure.failures==1);
    assert(g_vfs_memory_allocation_failure.required==8388609);
    assert(g_vfs_memory_allocation_failure.attempted_capacity==8388609);
    assert(g_vfs_memory_allocation_failure.previous_size==10001);
    assert(g_vfs_memory_allocation_failure.previous_capacity==16384);
    assert(g_vfs_memory_allocation_failure.total_pages==1048576);
    assert(g_vfs_memory_allocation_failure.free_pages==524288);
    assert(g_vfs_memory_allocation_failure.stats_status==K_OK);
    fail=1;
    assert(vfs_memory_backend_write(&b, 8388608, block, 1, &bytes)==K_OK);
    assert(b.data_capacity==8388609);
    old=b.data; fail=1; before=allocations;
    assert(vfs_memory_backend_truncate(&b,16777217)==K_ENOMEM);
    assert(allocations==before+1 && fail==0);
    assert(b.data==old && b.size==8388609 && b.data_capacity==8388609);
    assert(b.data[0]==0x5a && b.data[8388608]==0x5a);
    assert(g_vfs_memory_allocation_failure.failures==2 && stats_calls==1);
    assert(g_vfs_memory_allocation_failure.required==8388609);
    assert(vfs_memory_backend_truncate(&b, 0)==K_OK);
    assert(!b.data && !b.size && !b.data_capacity);
    assert(g_vfs_memory_allocation_failure.free_pages==524288);
    // Snapshot oracle for self-sourced writes: overlap, exact end, growth,
    // sparse holes, optional-capacity failure and complete allocation failure.
    unsigned cases=0;
    const unsigned sizes[]={17,255,4096};
    for(unsigned si=0;si<3;++si)for(unsigned src=0;src<3;++src)
    for(unsigned dest=0;dest<5;++dest)for(unsigned failure=0;failure<3;++failure) {
        unsigned size=sizes[si],source_offset=src==0?0:src==1?size/2:size-1;
        unsigned count=size-source_offset;
        unsigned offset=dest==0?0:dest==1?1:dest==2?size-1:dest==3?size:size+31;
        unsigned required=offset+count,visible=required>size?required:size;
        b=(vfs_backend_node_t){.data=malloc(size),.size=size,.data_capacity=size};
        assert(b.data);
        unsigned char *expected=calloc(1,visible);
        unsigned char *snapshot=malloc(count);assert(expected && snapshot);
        for(unsigned i=0;i<size;++i)b.data[i]=(unsigned char)(i*19+7);
        memcpy(expected,b.data,size);memcpy(snapshot,b.data+source_offset,count);
        memcpy(expected+offset,snapshot,count);
        old=b.data;before=allocations;bytes=123;
        unsigned capacity=size<4096?4096:size;
        while(capacity<required)capacity*=2;
        bool no_fallback=capacity==required;
        fail=failure;
        kstatus_t result=vfs_memory_backend_write(&b,offset,b.data+source_offset,count,&bytes);
        if(required>size && (failure==2 || (failure==1 && no_fallback))) {
            assert(result==K_ENOMEM && b.data==old && b.size==size && b.data_capacity==size);
            assert(bytes==123 && allocations==before+(no_fallback?1:2));
            for(unsigned i=0;i<size;++i)assert(b.data[i]==(unsigned char)(i*19+7));
        } else {
            assert(result==K_OK && bytes==count && b.size==visible);
            assert(!memcmp(b.data,expected,visible));
            assert(allocations==before+(required>size?(failure==1?2:1):0));
        }
        fail=0;free(expected);free(snapshot);free(b.data);++cases;
    }
    // A caller may also supply valid bytes from spare allocation capacity.
    // Only visible file bytes are migrated, so the source must stay in the
    // retired allocation until consumed rather than being blindly rebased.
    const unsigned spare_offsets[]={17,32,63};
    for(unsigned i=0;i<3;++i) {
        unsigned source_offset=spare_offsets[i],count=64-source_offset;
        b=(vfs_backend_node_t){.data=malloc(64),.size=17,.data_capacity=64};
        assert(b.data);
        unsigned char expected[144]={0};
        for(unsigned j=0;j<64;++j)b.data[j]=(unsigned char)(j*23+11);
        memcpy(expected,b.data,17);memcpy(expected+80,b.data+source_offset,count);
        before=allocations;
        assert(vfs_memory_backend_write(&b,80,b.data+source_offset,count,&bytes)==K_OK);
        assert(b.size==80+count && bytes==count && allocations==before+1);
        assert(!memcmp(b.data,expected,b.size));free(b.data);++cases;
    }
    // Spare-capacity sources can sit inside the hole that must be zeroed.
    // No reallocation isolates the copy/zero ordering from retired ownership.
    for(unsigned source_offset=0;source_offset<64;++source_offset)
    for(unsigned offset=18;offset<=50;offset+=16) {
        unsigned count=64-source_offset;
        if(count>64-offset)count=64-offset;
        b=(vfs_backend_node_t){.data=malloc(64),.size=17,.data_capacity=64};
        assert(b.data);unsigned char expected[64]={0},snapshot[64];
        for(unsigned j=0;j<64;++j)b.data[j]=(unsigned char)(j*23+11);
        memcpy(expected,b.data,17);memcpy(snapshot,b.data+source_offset,count);
        memcpy(expected+offset,snapshot,count);before=allocations;
        assert(vfs_memory_backend_write(&b,offset,b.data+source_offset,count,&bytes)==K_OK);
        assert(b.size==offset+count && bytes==count && allocations==before);
        assert(!memcmp(b.data,expected,b.size));free(b.data);++cases;
    }
    printf("Self-sourced writes: %u snapshot-oracle overlap/growth/hole/OOM cases passed\n",cases);
    assert(stats_calls==1 && g_vfs_memory_allocation_failure.failures>1);
    assert(g_vfs_memory_allocation_failure.required==8388609);
    assert(g_vfs_memory_allocation_failure.previous_size==10001);
    puts("4 MiB append: 1024 -> 11 allocations; truncate, holes, zero writes and OOM passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-memory-growth-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    obj = str(pathlib.Path(directory) / 'string.o')
    subprocess.run(['cc','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-fsanitize=address,undefined',
                    '-fno-tree-vectorize','-I'+str(root/'include'),
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x', 'c', '-', '-x', 'none', obj, '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    negative=code.replace('if (retired != 0) *retired = backend->data;',
                          'if (retired != 0) { kfree(backend->data); *retired = 0; }')
    assert negative!=code
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True)
    assert result.returncode!=0 and 'heap-use-after-free' in result.stderr
    print('Regression control: early buffer release reproduces ASan heap-use-after-free')
    copy = '    liteos_memmove(backend->data + offset, buffer, length);'
    clear = ('    if (offset > backend->size)\n'
             '        liteos_memset(backend->data + backend->size, 0, (size_t)(offset - backend->size));')
    assert code.count(copy)==1 and code.count(clear)==1
    negative=code.replace(copy, '').replace(clear, clear+'\n'+copy)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True)
    assert result.returncode!=0 and '!memcmp(b.data,expected,b.size)' in result.stderr
    print('Regression control: clearing the hole before copying corrupts spare-capacity source bytes')
