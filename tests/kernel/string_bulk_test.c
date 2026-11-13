#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

void *liteos_memcpy(void *, const void *, size_t);
void *liteos_memset(void *, int, size_t);
void *liteos_memmove(void *, const void *, size_t);

static unsigned char *guarded_page(void) {
    unsigned char *base = mmap(NULL, 12288, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED);
    assert(mprotect(base + 4096, 4096, PROT_READ | PROT_WRITE) == 0);
    return base + 4096;
}

static void fill(unsigned char *buffer, size_t size, unsigned char value) {
    for (size_t i = 0; i < size; ++i) buffer[i] = value;
}

static double seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

/* 原始生产循环，并使用相同的非 SIMD 编译标志。 */
__attribute__((noinline))
static void scalar_copy(unsigned char *out, const unsigned char *in,
                        size_t length) {
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
}

__attribute__((noinline))
static void scalar_backward(unsigned char *out,const unsigned char *in,size_t length) {
    for (size_t i=length;i!=0;--i) out[i-1]=in[i-1];
}

int main(void) {
    unsigned char *source = guarded_page(), *target = guarded_page();
    const size_t lengths[] = {0, 1, 7, 8, 15, 31, 63, 64, 65,
                              71, 127, 128, 255, 256, 1023, 4096};
    assert(liteos_memcpy(NULL, NULL, 0) == NULL);
    assert(liteos_memset(NULL, 123, 0) == NULL);
    assert(liteos_memmove(NULL, NULL, 0) == NULL);
    unsigned char overlap[8192], expected[8192];
    for (size_t n=0; n<sizeof(lengths)/sizeof(lengths[0]); ++n) {
        size_t length=lengths[n];
        for (unsigned shift=0; shift<32; ++shift) {
            for (unsigned direction=0; direction<2; ++direction) {
                for (unsigned i=0;i<8192;++i) overlap[i]=expected[i]=(unsigned char)(i*37+11);
                size_t s=direction ? shift : 0, d=direction ? 0 : shift;
                /* 参考结果从原始源数据读取，且在任何写入之前完成。 */
                for (size_t i=0;i<length;++i) expected[d+i]=(unsigned char)((s+i)*37+11);
                assert(liteos_memmove(overlap+d,overlap+s,length)==overlap+d);
                for (unsigned i=0;i<8192;++i) assert(overlap[i]==expected[i]);
            }
        }
    }
    uint32_t random_state=0x91375426;
    for (unsigned trial=0;trial<2048;++trial) {
        random_state=random_state*1664525U+1013904223U;
        size_t gap=random_state%128,base=(random_state>>8)%8;
        random_state=random_state*1664525U+1013904223U;
        size_t length=random_state%(8193-gap-base);
        size_t s=base+(trial&1?gap:0),d=base+(trial&1?0:gap);
        for (unsigned i=0;i<8192;++i) overlap[i]=expected[i]=(unsigned char)(i*37+11);
        for (size_t i=0;i<length;++i) expected[d+i]=(unsigned char)((s+i)*37+11);
        assert(liteos_memmove(overlap+d,overlap+s,length)==overlap+d);
        for (unsigned i=0;i<8192;++i) assert(overlap[i]==expected[i]);
    }
    for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        size_t length = lengths[n];
        /* 起始/结束位置都紧邻 PROT_NONE 保护区，用于检测越界读写。
         * 两个操作数分别覆盖所有模 8 偏移。 */
        for (unsigned s = 0; s < 16; ++s) {
            for (unsigned d = 0; d < 16; ++d) {
                if (length + s > 4096 || length + d > 4096) continue;
                for (unsigned edge = 0; edge < 4; ++edge) {
                    size_t si = edge & 1 ? 4096 - length - s : s;
                    size_t di = edge & 2 ? 4096 - length - d : d;
                    fill(target, 4096, 0xa5);
                    for (size_t i = 0; i < 4096; ++i)
                        source[i] = (unsigned char)(i * 37 + 11);
                    assert(liteos_memcpy(target + di, source + si, length)
                           == target + di);
                    assert(liteos_memmove(target + di, source + si, length)
                           == target + di);
                    for (size_t i = 0; i < 4096; ++i)
                        assert(target[i] == (i >= di && i - di < length ?
                               source[si + i - di] : 0xa5));
                    const int values[] = {0, 1, -1, 0x12ab};
                    for (unsigned v = 0; v < 4; ++v) {
                        fill(target, 4096, 0xa5);
                        assert(liteos_memset(target + di, values[v], length)
                               == target + di);
                        for (size_t i = 0; i < 4096; ++i)
                            assert(target[i] == (i >= di && i - di < length ?
                                   (unsigned char)values[v] : 0xa5));
                    }
                }
            }
        }
    }
    /* 同页重叠场景紧邻两侧保护区，并覆盖小于一个字的间隔。
     * 整页比较还能证明移动范围以外的字节未被修改。 */
    unsigned char snapshot[4096];
    for (unsigned gap=1;gap<32;++gap) {
        for (unsigned direction=0;direction<2;++direction) {
            for (unsigned edge=0;edge<2;++edge) {
                for (unsigned i=0;i<4096;++i)
                    source[i]=snapshot[i]=(unsigned char)(i*37+11);
                size_t length=4096-gap-(edge?17:0);
                size_t base=edge?17:0;
                size_t s=base+(direction?gap:0),d=base+(direction?0:gap);
                assert(liteos_memmove(source+d,source+s,length)==source+d);
                for (unsigned i=0;i<4096;++i)
                    assert(source[i]==(i>=d && i-d<length?snapshot[s+i-d]:snapshot[i]));
            }
        }
    }
    assert(munmap(source - 4096, 12288) == 0);
    assert(munmap(target - 4096, 12288) == 0);
    const size_t sizes[] = {4096, 32768, 1024 * 1024};
    for (unsigned n = 0; n < 3; ++n) {
        size_t size = sizes[n], repeats = 64 * 1024 * 1024 / size;
        unsigned char *a = malloc(size), *b = malloc(size);
        assert(a && b);
        fill(a, size, 7);
        double begin = seconds();
        for (size_t i = 0; i < repeats; ++i) scalar_copy(b, a, size);
        double scalar = seconds() - begin;
        begin = seconds();
        for (size_t i = 0; i < repeats; ++i) liteos_memcpy(b, a, size);
        double bulk = seconds() - begin;
        assert(b[0] == 7 && b[size - 1] == 7);
        printf("copy size=%zu scalar_ms=%.3f bulk_ms=%.3f speedup=%.2f\n",
               size, scalar * 1000, bulk * 1000, scalar / bulk);
        free(a);
        free(b);
    }
    for (unsigned gap=1;gap<=16;gap*=4) {
        size_t size=32768,repeats=64*1024*1024/size;
        unsigned char *buffer=malloc(size+gap);assert(buffer);
        fill(buffer,size+gap,7);
        double begin=seconds();
        for (size_t i=0;i<repeats;++i) scalar_copy(buffer,buffer+gap,size);
        double scalar=seconds()-begin;
        begin=seconds();
        for (size_t i=0;i<repeats;++i) liteos_memmove(buffer,buffer+gap,size);
        double bulk=seconds()-begin;
        assert(buffer[0]==7 && buffer[size+gap-1]==7);
        printf("forward overlap gap=%u size=%zu scalar_ms=%.3f bulk_ms=%.3f speedup=%.2f\n",
               gap,size,scalar*1000,bulk*1000,scalar/bulk);
        begin=seconds();
        for (size_t i=0;i<repeats;++i) scalar_backward(buffer+gap,buffer,size);
        scalar=seconds()-begin;
        begin=seconds();
        for (size_t i=0;i<repeats;++i) liteos_memmove(buffer+gap,buffer,size);
        bulk=seconds()-begin;
        assert(buffer[0]==7 && buffer[size+gap-1]==7);
        printf("backward overlap gap=%u size=%zu scalar_ms=%.3f bulk_ms=%.3f speedup=%.2f\n",
               gap,size,scalar*1000,bulk*1000,scalar/bulk);
        free(buffer);
    }
    puts("Kernel bulk copy/fill/move: overlap guard pages, alignment, exact tails and return values passed");
}
