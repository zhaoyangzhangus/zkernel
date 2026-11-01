/* zkernel TOP-cursor Frame Pool: x86-64 UEFI/QEMU bare-metal test.
 * No POSIX, no libc, no hosted allocator. Intended for OVMF and QEMU.
 * A physical 2 MiB frame is transferred out of root pool, its first 4 KiB
 * becomes metadata, mapped at a contiguous HIGH virtual address, and
 * the other 511 4 KiB physical pages are encoded as ready-to-use PTE words.
 */
#include "uefi_min.h"
#include "frame_pool.h"

#define ROOT_COUNT 32u
#define META_PAGES 32u
#define PHYS_ADDR_MASK UINT64_C(0x000ffffffffff000)
#define META_VA_BASE UINT64_C(0xffff900000000000)
#define MAP_VA_BASE (META_VA_BASE + FM_2M_SIZE)
#define MAP_PT_PAGES 3u
#define PCP_PT_PAGES 2u
#define PCP_VA_BASE (META_VA_BASE + (1u + MAP_PT_PAGES) * FM_2M_SIZE)
#define N_TRIALS 7
#define MAX_BATCH 1025u

/* Force a PE base relocation so OVMF may load the image at any address. */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table);
static void * volatile g_reloc_anchor = (void *)&efi_main;
static EFI_SERVICES *g_bs;
static EFI_SERVICES *g_rt;
static uint64_t g_firmware_cr3;
static uint64_t g_private_cr3;
static uint64_t *g_metadata_pt;
/* Three contiguous PT pages in identity-mapped loader RAM. Their PTEs map
 * three consecutive 2MiB virtual regions from MAP_VA_BASE onward. */
static uint64_t *g_map_ptes;
static uint64_t *g_pcp_ptes;
static fm_manager g_manager;
static uint64_t g_root_pa[ROOT_COUNT];
static uint64_t g_root_meta[ROOT_COUNT] __attribute__((aligned(4096)));
static uint64_t g_output[MAX_BATCH];
static uint64_t g_pa[MAX_BATCH];
static volatile uint64_t g_checksum;
static uint64_t g_tsc_hz; /* calibrated guest TSC frequency, 0 if unknown */
static int g_failed;
static unsigned g_passed;
static EFI_GUID g_guid = {0x2e7ab19c,0xab84,0x498d,{0x9a,0xb0,0x11,0x63,0x92,0xa1,0x58,0x27}};
static const CHAR16 g_var_name[] = {
    'Z','k','e','r','n','e','l','F','r','a','m','e','H','W','M',0
};
static const CHAR16 g_legacy_var_name[] = {
    'L','i','t','e','O','s','F','r','a','m','e','H','W','M',0
};

typedef struct {
    uint64_t signature;
    uint64_t high_water_pages;
    uint64_t complement;
} persistent_record;
#define HWM_SIGNATURE UINT64_C(0x5a4b464d48303130)
#define LEGACY_HWM_SIGNATURE UINT64_C(0x4c4f53464d483031)

/* Portable freestanding functions, also used by frame_pool.c. */
void *memset(void *dst, int value, size_t count) {
    uint8_t *p = dst;
    while (count--) *p++ = (uint8_t)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t count) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (count--) *d++ = *s++;
    return dst;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void serial_init(void) {
    outb(0x3f9, 0x00); /* interrupts off */
    outb(0x3fb, 0x80); /* DLAB */
    outb(0x3f8, 0x01); /* divisor=1, 115200 baud */
    outb(0x3f9, 0x00);
    outb(0x3fb, 0x03); /* 8N1 */
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
}
static void serial_char(char c) {
    /* Bounded polling: avoids hanging on real machines without COM1. */
    for (unsigned i=0; i<20000; ++i) {
        if (inb(0x3fd) & 0x20u) {
            outb(0x3f8, (uint8_t)c);
            return;
        }
    }
}
/* One output channel only. OVMF may mirror ConOut to COM1, so calling
 * both OutputString and serial_char duplicates every line on -serial stdio. */
static void print(const char *s) {
    while (*s) {
        if (*s == '\n') serial_char('\r');
        serial_char(*s++);
    }
}
static void print_u64(uint64_t x) {
    char s[32];
    unsigned n=0;
    if (!x) { print("0"); return; }
    while (x) { s[n++] = '0' + (char)(x%10); x/=10; }
    char out[32];
    unsigned k=0;
    while (n) out[k++]=s[--n];
    out[k]=0;
    print(out);
}
static void print_hex_u64(uint64_t x) {
    static const char digits[] = "0123456789abcdef";
    char out[16];
    for (unsigned i = 0; i < 16; ++i)
        out[15 - i] = digits[(x >> (i * 4)) & 0xfu];
    for (unsigned i = 0; i < 16; ++i) serial_char(out[i]);
}
static void check(int cond, const char *what) {
    if (cond) {
        ++g_passed;
        return;
    }
    print("[FAIL] ");
    print(what);
    print("\n");
    g_failed = 1;
}
static EFI_STATUS alloc_pages(size_t pages, EFI_PHYSICAL_ADDRESS *out) {
    EFI_ALLOC_PAGES fn=(EFI_ALLOC_PAGES)g_bs->slot[BS_ALLOCATE_PAGES];
    return fn(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, out);
}

