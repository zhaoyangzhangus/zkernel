#pragma once

/* System 分配器声明；不要回退到宿主 glibc。 */
#include <libc/libc.h>
#include <sys/cdefs.h>

struct mallinfo {
    int arena;
    int ordblks;
    int smblks;
    int hblks;
    int hblkhd;
    int usmblks;
    int fsmblks;
    int uordblks;
    int fordblks;
    int keepcost;
};

#ifdef __cplusplus
extern "C" {
#endif
int mallopt(int parameter, int value) __THROW;
struct mallinfo mallinfo(void) __THROW;
void malloc_stats(void) __THROW;
int malloc_trim(size_t padding) __THROW;
int malloc_info(int options, FILE *stream) __THROW;
#ifdef __cplusplus
}
#endif
