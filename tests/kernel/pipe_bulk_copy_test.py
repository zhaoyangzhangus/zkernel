"""Exercise unchanged production ring-copy functions across wrapping boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/ipc/pipe.c').read_text()
begin = source.index('static void pipe_copy_in(')
end = source.index('/* 在谓词内完成传输', begin)
functions = source[begin:end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    uint8_t *buffer;
    size_t capacity, tail, head, count;
} pipe_state_t;
''' + functions + r'''
int main(void) {
    const size_t capacities[] = {1,2,7,8,63,64,65,127,257,4096,32768};
    for (unsigned c = 0; c < sizeof(capacities)/sizeof(capacities[0]); ++c) {
        size_t capacity = capacities[c];
        uint8_t *ring = malloc(capacity + 2), *input = malloc(capacity),
                *output = malloc(capacity + 2);
        assert(ring && input && output);
        for (size_t i = 0; i < capacity; ++i) input[i] = (uint8_t)(i*37+11);
        size_t starts[] = {0,1,7,63,64,capacity/2,capacity-1};
        size_t sizes[] = {0,1,7,63,64,65,capacity/2,capacity-1,capacity};
        size_t start_count = capacity <= 257 ? capacity : 7;
        size_t size_count = capacity <= 127 ? capacity+1 : 9;
        for (size_t s = 0; s < start_count; ++s) {
            size_t start = capacity <= 257 ? s : starts[s];
            if (start >= capacity) continue;
            for (size_t n = 0; n < size_count; ++n) {
                size_t length = capacity <= 127 ? n : sizes[n];
                if (length > capacity) continue;
                for (size_t i = 0; i < capacity+2; ++i)
                    ring[i] = output[i] = 0xa5;
                pipe_state_t state = {ring+1, capacity, start, start, 0};
                pipe_copy_in(&state, input, length);
                assert(state.tail == (start+length)%capacity);
                assert(state.count == length && state.head == start);
                for (size_t i = 0; i < capacity; ++i) {
                    size_t offset = (i+capacity-start)%capacity;
                    assert(ring[i+1] == (offset < length ? input[offset] : 0xa5));
                }
                pipe_copy_out(&state, output+1, length);
                assert(state.head == state.tail && state.count == 0);
                assert(ring[0] == 0xa5 && ring[capacity+1] == 0xa5);
                for (size_t i = 0; i < capacity+2; ++i)
                    assert(output[i] == (i > 0 && i <= length ? input[i-1] : 0xa5));
            }
        }
        uint8_t *fifo = malloc(capacity);assert(fifo);
        pipe_state_t state = {ring+1, capacity, capacity-1, capacity-1, 0};
        size_t count=0,head=capacity-1,tail=capacity-1;
        unsigned random=17;
        for(unsigned operation=0;operation<4096;++operation) {
            random=random*1664525U+1013904223U;
            bool write=(random>>31)!=0;
            random=random*1664525U+1013904223U;
            size_t length=random%((write?capacity-count:count)+1);
            if(write) {
                pipe_copy_in(&state,input,length);
                memcpy(fifo+count,input,length);count+=length;
                tail=(tail+length)%capacity;
            } else {
                pipe_copy_out(&state,output+1,length);
                assert(!memcmp(output+1,fifo,length));count-=length;
                memmove(fifo,fifo+length,count);head=(head+length)%capacity;
            }
            assert(state.count==count && state.head==head && state.tail==tail);
            assert(ring[0]==0xa5 && ring[capacity+1]==0xa5);
        }
        pipe_copy_out(&state,output+1,count);assert(!memcmp(output+1,fifo,count) && !state.count);
        free(fifo);free(ring); free(input); free(output);
    }
    puts("Pipe bulk copy: 11 arbitrary capacities, exact spans/wrap/zero/full ring, 45056 mixed FIFO operations, cursors/count/canaries passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pipe-bulk-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