/* OVMF may protect its own paging-structure pages as read-only, including
 * the active PML4 at CR3. NEVER write to the firmware PML4 in place.
 *
 * Reserve nine new loader pages (PML4, PDPT, PD, metadata PT, 3 target PTs,
 * and 2 PCP PTs), clone the firmware
 * top-level page table, and install an isolated subtree for the metadata VA.
 * For this QEMU test, META_PAGES <= 512, all metadata VAs are within one PT.
 * The mapped-frame test uses three additional adjacent leaf PT pages.
 * All paging-structure pages must be identity-mapped and below 4 GiB here;
 * a production kernel should use its own direct-map/page-table allocator.
 *
 * This is a UEFI test technique, not a generic way of taking ownership of
 * firmware page tables across ExitBootServices or other firmware platforms.
 */
static uint64_t read_cr3_raw(void) {
    uint64_t x;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(x));
    return x;
}
static void write_cr3(uint64_t x) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(x) : "memory");
}
static void invlpg(uint64_t v) {
    __asm__ __volatile__("invlpg (%0)" :: "r"((uintptr_t)v) : "memory");
}
static int init_private_metadata_page_tables(void) {
    /* PML4 index 288 corresponds to 0xffff900000000000. */
    const size_t pml4_index = (size_t)((META_VA_BASE >> 39) & 511u);
    if (META_PAGES > 512 || MAP_PT_PAGES * 512u < MAX_BATCH ||
        PCP_PT_PAGES * 512u != FM_PCP_CAPACITY ||
        (META_VA_BASE & (FM_2M_SIZE-1)) != 0)
        return -1;

    g_firmware_cr3 = read_cr3_raw();
    uint64_t *firmware_pml4 =
        (uint64_t *)(uintptr_t)(g_firmware_cr3 & PHYS_ADDR_MASK);
    if (firmware_pml4[pml4_index] & 1u) {
        print("[FAIL] metadata VA conflicts with firmware PML4 mapping\n");
        return -1;
    }

    /* AllocateMaxAddress ensures page-table addresses are identity mapped
     * under this QEMU 512MiB firmware configuration. */
    EFI_PHYSICAL_ADDRESS pa = UINT64_C(0xffffffff);
    EFI_ALLOC_PAGES fn=(EFI_ALLOC_PAGES)g_bs->slot[BS_ALLOCATE_PAGES];
    if (EFI_ERROR(fn(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                     4 + MAP_PT_PAGES + PCP_PT_PAGES, &pa)) ||
        pa >= UINT64_C(0x100000000) -
              (4 + MAP_PT_PAGES + PCP_PT_PAGES) * 4096)
        return -1;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pa;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pa + 4096);
    uint64_t *pd   = (uint64_t *)(uintptr_t)(pa + 8192);
    uint64_t *pt   = (uint64_t *)(uintptr_t)(pa + 12288);
    uint64_t *map_ptes = (uint64_t *)(uintptr_t)(pa + 16384);
    uint64_t *pcp_ptes = (uint64_t *)(uintptr_t)
        (pa + (4 + MAP_PT_PAGES) * 4096);
    memcpy(pml4, firmware_pml4, 4096);
    memset(pdpt, 0, 4096);
    memset(pd, 0, 4096);
    memset(pt, 0, 4096);
    memset(map_ptes, 0, MAP_PT_PAGES * 4096);
    memset(pcp_ptes, 0, PCP_PT_PAGES * 4096);
    /* This high VA has PDPT/PD/PT indices all zero. */
    pml4[pml4_index] = (pa + 4096) | UINT64_C(3);
    pdpt[0] = (pa + 8192) | UINT64_C(3);
    pd[0] = (pa + 12288) | UINT64_C(3);
    /* Region 0 is metadata; 1..3 are target mappings; 4..5 are PCP VA. */
    for (unsigned i = 0; i < MAP_PT_PAGES; ++i)
        pd[1 + i] = (pa + 16384 + (uint64_t)i * 4096) | UINT64_C(3);
    for (unsigned i = 0; i < PCP_PT_PAGES; ++i)
        pd[1 + MAP_PT_PAGES + i] =
            (pa + (4 + MAP_PT_PAGES + i) * 4096) | UINT64_C(3);

    g_metadata_pt = pt;
    g_map_ptes = map_ptes;
    g_pcp_ptes = pcp_ptes;
    g_private_cr3 = pa;
    write_cr3(pa);
    if ((read_cr3_raw() & PHYS_ADDR_MASK) != g_private_cr3)
        return -1;
    return 0;
}
static int map_meta(void *ctx, size_t idx, uint64_t pa) {
    (void)ctx;
    if (!g_metadata_pt || idx>=META_PAGES) return -1;
    if (pa >= UINT64_C(0x100000000) || (pa & 4095u)) return -1;
    if ((read_cr3_raw() & PHYS_ADDR_MASK) != g_private_cr3) return -1;
    uint64_t va=META_VA_BASE+(uint64_t)idx*4096;
    if (g_metadata_pt[idx] & 1u) return -1;
    g_metadata_pt[idx] = pa | UINT64_C(3);
    invlpg(va);
    return 0;
}
static void unmap_meta(void *ctx, size_t idx) {
    (void)ctx;
    if (!g_metadata_pt || idx>=META_PAGES) return;
    if ((read_cr3_raw() & PHYS_ADDR_MASK) != g_private_cr3) return;
    uint64_t va=META_VA_BASE+(uint64_t)idx*4096;
    g_metadata_pt[idx] = 0;
    invlpg(va);
}
static void pcp_slot_changed(void *ctx, size_t slot, uint64_t va, uint64_t pte) {
    (void)ctx;
    (void)pte;
    if (!g_pcp_ptes || slot >= FM_PCP_CAPACITY ||
        va != PCP_VA_BASE + (uint64_t)slot * FM_4K_SIZE)
        return;
    invlpg(va);
}
static void invalidate_page(void *ctx, uint64_t va) {
    (void)ctx;
    invlpg(va);
}

