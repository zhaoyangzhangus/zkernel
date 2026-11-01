/* Linux-only fallback correctness test for the EXACT freestanding allocator
 * compiled with -DFM_UEFI; this is not a substitute for actual QEMU execution. */
#define _GNU_SOURCE
#include "frame_pool.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHUNKS 16
#define BATCH 1025
static uint64_t root[CHUNKS];
static uint64_t root_meta[CHUNKS] __attribute__((aligned(4096)));
static uint64_t pcp_ptes[FM_PCP_CAPACITY] __attribute__((aligned(4096)));
static uint64_t *metadata_va;
static uint64_t hwm;
static int map_page(void *ctx, size_t index, uint64_t pa) {
    (void)ctx;(void)pa;
    if (mmap((uint8_t*)metadata_va + index*4096, 4096, PROT_READ|PROT_WRITE,
             MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)==MAP_FAILED) return -1;
    return 0;
}
static void unmap_page(void *ctx, size_t index) {
    (void)ctx;
    mmap((uint8_t*)metadata_va+index*4096,4096,PROT_NONE,
         MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
}
static int store(void *ctx,uint64_t p) {(void)ctx;hwm=p;return 0;}
static void pcp_slot_changed(void *ctx,size_t slot,uint64_t va,uint64_t pte) {
    (void)ctx;(void)slot;(void)va;(void)pte;
}
static void invalidate_page(void *ctx,uint64_t va) {(void)ctx;(void)va;}
static int single_run(int reboot) {
    /* A reboot installs a fresh, empty PCP page-table window. */
    memset(pcp_ptes, 0, sizeof(pcp_ptes));
    fm_manager manager;
    fm_config c={0};
    c.root_physical_frames=root;
    c.root_count=CHUNKS;
    c.root_metadata_va=root_meta;
    c.root_metadata_entries=CHUNKS;
    c.metadata_4k_va=metadata_va;
    c.metadata_4k_pages_reserved=CHUNKS;
    c.pcp_leaf_ptes=pcp_ptes;
    c.pcp_virtual_base=UINT64_C(0x700000000000);
    c.saved_high_water_pages=hwm;
    c.hooks.map_meta_page=map_page;
    c.hooks.unmap_meta_page=unmap_page;
    c.hooks.store_high_water=store;
    c.hooks.pcp_slot_changed=pcp_slot_changed;
    c.hooks.invalidate_page=invalidate_page;
    if(fm_init(&manager,&c)!=FM_OK) return 1;
    fm_stats s;
    fm_get_stats(&manager,&s);
    if(reboot && s.chunks_owned==0) return 2;
    uint64_t out[BATCH], pa[BATCH];
    const size_t tests[]={1,511,512,1025};
    for(size_t k=0;k<4;++k){
        size_t n=tests[k];
        if(fm_alloc_4k(&manager,n,out)!=FM_OK) return 3;
        for(size_t i=0;i<n;++i) {
            pa[i]=fm_phys_4k(out[i]);
            if(!pa[i] || (pa[i]&4095) || (pa[i]&((uint64_t)2097152-1))==0) return 4;
            for(size_t j=0;j<i;++j) if(pa[i]==pa[j]) return 5;
        }
        if(fm_free_4k(&manager,n,pa)!=FM_OK) return 6;
    }
    uint64_t one,two;
    if(fm_alloc_4k(&manager,1,&one)!=FM_OK) return 7;
    uint64_t phys=fm_phys_4k(one);
    if(fm_free_4k(&manager,1,&phys)!=FM_OK) return 8;
    if(fm_alloc_4k(&manager,1,&two)!=FM_OK || one!=two) return 9;
    if(fm_free_4k(&manager,1,&phys)!=FM_OK) return 10;
    /* Direct-TOP-to-leaf-PTE path, including the 511/512 and 512/513
     * metadata / leaf-page boundaries. This host test checks the exact PTE
     * values; the UEFI test also checks REAL CR3-backed virtual aliases. */
    const size_t direct_sizes[]={1,511,512,1025};
    uint64_t leaf_ptes[BATCH]={0};
    for (size_t t=0;t<4;++t) {
        size_t n=direct_sizes[t];
        if(fm_alloc_map_4k(&manager,n,leaf_ptes)!=FM_OK) return 13;
        for(size_t i=0;i<n;++i) {
            uint64_t p=leaf_ptes[i];
            if ((p & FM_PTE_FLAGS)!=FM_PTE_FLAGS ||
                !(p & FM_PHYS_MASK_4K)) return 14;
            pa[i]=fm_phys_4k(p);
            for(size_t j=0;j<i;++j) if(pa[i]==pa[j]) return 15;
            leaf_ptes[i]=0;
        }
        if(fm_free_4k(&manager,n,pa)!=FM_OK) return 16;
    }
    /* Even non-present nonzero PTEs belong to the caller. Never overwrite. */
    leaf_ptes[1]=UINT64_C(0x4000000000000000);
    size_t old_free;
    fm_get_stats(&manager,&s);
    old_free=s.free_4k;
    if (fm_alloc_map_4k(&manager,2,leaf_ptes)!=FM_EINVAL) return 17;
    if (leaf_ptes[0]!=0 || leaf_ptes[1]!=UINT64_C(0x4000000000000000)) return 18;
    fm_get_stats(&manager,&s);
    if(s.free_4k!=old_free) return 19;
    leaf_ptes[1]=0;
    fm_get_stats(&manager,&s);
    if(s.free_4k!=s.total_4k_frames || s.total_4k_frames!=s.chunks_owned*511) return 11;
    if(s.high_water_4k_pages!=hwm) return 12;
    printf("[HOST-SMOKE] %s: chunks=%zu free=%zu HWM=%llu PASS\n",
           reboot?"restart":"first boot",s.chunks_owned,s.free_4k,
           (unsigned long long)hwm);
    fm_destroy(&manager);
    return 0;
}
int main(void) {
    for(size_t i=0;i<CHUNKS;++i) root[i]=0x2000000+(uint64_t)i*2097152;
    metadata_va=mmap(0,CHUNKS*4096,PROT_NONE,
                     MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(metadata_va==MAP_FAILED) return 1;
    int rc=single_run(0);
    if(!rc) rc=single_run(1);
    munmap(metadata_va,CHUNKS*4096);
    if(rc) fprintf(stderr,"HOST-SMOKE FAILED %d\n",rc);
    return rc;
}
