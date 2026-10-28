"""真实 CPU 采样索引与旧线性身份匹配对照，覆盖标识重用和重复记录。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'user/desktop/taskmgr/main.c').read_text()
entry=source[source.index('typedef struct taskmgr_entry {'):source.index('} taskmgr_entry_t;')+len('} taskmgr_entry_t;')]
previous=source[source.index('typedef struct taskmgr_previous {'):source.index('static uint64_t g_sample_time;')]
functions=source[source.index('static uint32_t previous_hash('):source.index('static void sample_resources(')]
functions=functions.replace('while (g_previous_index[slot]) {','while (g_previous_index[slot]) { ++probes;')
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/all.h>
#define TASKMGR_MAX_ENTRIES 768U
'''+entry+r'''
static taskmgr_entry_t g_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_count;
static unsigned probes,cases;
'''+previous+functions+r'''
static const taskmgr_previous_t *reference(const taskmgr_entry_t *entry) {
    for(unsigned i=0;i<g_previous_count;++i) {
        const taskmgr_previous_t *old=g_previous+i;
        if(old->id==entry->id&&old->parent_id==entry->parent_id&&old->thread==entry->thread&&
            old->created==entry->resources.create_time_ns)return old;
    }
    return 0;
}
static void verify(taskmgr_entry_t entry) {
    assert(previous_find(&entry)==reference(&entry));++cases;
}
int main(void) {
    uint32_t random=7;
    for(unsigned layout=0;layout<200;++layout) {
        g_entry_count=layout*37U%769U;
        for(unsigned i=0;i<g_entry_count;++i) {
            random=random*1664525U+1013904223U;
            g_entries[i]=(taskmgr_entry_t){.id=(uint64_t)random<<32U|i,
                .parent_id=i%17,.thread=i%3!=0,.expanded=i%5==0};
            g_entries[i].resources.create_time_ns=random;
            g_entries[i].resources.runtime_ns=i*1000U;
            if(i&&i%11==0)g_entries[i]=g_entries[i-1];
        }
        previous_snapshot();assert(g_previous_count==g_entry_count);
        for(unsigned i=0;i<g_entry_count;++i) {
            const taskmgr_entry_t *entry=g_entries+i;
            assert(g_previous[i].runtime==entry->resources.runtime_ns&&g_previous[i].expanded==entry->expanded);
            verify(*entry);
            taskmgr_entry_t changed=*entry;
            changed.resources.runtime_ns+=10000;changed.expanded=!changed.expanded;
            memset(changed.name,'x',sizeof(changed.name));verify(changed);
            changed=*entry;++changed.resources.create_time_ns;verify(changed);
            changed=*entry;changed.thread=!changed.thread;verify(changed);
            changed=*entry;++changed.parent_id;verify(changed);
        }
    }
    g_entry_count=768;
    for(unsigned i=0;i<768;++i) {
        g_entries[i]=(taskmgr_entry_t){.id=i+1,.parent_id=i/3,.thread=i%2};
        g_entries[i].resources.create_time_ns=1000000000ULL+i*100U;
    }
    previous_snapshot();probes=0;
    for(unsigned i=0;i<768;++i)verify(g_entries[i]);
    assert(probes<768U*16U);
    printf("Full CPU snapshot: %u index probes instead of 295296 linear candidates; storage %zu -> %zu bytes\n",
        probes,768*sizeof(taskmgr_entry_t),sizeof(g_previous)+sizeof(g_previous_index));
    g_entry_count=0;previous_snapshot();assert(!g_previous_count);
    for(unsigned i=0;i<768;++i)assert(!previous_find(g_entries+i));
    printf("Task sample index: %u old-rule identity oracles, PID/TID reuse, duplicates, metadata changes, full/empty replacement PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='taskmgr-sample-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