static uint64_t hwm_read_one(const CHAR16 *name, uint64_t signature) {
    EFI_GET_VARIABLE fn=(EFI_GET_VARIABLE)g_rt->slot[RT_GET_VARIABLE];
    persistent_record p={0};
    size_t sz=sizeof(p);
    uint32_t flags=0;
    EFI_STATUS e=fn(name,&g_guid,&flags,&sz,&p);
    if (EFI_ERROR(e) || sz!=sizeof(p) || p.signature!=signature ||
        p.complement!=~p.high_water_pages ||
        (p.high_water_pages%511)!=0) return 0;
    return p.high_water_pages;
}
static int hwm_store(void *ctx, uint64_t pages);
static uint64_t hwm_read(void) {
    uint64_t current=hwm_read_one(g_var_name,HWM_SIGNATURE);
    uint64_t legacy=hwm_read_one(g_legacy_var_name,LEGACY_HWM_SIGNATURE);
    /* Preserve the maximum across the project rename on an existing OVMF
     * VARS image, without ever reducing the persisted high-water mark. */
    if (legacy > current) {
        if (hwm_store(0,legacy)==0) return legacy;
        return legacy; /* use the safely read record even if migration fails */
    }
    return current;
}
static int hwm_store(void *ctx, uint64_t pages) {
    (void)ctx;
    persistent_record p={HWM_SIGNATURE,pages,~pages};
    EFI_SET_VARIABLE fn=(EFI_SET_VARIABLE)g_rt->slot[RT_SET_VARIABLE];
    EFI_STATUS e=fn(g_var_name,&g_guid,EFI_VARIABLE_NV_BS_RT,sizeof(p),&p);
    return EFI_ERROR(e) ? -1 : 0;
}

static uint64_t tsc_begin(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\nrdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi<<32)|lo;
}
static uint64_t tsc_end(void) {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp\nlfence" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    return ((uint64_t)hi<<32)|lo;
}
static void sort_u64(uint64_t *x, unsigned n) {
    for (unsigned i=1;i<n;++i) {
        uint64_t v=x[i];
        unsigned j=i;
        while (j>0 && x[j-1]>v) {x[j]=x[j-1];--j;}
        x[j]=v;
    }
}
/* Stall is specified in microseconds by UEFI Boot Services. Three
 * independent 100 ms samples avoid assuming that a TSC tick is one ns.
 * The conversion is only an estimate: virtualization, timer resolution,
 * and TSC scaling can affect accuracy. */
static void calibrate_tsc(void) {
    EFI_STALL stall = (EFI_STALL)g_bs->slot[BS_STALL];
    if (!stall) {
        print("[bench] TSC calibration unavailable (no EFI Stall); ns omitted\n");
        return;
    }
    uint64_t samples[3];
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t start = tsc_begin();
        EFI_STATUS status = stall(100000u);
        uint64_t end = tsc_end();
        if (EFI_ERROR(status) || end <= start || (end - start) > UINT64_MAX / 10u) {
            print("[bench] TSC calibration failed; ns omitted\n");
            return;
        }
        samples[i] = (end - start) * 10u;
    }
    sort_u64(samples, 3);
    if (samples[1] < UINT64_C(1000000) || samples[1] > UINT64_C(100000000000)) {
        print("[bench] TSC calibration out of range; ns omitted\n");
        return;
    }
    g_tsc_hz = samples[1];
    print("[bench] estimated TSC=");
    print_u64((g_tsc_hz + 500000u) / 1000000u);
    print(" MHz (EFI Stall, median 3 x 100 ms); ns values are estimates\n");
}

