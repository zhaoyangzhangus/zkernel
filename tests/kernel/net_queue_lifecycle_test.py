"""Production software RX queue: FIFO, wrap, failure balance and concurrent producers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/queue.c').read_text()
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/mm.h>
static _Thread_local unsigned depth;
static _Thread_local size_t copied;
static unsigned inject_fault;
static unsigned allocations,releases,fail_allocation;
static void *owned_queue;
void *kmalloc(size_t n,uint32_t flags){
    assert(!depth && !flags && !owned_queue);
    if(fail_allocation)return 0;
    owned_queue=malloc(n);assert(owned_queue);++allocations;
    memset(owned_queue,0xa5,n);return owned_queue;
}
void kfree(void *p){
    assert(!depth && p && p==owned_queue);
    free(p);owned_queue=0;++releases;
}
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
void *queue_copy(void *out,const void *in,size_t n) {
    assert(depth);copied+=n;memcpy(out,in,n);
    if(inject_fault){
        /* Corrupt one push only, so the pop cannot undo the fault. */
        ((unsigned char *)out)[inject_fault==1 ? n-1 : 0]^=1;
        inject_fault=0;
    }
    return out;
}
#define memcpy queue_copy
''' + source + r'''
#undef memcpy
static e1000_software_queue_t queue;
/* 测试辅助函数消费借用的槽位；生产派发路径直接读取，不做这次复制。 */
static bool test_pop(e1000_software_queue_t *q,e1000_rx_packet_t *out) {
    if(!q || !out)return false;
    const e1000_rx_packet_t *entry=e1000_queue_borrow(q);
    if(!entry)return false;
    memcpy(out,entry,offsetof(e1000_rx_packet_t,payload)+entry->payload_length);
    e1000_queue_release(q,entry);
    return true;
}
static e1000_rx_packet_t packet(unsigned value) {
    e1000_rx_packet_t p;memset(&p,(unsigned char)value,sizeof(p));
    p.sequence=value;p.payload_length=SOCKET_MAX_PAYLOAD;return p;
}
static void check_packet(const e1000_rx_packet_t *p,unsigned value,unsigned length) {
    e1000_rx_packet_t expected=packet(value);
    expected.payload_length=length;
    assert(!memcmp(p,&expected,offsetof(e1000_rx_packet_t,payload)+length));
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<3000;++i) {
        e1000_rx_packet_t p=packet(id*10000+i);
        p.payload_length=(id*10000+i)%(SOCKET_MAX_PAYLOAD+1);
        while(!(id&1 ? e1000_queue_push_payload(&queue,&p,p.payload) :
                       e1000_queue_push(&queue,&p))) {assert(!depth);sched_yield();}
        assert(!depth);
    }
    return 0;
}
int main(void) {
    fail_allocation=1;
    assert(!e1000_packet_queue_self_test() && !allocations && !releases && !owned_queue);
    fail_allocation=0;
    assert(e1000_packet_queue_self_test());
    inject_fault=1;assert(!e1000_packet_queue_self_test() && !inject_fault && !depth);
    inject_fault=2;assert(!e1000_packet_queue_self_test() && !inject_fault && !depth);
    assert(e1000_packet_queue_self_test());
    assert(allocations==4 && allocations==releases && !owned_queue);
    puts("RX boot self-test: allocation failure, balanced cleanup, payload/metadata corruption and clean rerun passed");
    /* Reuse every slot after a full-sized packet, then shrink the payload.
     * Verify exact copy bounds: no stale queue tail escapes into the output. */
    e1000_queue_init(&queue);
    size_t header=offsetof(e1000_rx_packet_t,payload);
    for(unsigned length=0;length<=SOCKET_MAX_PAYLOAD;++length) {
        e1000_rx_packet_t p=packet(length),out;
        p.payload_length=length;
        memset(&out,0xa5,sizeof(out));
        size_t before=copied;
        assert(e1000_queue_push(&queue,&p));
        assert(copied-before==header+length);
        assert(test_pop(&queue,&out));
        assert(copied-before==header+length);
        assert(!memcmp(&p,&out,header+length));
        const unsigned char *tail=(const unsigned char *)&out;
        for(size_t i=header+length;i<sizeof(out);++i)assert(tail[i]==0xa5);
        assert(!depth);
    }
    printf("RX copy bounds: all %u payload lengths; ACK %zu bytes, 1440-byte payload %zu bytes per queue admission; dequeue copies no bytes PASS\n",
           SOCKET_MAX_PAYLOAD+1,header,header+1440);
    for(unsigned length=0;length<=SOCKET_MAX_PAYLOAD;++length) {
        e1000_rx_packet_t p=packet(length),out;
        p.payload_length=length;
        /* Exact-sized allocation lets ASan catch any speculative tail read.
         * Metadata's inline payload deliberately differs from the DMA bytes. */
        uint8_t *dma=length ? malloc(length) : 0;
        if(length){assert(dma);memset(dma,0x5a,length);}
        memset(&out,0xa5,sizeof(out));
        size_t before=copied;
        assert(e1000_queue_push_payload(&queue,&p,dma));
        assert(copied-before==header+length);
        if(length)memset(dma,0xcc,length);
        free(dma); /* Queue must already own a snapshot, not a DMA pointer. */
        assert(test_pop(&queue,&out));
        assert(copied-before==header+length);
        assert(!memcmp(&p,&out,header));
        for(unsigned i=0;i<length;++i)assert(out.payload[i]==0x5a);
        for(size_t i=header+length;i<sizeof(out);++i)
            assert(((unsigned char *)&out)[i]==0xa5);
        assert(!depth);
    }
    puts("Direct RX queue: every payload length, exact-sized DMA source, immediate recycling and output tail bounds PASS");
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        e1000_queue_init(&queue);
        e1000_rx_packet_t p=packet(1),out=packet(99),before=out;
        assert(!e1000_queue_push(0,&p) && !e1000_queue_push(&queue,0));
        assert(!test_pop(0,&out) && !test_pop(&queue,0));
        assert(!test_pop(&queue,&out) && !memcmp(&out,&before,sizeof(out)));
        p.payload_length=SOCKET_MAX_PAYLOAD+1;
        assert(!e1000_queue_push(&queue,&p));
        assert(!e1000_queue_push_payload(&queue,&p,p.payload));
        p.payload_length=1;
        assert(!e1000_queue_push_payload(&queue,&p,0) && depth==nesting);
        assert(!e1000_queue_push_payload(0,&p,p.payload));
        assert(!e1000_queue_push_payload(&queue,0,p.payload));
        for(unsigned cycle=0;cycle<100;++cycle) {
            for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH;++i) {
                p=packet(cycle*E1000_SOFTWARE_QUEUE_DEPTH+i);
                assert(e1000_queue_push(&queue,&p) && depth==nesting);
            }
            assert(!e1000_queue_push(&queue,&p) && depth==nesting);
            assert(queue.count==E1000_SOFTWARE_QUEUE_DEPTH && queue.dropped==cycle+1);
            for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH;++i) {
                assert(test_pop(&queue,&out) && depth==nesting);
                check_packet(&out,cycle*E1000_SOFTWARE_QUEUE_DEPTH+i,SOCKET_MAX_PAYLOAD);
            }
            assert(!test_pop(&queue,&out) && depth==nesting);
        }
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!depth);
    }
    /* 借用的头部既不能被其他消费者拿走，也不能被生产者覆盖。错误归还、
     * 满队列及槽位回绕都不能改变该报文。 */
    e1000_queue_init(&queue);
    assert(!e1000_queue_borrow(0) && !e1000_queue_borrow(&queue));
    e1000_rx_packet_t first=packet(77),wrong=packet(78),out;
    assert(e1000_queue_push(&queue,&first));
    size_t before_borrow=copied;
    const e1000_rx_packet_t *held=e1000_queue_borrow(&queue);
    assert(held==&queue.packets[queue.head] && !depth && copied==before_borrow);
    check_packet(held,77,SOCKET_MAX_PAYLOAD);
    assert(!e1000_queue_borrow(&queue) && !test_pop(&queue,&out));
    e1000_queue_release(0,held);e1000_queue_release(&queue,0);
    e1000_queue_release(&queue,&wrong);
    assert(queue.borrowed && queue.count==1 && held->sequence==77);
    for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH-1;++i){
        e1000_rx_packet_t p=packet(i+100);assert(e1000_queue_push(&queue,&p));
        check_packet(held,77,SOCKET_MAX_PAYLOAD);
    }
    assert(!e1000_queue_push(&queue,&wrong) && queue.count==E1000_SOFTWARE_QUEUE_DEPTH);
    e1000_queue_release(&queue,held);
    assert(!queue.borrowed && queue.count==E1000_SOFTWARE_QUEUE_DEPTH-1);
    e1000_queue_release(&queue,held);assert(queue.count==E1000_SOFTWARE_QUEUE_DEPTH-1);
    for(unsigned i=0;i<E1000_SOFTWARE_QUEUE_DEPTH-1;++i){
        held=e1000_queue_borrow(&queue);assert(held);check_packet(held,i+100,SOCKET_MAX_PAYLOAD);
        e1000_queue_release(&queue,held);
    }
    assert(!e1000_queue_borrow(&queue) && !queue.count && !depth);
    puts("Borrowed RX head: no dequeue copy, exclusive ownership, invalid release, full queue and FIFO wrap PASS");
    e1000_queue_init(&queue);
    pthread_t threads[4];unsigned received[4]={0};
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,producer,(void *)(uintptr_t)i));
    for(unsigned i=0;i<12000;++i) {
        e1000_rx_packet_t out;
        while(!test_pop(&queue,&out)) {assert(!depth);sched_yield();}
        unsigned id=out.sequence/10000;
        assert(id<4 && out.sequence%10000==received[id]++ && !depth);
        check_packet(&out,out.sequence,out.sequence%(SOCKET_MAX_PAYLOAD+1));
    }
    for(unsigned i=0;i<4;++i) {
        assert(!pthread_join(threads[i],0));assert(received[i]==3000);
    }
    assert(!queue.count && !atomic_load(&queue.lock.state) && !depth);
    puts("RX queue: 32 nested depths, invalid/full/empty exits, 102400 complete-packet FIFO round trips and 12000 packets from four concurrent producers passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-queue-') as directory:
    binary = str(Path(directory)/'test')
    for negative in (False, True):
        candidate = code
        if negative:
            candidate = candidate.replace(
                'packet == &queue->packets[queue->head]', 'true')
            assert candidate != code, 'Borrowed slot ownership check is missing'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-fstack-usage',
                        '-I'+str(root/'include'),'-I'+str(root/'kernel/drivers/net'),
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-o',binary],input=candidate,text=True,check=True)
        if not negative:
            frames = [line for report in Path(directory).glob('*.su')
                      for line in report.read_text().splitlines()
                      if 'e1000_packet_queue_self_test\t' in line]
            assert len(frames) == 1, frames
            frame = int(frames[0].split('\t')[1])
            assert frame <= 16384, ('RX boot self-test stack budget exceeded', frame)
            print(f'RX boot self-test stack: {frame} bytes <= 16KiB budget')
        result = subprocess.run([binary],capture_output=True,text=True,timeout=30)
        if negative:
            assert result.returncode != 0, 'Full packet copies must fail the copy-bound test'
            print('Negative control rejected: releasing a different queue slot')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout,end='')
