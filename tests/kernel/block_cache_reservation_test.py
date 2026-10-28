"""Unified production victim selector vs LRU oracle, including I/O failures."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/pagecache/cache.c').read_text()
body=source[source.index('static VOID copy_bytes('):]
start=body.index('static OS_BLOCK_CACHE_ENTRY *find_entry_locked(')
end=body.index('static BOOLEAN flush_entry_locked(',start)
lookup=body[start:end].replace('    for (UINT32 i = 0;',
                             '    ++lookup_calls; for (UINT32 i = 0;')
body=body[:start]+lookup+body[end:]
body=body.replace('if ((reserved_mask & (1U << i)) != 0U) continue;',
                  '++checks; if ((reserved_mask & (1U << i)) != 0U) continue;')
body=body.replace('if (entry->Valid && entry->Lba == lba) {',
                  '++write_checks; if (entry->Valid && entry->Lba == lba) {')
body=body.replace('OS_BLOCK_CACHE_ENTRY *candidate = &cache->Entries[index];',
                  '++range_checks; OS_BLOCK_CACHE_ENTRY *candidate = &cache->Entries[index];')
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/block_cache.h>
#define OS_BLOCK_CACHE_READ_AHEAD_BLOCKS 15U
static unsigned checks,diagnostics,reads,writes,fail_write;
static unsigned write_checks;
static unsigned lookup_calls;
static unsigned range_checks;
static BOOLEAN fail_read;
static void block_cache_diag(UINT32 stage,UINT64 lba,const OS_BLOCK_CACHE *cache) {
    (void)stage;(void)lba;(void)cache;++diagnostics;
}
''' + body + r'''
static OS_BLOCK_CACHE_ENTRY *oracle(OS_BLOCK_CACHE *c,BOOLEAN *used) {
    OS_BLOCK_CACHE_ENTRY *chosen=0;
    for(unsigned i=0;i<16;++i) {
        if(used[i]) continue;
        if(!c->Entries[i].Valid) return &c->Entries[i];
        if(!chosen || c->Entries[i].LastUse<chosen->LastUse) chosen=&c->Entries[i];
    }
    return chosen;
}
static BOOLEAN read_disk(VOID *ctx,UINT64 lba,UINT32 n,VOID *out) {
    (void)ctx;++reads;if(fail_read)return 0;
    for(unsigned i=0;i<n;++i) memset((UINT8 *)out+512*i,(int)((lba+i)&255),512);
    return 1;
}
static BOOLEAN write_disk(VOID *ctx,UINT64 lba,UINT32 n,const VOID *in) {
    (void)ctx;(void)lba;(void)n;(void)in;
    return ++writes!=fail_write;
}
static unsigned random_state=12345;
static unsigned random_value(void) {return random_state=random_state*1664525U+1013904223U;}
int main(void) {
    OS_BLOCK_CACHE cache={0};
    for(unsigned trial=0;trial<10000;++trial) {
        BOOLEAN reserved[16];
        UINT32 reserved_mask=0;
        for(unsigned i=0;i<16;++i) {
            cache.Entries[i].Valid=(random_value()>>16)&1;
            cache.Entries[i].LastUse=(random_value()>>16)%8;
            reserved[i]=trial==0?1:((random_value()>>16)&1);
            if(reserved[i]) reserved_mask |= 1U << i;
        }
        checks=0;
        OS_BLOCK_CACHE_ENTRY *actual=select_entry_locked(&cache,reserved_mask);
        assert(actual==oracle(&cache,reserved));
        assert(checks<=16);
        BOOLEAN none[16]={0};
        assert(select_entry_locked(&cache,0U)==oracle(&cache,none));
        for(unsigned i=0;i<16;++i)cache.Entries[i].Lba=(random_value()>>16)%32;
        UINT64 lba=(random_value()>>16)%32;
        OS_BLOCK_CACHE_ENTRY *expected=0;
        for(unsigned i=0;i<16;++i)
            if(cache.Entries[i].Valid && cache.Entries[i].Lba==lba) {expected=&cache.Entries[i];break;}
        BOOLEAN expected_hit=expected!=0,hit=0;
        if(!expected)expected=oracle(&cache,none);
        write_checks=0;
        assert(find_write_entry_locked(&cache,lba,&hit)==expected && hit==expected_hit);
        assert(write_checks<=16);
    }
    OS_BLOCK_DEVICE device={.Registered=1,.BlockSize=512,.BlockCount=1024,
                               .Read=read_disk,.Write=write_disk};
    for(unsigned pattern=0;pattern<4096;++pattern) {
        memset(&cache,0,sizeof(cache));assert(block_cache_init(&cache,&device));
        cache.Clock=100;unsigned hits=0,runs=0;BOOLEAN in_miss=0;
        for(unsigned i=0;i<12;++i) {
            BOOLEAN hit=(pattern&(1U<<i))!=0;
            cache.Entries[i].Valid=cache.Entries[i].Dirty=hit;
            cache.Entries[i].Lba=64+i;cache.Entries[i].LastUse=50+i;
            memset(cache.Entries[i].Data,(int)(0x80+i),512);
            if(hit) {++hits;in_miss=0;}
            else if(!in_miss) {++runs;in_miss=1;}
        }
        UINT8 output[12*512+32];memset(output,0x75,sizeof(output));
        reads=writes=lookup_calls=range_checks=0;fail_read=0;fail_write=0;
        assert(block_cache_read_range(&cache,64,12,output+16));
        assert(!lookup_calls && range_checks==16 && reads==runs && !writes && cache.Clock==100+hits);
        unsigned clock=100;
        for(unsigned i=0;i<12;++i) {
            BOOLEAN hit=(pattern&(1U<<i))!=0;
            for(unsigned byte=0;byte<512;++byte)
                assert(output[16+i*512+byte]==(hit?0x80+i:64+i));
            assert(cache.Entries[i].Valid==hit && cache.Entries[i].Dirty==hit);
            assert(cache.Entries[i].Lba==64+i);
            assert(cache.Entries[i].LastUse==(hit?++clock:50+i));
        }
        for(unsigned i=0;i<16;++i)assert(output[i]==0x75 && output[12*512+16+i]==0x75);
        if(runs) {
            unsigned first_miss=0;while(pattern&(1U<<first_miss))++first_miss;
            UINT64 before=cache.Clock;
            memset(output,0x75,sizeof(output));reads=0;fail_read=1;
            assert(!block_cache_read_range(&cache,64,12,output+16));
            assert(reads==1 && !cache.Lock && cache.Clock==before+first_miss);
            for(unsigned i=0;i<12;++i)for(unsigned byte=0;byte<512;++byte)
                assert(output[16+i*512+byte]==(i<first_miss?0x80+i:0x75));
            for(unsigned i=0;i<16;++i)assert(output[i]==0x75 && output[12*512+16+i]==0x75);
            fail_read=0;
        }
    }
    /* Maximum extent, high LBA and first-slot duplicate semantics. */
    for(unsigned high=0;high<2;++high) {
        memset(&cache,0,sizeof(cache));assert(block_cache_init(&cache,&device));
        UINT64 base=high?UINT64_MAX-128U:64U;
        device.BlockCount=high?UINT64_MAX:1024U;
        for(unsigned i=0;i<16;++i) {
            cache.Entries[i].Valid=cache.Entries[i].Dirty=1;
            cache.Entries[i].Lba=base+i*7U;
            memset(cache.Entries[i].Data,0xa0+i,512);
        }
        cache.Entries[15].Lba=base;
        UINT8 output[128*512+32];memset(output,0x75,sizeof(output));
        range_checks=lookup_calls=reads=writes=0;
        assert(block_cache_read_range(&cache,base,128,output+16));
        assert(range_checks==16&&!lookup_calls&&!writes);
        for(unsigned i=0;i<128;++i) {
            unsigned expected=(unsigned)((base+i)&255U);
            for(unsigned slot=0;slot<16;++slot)if(cache.Entries[slot].Lba==base+i) {
                expected=0xa0+slot;break;
            }
            for(unsigned byte=0;byte<512;++byte)assert(output[16+i*512+byte]==expected);
        }
        for(unsigned i=0;i<16;++i)assert(output[i]==0x75&&output[16+128*512+i]==0x75);
    }
    device.BlockCount=1024U;
    memset(&cache,0,sizeof(cache));assert(block_cache_init(&cache,&device));
    cache.Entries[0].Valid=cache.Entries[0].Dirty=1;
    cache.Entries[0].Lba=64;memset(cache.Entries[0].Data,0xa5,512);
    UINT8 single_output[512];range_checks=lookup_calls=reads=0;
    assert(block_cache_read_range(&cache,64,1,single_output));
    assert(!range_checks&&lookup_calls==1&&!reads);
    for(unsigned i=0;i<512;++i)assert(single_output[i]==0xa5);
    /* Write hit/eviction: failed dirty flush must preserve the old entry. */
    for(unsigned failure=0;failure<2;++failure) {
        memset(&cache,0,sizeof(cache));assert(block_cache_init(&cache,&device));
        cache.Clock=100;
        for(unsigned i=0;i<16;++i) {
            cache.Entries[i].Valid=cache.Entries[i].Dirty=1;
            cache.Entries[i].Lba=i;cache.Entries[i].LastUse=i+1;
            memset(cache.Entries[i].Data,(int)i,512);
        }
        UINT8 input[512];memset(input,0xa5,sizeof(input));
        fail_read=0;reads=writes=0;fail_write=failure?1:0;
        write_checks=0;
        assert(block_cache_write(&cache,100,input)==!failure);
        assert(write_checks==16);
        assert(writes==1 && !reads);
        if(failure) {
            assert(cache.Clock==100 && cache.Entries[0].Lba==0 && cache.Entries[0].Dirty);
            for(unsigned i=0;i<512;++i) assert(cache.Entries[0].Data[i]==0);
        } else {
            assert(cache.Clock==101 && cache.Entries[0].Lba==100 && cache.Entries[0].Dirty);
            assert(!memcmp(input,cache.Entries[0].Data,512));
        }
        fail_write=0;assert(block_cache_write(&cache,100,input));
        unsigned before=writes;
        assert(block_cache_write(&cache,100,input) && writes==before);
        assert(block_cache_destroy(&cache));
    }
    for(unsigned failure=0;failure<3;++failure) {
        memset(&cache,0,sizeof(cache));assert(block_cache_init(&cache,&device));
        cache.Clock=100;
        for(unsigned i=0;i<16;++i) {
            cache.Entries[i].Valid=cache.Entries[i].Dirty=1;
            cache.Entries[i].Lba=i;cache.Entries[i].LastUse=i+1;
        }
        reads=writes=diagnostics=0;fail_read=failure==2;fail_write=failure==1?2:0;
        UINT8 out[512];
        assert(block_cache_read(&cache,100,out)==(failure==0));
        if(failure==1) {
            assert(!reads && writes==2 && diagnostics==1);
            for(unsigned i=0;i<16;++i) {
                assert(cache.Entries[i].Valid==(i>=8));
                assert(cache.Entries[i].Dirty==(i>=8));
            }
        } else if(failure==2) {
            assert(reads==1 && writes==2 && diagnostics==1);
            for(unsigned i=0;i<15;++i) assert(!cache.Entries[i].Valid && !cache.Entries[i].Dirty);
            assert(cache.Entries[15].Valid && !cache.Entries[15].Dirty);
        } else {
            assert(reads==1 && writes==2 && !diagnostics && out[0]==100);
            for(unsigned i=0;i<15;++i) assert(cache.Entries[i].Valid &&
                !cache.Entries[i].Dirty && cache.Entries[i].Lba==100+i);
            assert(cache.Entries[15].Valid && !cache.Entries[15].Dirty && cache.Entries[15].Lba==15);
        }
        fail_read=0;fail_write=0;
        assert(block_cache_read(&cache,100,out) && out[0]==100);
        assert(block_cache_destroy(&cache));
    }
    puts("Cache victim: 10000 masked/unmasked LRU states; <=16 checks; write-hit/eviction, flush/read failure and retry passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cache-reservation-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',str(path/'test.c'),
                    str(root/'kernel/block/bio.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
print('Range index: 4096 hit patterns, 128-sector/high-LBA/duplicate cases and single-sector fast path passed; bulk metadata scans bounded to 16 slots')