static void print_fixed_2(uint64_t hundredths) {
    print_u64(hundredths / 100u);
    print(".");
    if (hundredths % 100u < 10u) print("0");
    print_u64(hundredths % 100u);
}

/* Rounded to hundredths of a nanosecond. Timed rounds are small enough
 * (well below 1e8 cycles) that the 64-bit fixed-point product cannot wrap. */
static uint64_t cycles_to_ns_x100(uint64_t cycles, uint64_t pages) {
    uint64_t denominator = g_tsc_hz * pages;
    return (cycles * UINT64_C(100000000000) + denominator / 2u) / denominator;
}
static void print_time_estimate(uint64_t cycles, unsigned pages) {
    if (!g_tsc_hz) return;
    print(", ~");
    print_fixed_2(cycles_to_ns_x100(cycles, 1));
    print(" ns/round, ~");
    print_fixed_2(cycles_to_ns_x100(cycles, pages));
    print(" ns/page");
}

static void test_correctness(void) {
    uint64_t e, f, q;
    check(fm_alloc_4k(&g_manager,1,&e)==FM_OK,"single-page allocate");
    if (g_failed) return;
    uint64_t pa=fm_phys_4k(e);
    check((pa & 4095)==0 && (pa&((uint64_t)2097152-1))!=0,
          "4K phys aligned and metadata page excluded");
    check(fm_free_4k(&g_manager,1,&pa)==FM_OK,"single-page free");
    check(fm_alloc_4k(&g_manager,1,&f)==FM_OK && f==e,
          "TOP cursor LIFO returns same PTE");
    check(fm_free_4k(&g_manager,1,&pa)==FM_OK,"LIFO free restore");

    const size_t cases[]={510,511,512,1025};
    for (size_t t=0;t<4;++t) {
        size_t count=cases[t];
        int rc=fm_alloc_4k(&g_manager,count,g_output);
        if (rc!=FM_OK) { check(0,"cross-boundary batch allocation"); return; }
        int valid=1;
        for (size_t i=0;i<count;++i) {
            g_pa[i]=fm_phys_4k(g_output[i]);
            if ((g_pa[i]&4095)!=0 || (g_pa[i]&((uint64_t)2097152-1))==0)
                valid=0;
            for (size_t j=0;j<i;++j) {
                if (g_pa[i]==g_pa[j]) valid=0;
            }
        }
        check(valid,"batch unique frames, never metadata");
        check(fm_free_4k(&g_manager,count,g_pa)==FM_OK,
              "batch free and TOP restore");
    }
    uint64_t meta0=fm_4k_metadata_phys(&g_manager,0);
    uint64_t *virtual_metadata=(uint64_t *)(uintptr_t)META_VA_BASE;
    check(meta0!=UINT64_MAX && fm_phys_4k(virtual_metadata[0])==meta0,
          "real 4K metadata page mapped at high virtual address");
    if (meta0!=UINT64_MAX) {
        uint64_t *physical_alias=(uint64_t *)(uintptr_t)meta0;
        check(physical_alias[0]==virtual_metadata[0],
              "physical metadata and virtual metadata alias agree");
    }
    fm_stats stats;
    fm_get_stats(&g_manager,&stats);
    check(stats.total_4k_frames==stats.chunks_owned*511,
          "each 2MiB transfer yields precisely 511 free frames");
    check(stats.total_4k_frames==stats.free_4k,
          "after testing, all 4K frames are free");
    check(stats.high_water_4k_pages>=stats.total_4k_frames,
          "persistent high-water mark is monotonic");

    /* Also test 2MiB TOP LIFO and the 2MiB root no longer owns child chunks. */
    check(fm_alloc_2m(&g_manager,1,&q)==FM_OK,"root 2MiB TOP allocate");
    if (!g_failed) {
        uint64_t root_pa=fm_phys_2m(q);
        check(fm_free_2m(&g_manager,1,&root_pa)==FM_OK,
              "root 2MiB free");
    }
}
/* The PCP itself is a 4 MiB virtual mapping whose leaf PTEs are the cache.
 * Free maps a frame at its PCP VA; allocation clears that PCP mapping. */
static void test_pcp_virtual_mapping(void) {
    uint64_t entry;
    check(fm_alloc_4k(&g_manager, 1, &entry) == FM_OK,
          "PCP virtual test allocate one frame");
    if (g_failed) return;
    uint64_t pa = fm_phys_4k(entry);
    check(fm_free_4k(&g_manager, 1, &pa) == FM_OK,
          "PCP virtual test cache frame");
    size_t slot = (g_manager.pcp.head + g_manager.pcp.count - 1) % FM_PCP_CAPACITY;
    check(g_pcp_ptes[slot] == entry,
          "PCP leaf PTE directly stores cached frame");
    volatile uint64_t *pcp_va = (volatile uint64_t *)(uintptr_t)
        (PCP_VA_BASE + slot * FM_4K_SIZE);
    volatile uint64_t *physical_alias = (volatile uint64_t *)(uintptr_t)pa;
    uint64_t old = *physical_alias;
    uint64_t pattern = UINT64_C(0x9ac5f007afebc012) ^ pa;
    *pcp_va = pattern;
    check(*physical_alias == pattern,
          "PCP virtual slot aliases cached physical frame");
    *pcp_va = old;
    check(*physical_alias == old, "PCP virtual alias restores frame contents");
    uint64_t again;
    check(fm_alloc_4k(&g_manager, 1, &again) == FM_OK && again == entry,
          "PCP allocation returns same PTE");
    check(g_pcp_ptes[slot] == 0,
          "PCP allocation clears cached leaf PTE");
    check(fm_free_4k(&g_manager, 1, &pa) == FM_OK,
          "PCP virtual test restores cached frame");
}

