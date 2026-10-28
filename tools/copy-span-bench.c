/* Bounded host WB-RAM benchmark, NOT a GOP/MMIO throughput measurement.
 * cc -O2 -Wall -Wextra -Werror tools/copy-span-bench.c -o /tmp/copy-span-bench
 * No GPU mappings, privileged instructions or host configuration changes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*copy_fn)(void *, const void *, size_t);
#ifdef OS_BENCH_KERNEL_COPY
/* Link kernel/string.c compiled separately with kernel (non-SIMD) flags and
 * -Dmemcpy=os_bench_kernel_memcpy (rename the other exports likewise).
 * Keep timing/stdio in this hosted translation unit. */
void *os_bench_kernel_memcpy(void *, const void *, size_t);
static void copy_kernel(void *d, const void *s, size_t n) {
    os_bench_kernel_memcpy(d,s,n);
}
static __attribute__((noinline,optimize("no-tree-vectorize")))
void copy_old_bytes(void *d, const void *s, size_t n) {
    uint8_t *out=d;
    const uint8_t *in=s;
    while(n--!=0) *out++=*in++;
}
#endif
static __attribute__((noinline)) void copy_movsb(void *d, const void *s, size_t n) {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static __attribute__((noinline)) void copy_movsq(void *d, const void *s, size_t n) {
    size_t q = n / 8U, tail = n % 8U;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    if (tail != 0U)
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
}
static __attribute__((noinline)) void copy_scalar32(void *d, const void *s, size_t n) {
    volatile uint32_t *out = d;
    const uint32_t *in = s;
    for (size_t i = 0; i < n / 4U; ++i) out[i] = in[i];
}
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void frame(copy_fn copy, uint8_t *d, const uint8_t *s,
                  size_t row, size_t pitch, size_t height) {
    for (size_t y = 0; y < height; ++y) copy(d + y*pitch, s + y*pitch, row);
    __asm__ volatile("sfence" : : : "memory");
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int verify_boundaries(void) {
    _Alignas(64) uint8_t src[1152], dst[1152];
    for (size_t i=0;i<sizeof(src);++i) src[i]=(uint8_t)(i*29U+1U);
    const copy_fn methods[]={copy_movsb,copy_movsq,copy_scalar32,
#ifdef OS_BENCH_KERNEL_COPY
                            copy_kernel,copy_old_bytes,
#endif
    };
    for (unsigned method=0;method<sizeof(methods)/sizeof(*methods);++method)
    for (size_t s=0;s<8;++s) for (size_t d=0;d<8;++d)
    for (size_t n=0;n<=1024;++n) {
        if (method==2 && ((s|d|n)&3U)) continue;
        memset(dst,0xa5,sizeof(dst));
        methods[method](dst+64+d,src+64+s,n);
        if(memcmp(dst+64+d,src+64+s,n)) return 0;
        for(size_t i=0;i<64+d;++i) if(dst[i]!=0xa5) return 0;
        for(size_t i=64+d+n;i<sizeof(dst);++i) if(dst[i]!=0xa5) return 0;
    }
    return 1;
}
static void short_copies(void) {
    _Alignas(64) uint8_t src[8192],dst[8192];
    memset(src,0x37,sizeof(src));memset(dst,0,sizeof(dst));
    const size_t lengths[]={16,32,63,64,65,76,128,152,256,1440,1500,2048,3032,4096};
    const size_t offsets[]={0,1,4};
    const copy_fn methods[]={copy_movsb,copy_movsq,
#ifdef OS_BENCH_KERNEL_COPY
                            copy_kernel,copy_old_bytes,
#endif
    };
    const char *names[]={"rep_movsb","rep_movsq",
#ifdef OS_BENCH_KERNEL_COPY
                         "kernel_memcpy","old_byte_loop",
#endif
    };
    const unsigned method_count=sizeof(methods)/sizeof(*methods);
    puts("scope=host_WB_RAM mode=short_copies samples=7 fence=per_batch not_per_copy");
    for(unsigned n=0;n<sizeof(lengths)/sizeof(*lengths);++n)for(unsigned off=0;off<3;++off) {
        double samples[sizeof(methods)/sizeof(*methods)][7];
        size_t count=4194304U/lengths[n];if(count<16384)count=16384;
        for(unsigned round=0;round<7;++round)for(unsigned slot=0;slot<method_count;++slot) {
            unsigned method=(round+slot)%method_count;
            __asm__ volatile("mfence" : : : "memory");
            uint64_t begin=now();
            for(size_t i=0;i<count;++i)methods[method](dst+offsets[off],src,lengths[n]);
            __asm__ volatile("mfence" : : : "memory");
            samples[method][round]=(double)(now()-begin)/count;
            if(memcmp(dst+offsets[off],src,lengths[n]))exit(2);
        }
        for(unsigned method=0;method<method_count;++method) {
            qsort(samples[method],7,sizeof(double),compare);
            printf("bytes=%zu dst_offset=%zu method=%s median_ns=%.2f\n",lengths[n],offsets[off],
                   names[method],samples[method][3]);
        }
    }
}
int main(int argc, char **argv) {
    if (argc>2 || (argc==2 && strcmp(argv[1],"--short"))) {
        fputs("usage: copy-span-bench [--short]\n",stderr);return 1;
    }
    if (!verify_boundaries()) { fputs("boundary validation failed\n",stderr); return 1; }
    cpu_set_t allowed, selected;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) { perror("affinity"); return 1; }
    int cpu;
    for (cpu = 0; cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed); ++cpu) {}
    if (cpu == CPU_SETSIZE) return 1;
    CPU_ZERO(&selected); CPU_SET(cpu, &selected);
    if (sched_setaffinity(0, sizeof(selected), &selected)) { perror("pin"); return 1; }
    if(argc==2) {short_copies();return 0;}
    const size_t capacity = (3840U*4U+64U)*2160U+256U;
    uint8_t *src = 0, *dst = 0;
    if (posix_memalign((void **)&src, 64, capacity) ||
        posix_memalign((void **)&dst, 64, capacity)) { free(src); free(dst); return 1; }
    for (size_t i = 0; i < capacity; ++i) src[i] = (uint8_t)(i*37U+(i>>8));
    const unsigned shapes[][2] = {{64,32},{800,600},{1920,1080},{3840,2160}};
    const copy_fn methods[] = {copy_scalar32, copy_movsb, copy_movsq};
    const char *names[] = {"volatile32", "rep_movsb", "rep_movsq"};
    printf("scope=host_WB_RAM cpu=%d samples=7 fence=per_frame working_set_max_MiB=64\n",cpu);
    puts("mode=warm_repeated_same_buffers includes_row_calls excludes_allocation_and_validation");
    for (unsigned shape = 0; shape < 4; ++shape)
    for (unsigned offset = 0; offset <= 4; offset += 4)
    for (unsigned padded = 0; padded < 2; ++padded) {
        size_t width=shapes[shape][0], height=shapes[shape][1];
        size_t row=width*4U, pitch=row+(padded?64U:0U);
        unsigned loops=(unsigned)((64U*1024U*1024U)/(row*height));
        if (loops < 4) loops=4;
        double samples[3][7];
        // Rotate method order to reduce systematic temperature/order bias.
        for (unsigned round=0; round<7; ++round)
        for (unsigned slot=0; slot<3; ++slot) {
            unsigned method=(round+slot)%3;
            memset(dst,0xa5,capacity);
            uint8_t *d=dst+64+offset;
            const uint8_t *s=src+64; // Also exercises mismatched source alignment.
            frame(methods[method],d,s,row,pitch,height);
            __asm__ volatile("mfence" : : : "memory");
            uint64_t begin=now();
            for (unsigned i=0;i<loops;++i) frame(methods[method],d,s,row,pitch,height);
            samples[method][round]=(double)(now()-begin)/loops;
            for (size_t y=0;y<height;++y) {
                if (memcmp(d+y*pitch,s+y*pitch,row)) return 2;
                for (size_t x=row;x<pitch;++x) if(d[y*pitch+x]!=0xa5) return 3;
            }
            for (size_t i=0;i<64+offset;++i) if(dst[i]!=0xa5) return 4;
            for (size_t i=64+offset+height*pitch;i<capacity;++i) if(dst[i]!=0xa5) return 5;
        }
        for(unsigned method=0;method<3;++method) {
            qsort(samples[method],7,sizeof(double),compare);
            double ns=samples[method][3];
            printf("size=%zux%zu dst_offset=%u pad=%u method=%s median_us=%.2f min_us=%.2f max_us=%.2f GB_s=%.2f\n",
                   width,height,offset,padded?64U:0U,names[method],ns/1000.,
                   samples[method][0]/1000.,samples[method][6]/1000.,row*height/ns);
        }
    }
    free(src); free(dst);
    puts("COPY_CORRECTNESS_AND_CANARIES_PASS");
    return 0;
}