/* PCP transfers exactly 512 usable frames, crossing the 511-entry
 * metadata boundary. The main stack is not used for one-page requests. */
static void test_pcp(void) {
    fm_stats before, current;
    fm_get_stats(&g_manager, &before);
    check(before.backed_4k_meta_pages == before.chunks_owned &&
          before.backed_4k_meta_end_va == META_VA_BASE +
              before.chunks_owned * FM_4K_SIZE,
          "metadata physical boundary matches mapped pages");
    check(before.pcp_free_4k < FM_PCP_CAPACITY &&
          before.free_4k == before.main_free_4k + before.pcp_free_4k,
          "PCP occupancy and pool total consistent");
    if (g_failed) return;
    static uint64_t saved[FM_PCP_CAPACITY];
    unsigned old_cached = (unsigned)before.pcp_free_4k;
    if (old_cached) {
        check(fm_alloc_4k(&g_manager,old_cached,g_output)==FM_OK,
              "empty PCP before controlled handoff test");
        if (g_failed) return;
        for (unsigned i=0;i<old_cached;++i) saved[i]=fm_phys_4k(g_output[i]);
    }
    fm_get_stats(&g_manager,&before);
    check(before.pcp_free_4k==0,"controlled PCP starts empty");
    check(fm_alloc_4k(&g_manager,512,g_output)==FM_OK,
          "one global->PCP transfer is exactly 512 frames");
    if (g_failed) return;
    for (unsigned i=0;i<512;++i)g_pa[i]=fm_phys_4k(g_output[i]);
    fm_get_stats(&g_manager,&current);
    check(current.pcp_refills==before.pcp_refills+1 &&
          current.main_free_4k+512<=before.main_free_4k+1022 &&
          current.pcp_free_4k==0,
          "PCP refilled exactly 512 usable frames");
    check(fm_free_4k(&g_manager,511,g_pa)==FM_OK,
          "511 freed pages stay in PCP");
    fm_get_stats(&g_manager,&current);
    check(current.pcp_free_4k==511,"partial PCP stays local");
    check(fm_pcp_drain_4k(&g_manager)==FM_OK,
          "partial PCP drain is no-op");
    fm_get_stats(&g_manager,&current);
    check(current.pcp_free_4k==511,"no partial global handoff");
    check(fm_free_4k(&g_manager,1,&g_pa[511])==FM_OK,
          "512th freed page available for whole drain");
    check(fm_pcp_drain_4k(&g_manager)==FM_OK,
          "explicit 512-frame PCP drain");
    fm_get_stats(&g_manager,&current);
    check(current.pcp_free_4k==0 && current.pcp_drains==before.pcp_drains+1,
          "PCP drained exactly 512 frames");
    check(fm_alloc_4k(&g_manager,512,g_output)==FM_OK,
          "global 512 refill recovers drained frames");
    for (unsigned i=0;i<512;++i)
        check(fm_phys_4k(g_output[i])==g_pa[511-i],
              "512-frame PCP handoff preserves LIFO");
    check(fm_free_4k(&g_manager,512,g_pa)==FM_OK,
          "restore controlled 512-frame test");
    if (old_cached) check(fm_free_4k(&g_manager,old_cached,saved)==FM_OK,
                          "restore previous PCP cached frames");
    fm_get_stats(&g_manager,&current);
    check(current.free_4k==before.free_4k+old_cached &&
          current.backed_4k_meta_pages==current.chunks_owned,
          "PCP handoff test preserves total free pages and mapped boundary");
}

/* These page tables are wired into the private CR3 at MAP_VA_BASE.
 * This test has exactly one CPU, so local INVLPG is sufficient. On zkernel
 * SMP, PTE removal must be followed by a cross-CPU shootdown BEFORE the
 * physical frame is released to the pool. */
static int unmap_return(size_t n) {
    if (n > MAX_BATCH) return FM_EINVAL;
    return fm_free_unmap_4k(&g_manager, n, g_map_ptes, MAP_VA_BASE);
}

static int test_high_va_alias(size_t i) {
    if (!(g_map_ptes[i] & FM_X86_P)) return 0;
    uint64_t pa = fm_phys_4k(g_map_ptes[i]);
    volatile uint64_t *low = (volatile uint64_t *)(uintptr_t)pa;
    volatile uint64_t *high =
        (volatile uint64_t *)(uintptr_t)(MAP_VA_BASE + i * FM_4K_SIZE);
    uint64_t old = *low;
    uint64_t pattern = UINT64_C(0x9ac5f007afebc012) ^ pa ^ (uint64_t)i;
    *high = pattern;
    int ok = (*low == pattern && *high == pattern);
    *high = old;
    return ok && *low == old;
}

static void test_direct_map(void) {
    const size_t sizes[] = {1, 511, 512, 1025};
    fm_stats before, after;
    for (unsigned k = 0; k < sizeof(sizes)/sizeof(sizes[0]); ++k) {
        size_t n = sizes[k];
        fm_get_stats(&g_manager, &before);
        check(fm_alloc_map_4k_at(&g_manager,n,g_map_ptes,MAP_VA_BASE)==FM_OK,
              "direct PCP-PTE-to-target-PTE allocation");
        if (g_failed) return;
        fm_get_stats(&g_manager, &after);
        check(before.free_4k >= n && after.free_4k == before.free_4k-n,
              "direct map only consumes expected frame count");
        int ok=1;
        for (size_t i=0; i<n; ++i) {
            uint64_t pa=fm_phys_4k(g_map_ptes[i]);
            if (!(g_map_ptes[i] & FM_X86_P) || (pa & (FM_4K_SIZE-1)) ||
                (pa & (FM_2M_SIZE-1)) == 0) ok=0;
            for (size_t j=0;j<i;++j)
                if (pa == fm_phys_4k(g_map_ptes[j])) ok=0;
        }
        check(ok,"direct PTEs unique and exclude metadata");
        check(test_high_va_alias(0),"first mapped virtual address reaches physical frame");
        check(test_high_va_alias(n-1),"last mapped virtual address reaches physical frame");
        if(n >= 513)
            check(test_high_va_alias(512),"mapping crosses PT page boundary");
        check(unmap_return(n)==FM_OK,"unmap, INVLPG, return all frames");
        fm_get_stats(&g_manager, &after);
        check(after.free_4k == before.free_4k,
              "all directly mapped frames returned");
    }
    /* Occupied destination must fail without changing the TOP cursor. */
    fm_stats before_failed;
    fm_get_stats(&g_manager,&before_failed);
    g_map_ptes[1] = UINT64_C(0x4000000000000000);
    check(fm_alloc_map_4k_at(&g_manager,2,g_map_ptes,MAP_VA_BASE)==FM_EINVAL,
          "occupied leaf PTE rejected atomically");
    g_map_ptes[1] = 0;
    fm_get_stats(&g_manager,&after);
    check(before_failed.free_4k == after.free_4k,
          "failed direct mapping leaves cursor unchanged");
}

/* Full cycle: no staging array on the ALLOCATION path. The timed work
 * installs real leaf PTEs, clears them, INVLPGs the 4KiB virtual pages,
 * and returns physical frames. This includes TLB work and synchronization;
 * it is not a pure TOP pop microbenchmark. */
static void benchmark_direct_map(unsigned n, unsigned iterations) {
    if (!n || n>MAX_BATCH || !iterations) return;
    if (fm_alloc_map_4k_at(&g_manager,n,g_map_ptes,MAP_VA_BASE)!=FM_OK) {
        check(0,"direct-map benchmark initial allocation");return;
    }
    /* The same leaf PTEs move target-PT -> PCP-PT -> target-PT. */
    uint64_t samples[N_TRIALS];
    for (unsigned trial=0;trial<N_TRIALS;++trial) {
        uint64_t begin=tsc_begin();
        for (unsigned i=0;i<iterations;++i) {
            /* Clear target PTEs, invalidate, and move those exact PTEs into
             * PCP virtual slots; allocation moves them back without a PA list. */
            if (fm_free_unmap_4k(&g_manager,n,g_map_ptes,MAP_VA_BASE)!=FM_OK) {
                g_failed=1;break;
            }
            if (fm_alloc_map_4k_at(&g_manager,n,g_map_ptes,MAP_VA_BASE)!=FM_OK) {
                g_failed=1;break;
            }
        }
        uint64_t end=tsc_end();
        samples[trial]=(end-begin)/iterations;
        g_checksum+=g_map_ptes[0] ^ g_map_ptes[n-1];
    }
    sort_u64(samples,N_TRIALS);
    uint64_t cycles=samples[N_TRIALS/2];
    uint64_t per_100=cycles*100/n;
    print("[bench] map ");print_u64(n);print(" page(s): ");
    print_u64(cycles);print(" cycles/round, ");
    print_u64(per_100/100);print(".");
    if(per_100%100<10)print("0");
    print_u64(per_100%100);print(" cycles/page");
    print_time_estimate(cycles, n);
    print("\n");
    check(unmap_return(n)==FM_OK,"direct-map benchmark restores all frames");
}

/* Legacy one-page indexed baseline, same free stack, no locking and
 * alignment validation, but re-computes (free_index / 511, % 511) per op.
 * The cursor fields are not touched; each free + alloc roundtrip restores the
 * original free_count, so the real manager remains consistent afterward.
 * Separate non-inline functions prevent elimination of the compared work. */
static __attribute__((noinline)) int legacy_free_one(fm_manager *m, uint64_t pa) {
    if ((pa & (FM_4K_SIZE - 1)) || (pa & ~FM_PHYS_MASK_4K)) return FM_EINVAL;
    fm_mutex_lock(&m->mutex);
    if (m->free_4k >= m->chunks_owned * FM_4K_FRAMES_PER_CHUNK) {
        fm_mutex_unlock(&m->mutex);
        return FM_ENOMEM;
    }
    size_t at = m->free_4k;
    m->meta_4k[(at / 511u) * 512u + 1u + (at % 511u)] = pa | FM_PTE_FLAGS;
    ++m->free_4k;
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}
static __attribute__((noinline)) int legacy_alloc_one(fm_manager *m, uint64_t *out) {
    fm_mutex_lock(&m->mutex);
    if (!m->free_4k) {fm_mutex_unlock(&m->mutex);return FM_ENOMEM;}
    size_t at = --m->free_4k;
    *out = m->meta_4k[(at / 511u) * 512u + 1u + (at % 511u)];
    fm_mutex_unlock(&m->mutex);
    return FM_OK;
}
static void benchmark_legacy(unsigned iterations) {
    uint64_t entry=0;
    if (fm_alloc_4k(&g_manager,1,&entry)!=FM_OK) {check(0,"legacy initial alloc");return;}
    uint64_t pa=fm_phys_4k(entry);
    uint64_t samples[N_TRIALS];
    for (unsigned trial=0;trial<N_TRIALS;++trial) {
        uint64_t begin=tsc_begin();
        for (unsigned i=0;i<iterations;++i) {
            if (legacy_free_one(&g_manager,pa)!=FM_OK) {g_failed=1;break;}
            if (legacy_alloc_one(&g_manager,&entry)!=FM_OK) {g_failed=1;break;}
        }
        uint64_t end=tsc_end();
        samples[trial]=(end-begin)/iterations;
        g_checksum+=entry;
    }
    sort_u64(samples,N_TRIALS);
    print("[bench] legacy global idx 1 page: ");
    print_u64(samples[N_TRIALS/2]); print(" cycles/round");
    print_time_estimate(samples[N_TRIALS/2], 1);
    print("\n");
    check(fm_free_4k(&g_manager,1,&pa)==FM_OK,"legacy benchmark restores pool");
}

static void benchmark(unsigned n, unsigned iterations) {
    if (n>MAX_BATCH || !iterations) return;
    if (fm_alloc_4k(&g_manager,n,g_output)!=FM_OK) {
        check(0,"benchmark initial allocation"); return;
    }
    for (unsigned i=0;i<n;++i) g_pa[i]=fm_phys_4k(g_output[i]);
    uint64_t samples[N_TRIALS];
    for (unsigned trial=0;trial<N_TRIALS;++trial) {
        uint64_t begin=tsc_begin();
        for (unsigned i=0;i<iterations;++i) {
            /* No return-list preparation in timed loop. The same N owned
             * frames are returned and immediately reacquired every cycle. */
            if (fm_free_4k(&g_manager,n,g_pa)!=FM_OK) {g_failed=1;break;}
            if (fm_alloc_4k(&g_manager,n,g_output)!=FM_OK) {g_failed=1;break;}
        }
        uint64_t end=tsc_end();
        samples[trial]=(end-begin)/iterations;
        g_checksum+=g_output[0] ^ g_output[n-1];
    }
    sort_u64(samples,N_TRIALS);
    uint64_t cycles=samples[N_TRIALS/2];
    uint64_t per_100=cycles*100/n;
    print("[bench] PCP "); print_u64(n); print(" page(s): ");
    print_u64(cycles); print(" cycles/round, ");
    print_u64(per_100/100); print(".");
    if ((per_100%100)<10) print("0");
    print_u64(per_100%100);
    print(" cycles/page");
    print_time_estimate(cycles, n);
    print("\n");
    check(fm_free_4k(&g_manager,n,g_pa)==FM_OK,"benchmark restores free frames");
}
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    (void)image_handle;
    if (!g_reloc_anchor) return EFI_OUT_OF_RESOURCES;
    g_bs=(EFI_SERVICES *)system_table->boot_services;
    g_rt=(EFI_SERVICES *)system_table->runtime_services;
    serial_init();
    print("zkernel Frame Pool | UEFI x86-64\n");
    print("[boot] single-core non-reentrant, lock-free build\n");
    EFI_SET_WATCHDOG wd=(EFI_SET_WATCHDOG)g_bs->slot[BS_SET_WATCHDOG_TIMER];
    if (wd) wd(0,0,0,0);

    uint64_t stored=hwm_read();
    EFI_PHYSICAL_ADDRESS raw=0;
    size_t request_pages=(ROOT_COUNT+1)*512;
    EFI_STATUS e=alloc_pages(request_pages,&raw);
    if (EFI_ERROR(e)) {
        print("[FAIL] firmware cannot reserve 2MiB root test frames\n");
        g_failed=1;goto done;
    }
    uint64_t aligned=(raw+FM_2M_SIZE-1)&~(FM_2M_SIZE-1);
    if (aligned+ROOT_COUNT*FM_2M_SIZE>raw+request_pages*4096) {
        print("[FAIL] root physical alignment\n");g_failed=1;goto done;
    }
    for (unsigned i=0;i<ROOT_COUNT;++i)
        g_root_pa[i]=aligned+(uint64_t)i*FM_2M_SIZE;
    if (init_private_metadata_page_tables() != 0) {
        print("[FAIL] cannot install private metadata page tables\n");
        g_failed=1;goto done;
    }
    fm_config cfg={0};
    cfg.root_physical_frames=g_root_pa;
    cfg.root_count=ROOT_COUNT;
    cfg.root_metadata_va=g_root_meta;
    cfg.root_metadata_entries=ROOT_COUNT;
    cfg.metadata_4k_va=(uint64_t *)(uintptr_t)META_VA_BASE;
    cfg.metadata_4k_pages_reserved=META_PAGES;
    cfg.pcp_leaf_ptes=g_pcp_ptes;
    cfg.pcp_virtual_base=PCP_VA_BASE;
    cfg.saved_high_water_pages=stored;
    cfg.hooks.context=0;
    cfg.hooks.map_meta_page=map_meta;
    cfg.hooks.unmap_meta_page=unmap_meta;
    cfg.hooks.store_high_water=hwm_store;
    cfg.hooks.pcp_slot_changed=pcp_slot_changed;
    cfg.hooks.invalidate_page=invalidate_page;
    int rc=fm_init(&g_manager,&cfg);
    if (rc!=FM_OK) {print("[FAIL] fm_init ");print_u64((uint64_t)-rc);print("\n");g_failed=1;goto done;}
    fm_stats stats;
    fm_get_stats(&g_manager,&stats);
    print("[boot] saved="); print_u64(stored);
    print(" pages, initialized="); print_u64(stats.chunks_owned);
    print(" chunks, degraded="); print_u64(stats.boot_degraded);
    print("\n");
    check(!stats.boot_degraded,"high-water initialization from UEFI NV variable");

    test_correctness();
    if (g_failed) goto done;
    test_pcp_virtual_mapping();
    if (g_failed) goto done;
    test_pcp();
    if (g_failed) goto done;
    print("[test] PCP PASS (1024 virtual PTE slots, 512-frame handoffs)\n");
    fm_get_stats(&g_manager, &stats);
    print("[pcp] size=");print_u64(FM_PCP_CAPACITY);
    print(" batch=");print_u64(FM_PCP_BATCH);
    print(" cached=");print_u64(stats.pcp_free_4k);
    print(" global=");print_u64(stats.main_free_4k);
    print(" refills=");print_u64(stats.pcp_refills);
    print(" drains=");print_u64(stats.pcp_drains);
    print(" backed-meta=");print_u64(stats.backed_4k_meta_pages);
    print(" pages pcp-va=0x");print_hex_u64(PCP_VA_BASE);
    print("\n");
    test_direct_map();
    if (g_failed) goto done;
    print("[test] correctness PASS\n");
    calibrate_tsc();
    print("[bench] TSC cycles and estimated time per free+alloc round (7-trial median)\n");
    benchmark(1,200000);
    benchmark_legacy(200000);
    benchmark(511,10000);
    benchmark(512,10000);
    benchmark(1025,5000);
    print("[bench] direct target-PT <-> PCP-PT transfer + INVLPG (real CR3 mappings)\n");
    benchmark_direct_map(1,50000);
    benchmark_direct_map(511,200);
    benchmark_direct_map(512,200);
    benchmark_direct_map(1025,100);
    print("[boot] high-water: ");print_u64(stored);
    print(" -> ");print_u64(hwm_read());print(" pages\n");

done:
    /* Return control with the original firmware CR3 installed. */
    if (g_firmware_cr3 &&
        (read_cr3_raw() & PHYS_ADDR_MASK) == g_private_cr3)
        write_cr3(g_firmware_cr3);
    if (!g_failed) {
        print("[test] PASS ");print_u64(g_passed);print(" checks\n");
    }
    print(g_failed ? "FRAME_POOL_UEFI_TEST_FAILED\n" : "FRAME_POOL_UEFI_TEST_PASSED\n");
    /* isa-debug-exit device makes QEMU exit deterministically.
     * Code 0x10 => host 33 = success; code 0x11 => host 35 = failure.
     * On actual UEFI hardware without isa-debug-exit the port write is inert. */
    outb(0xf4,(uint8_t)(g_failed?0x11:0x10));
    return g_failed?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;
}
