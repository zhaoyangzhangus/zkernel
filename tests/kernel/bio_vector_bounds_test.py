"""Actual BIO validation: compound bounds, cheap failures and maximum vector count."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/block/queue.c').read_text()
functions = source[source.index('static bool bio_page_vectors_valid('):
                   source.index('kstatus_t block_submit_bio(')]
code = r'''
#include <kernel/block.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static page_t page;
static unsigned translations;
static bool physical_failure;
paddr_t page_to_phys(const page_t *p) {
    assert(p==&page); ++translations;
    return paddr_make(physical_failure ? UINT64_MAX : 4096);
}
''' + functions + r'''
static bool metadata_valid(uint32_t flags,unsigned order,uint32_t offset,uint32_t length) {
    if(flags & (PAGE_FREE | PAGE_COMPOUND_TAIL)) return false;
    if((flags & PAGE_COMPOUND_HEAD) && order>BUDDY_MAX_ORDER) return false;
    __uint128_t capacity=PAGE_SIZE;
    if(flags & PAGE_COMPOUND_HEAD) capacity*=((__uint128_t)1<<order);
    return length && (__uint128_t)offset<capacity && (__uint128_t)offset+length<=capacity;
}
int main(void) {
    bio_vec_t vec={.page=&page};
    io_request_t request={0}; uint8_t device_token;
    request.device=(struct device *)&device_token;
    request.opcode=IO_READ; request.vec_count=1; atomic_init(&request.state,IOREQ_NEW);
    bio_t bio={.op=BIO_OP_READ,.vecs=&vec,.vec_count=1,.io=&request};
    const uint32_t bounds[]={0,1,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,
                            (PAGE_SIZE<<BUDDY_MAX_ORDER)-1,
                            PAGE_SIZE<<BUDDY_MAX_ORDER,
                            (PAGE_SIZE<<BUDDY_MAX_ORDER)+1,UINT32_MAX-1,UINT32_MAX};
    unsigned cases=0;
    for(unsigned order=0;order<256;++order) for(unsigned mask=0;mask<8;++mask) {
        page.order=(uint8_t)order;
        page.flags=PAGE_DIRTY | ((mask&1)?PAGE_FREE:0) |
                   ((mask&2)?PAGE_COMPOUND_HEAD:0) | ((mask&4)?PAGE_COMPOUND_TAIL:0);
        for(unsigned i=0;i<10;++i) for(unsigned j=0;j<10;++j) for(unsigned fail=0;fail<2;++fail) {
            vec.offset=bounds[i]; vec.length=bounds[j]; physical_failure=fail;
            bool metadata=metadata_valid(page.flags,order,vec.offset,vec.length);
            translations=0;
            assert(bio_submission_valid(&bio)==(metadata && !fail));
            assert(translations==(unsigned)metadata); ++cases;
        }
    }
    page.flags=0; page.order=255; physical_failure=false; vec.offset=0; vec.length=1;
    for(unsigned op=0;op<6;++op) for(unsigned opcode=0;opcode<8;++opcode) {
        bio.op=op; request.opcode=opcode; translations=0;
        bool valid=(op==BIO_OP_READ && opcode==IO_READ) ||
                   (op==BIO_OP_WRITE && opcode==IO_WRITE) ||
                   (op==BIO_OP_FLUSH && opcode==IO_FLUSH);
        assert(bio_submission_valid(&bio)==valid && translations==(unsigned)valid);
    }
    bio.op=BIO_OP_READ; request.opcode=IO_READ;
    request.vec_count=0; translations=0;
    assert(!bio_submission_valid(&bio) && !translations); request.vec_count=1;
    vec.page=0; assert(!bio_submission_valid(&bio)); vec.page=&page;
    bio.vecs=0; assert(!bio_submission_valid(&bio)); bio.vecs=&vec;
    request.device=0; assert(!bio_submission_valid(&bio)); request.device=(struct device *)&device_token;
    request.completion_target=&page; assert(!bio_submission_valid(&bio)); request.completion_target=0;
    atomic_store(&request.state,IOREQ_SUBMITTED); assert(!bio_submission_valid(&bio));
    atomic_store(&request.state,IOREQ_NEW); assert(!bio_submission_valid(0));
    bio.io=0; assert(!bio_submission_valid(&bio)); bio.io=&request;
    bio.vec_count=0; assert(!bio_submission_valid(&bio));
    bio.op=BIO_OP_FLUSH; request.opcode=IO_FLUSH; request.vec_count=0;
    assert(bio_submission_valid(&bio));
    /* 16 位向量数量和 32 位长度相加不会溢出 64 位总和。
     * 覆盖全部 65535 个条目，并测试最后一个条目失败且不发生累加。 */
    bio_vec_t *many=calloc(UINT16_MAX,sizeof(*many)); assert(many);
    page.flags=PAGE_COMPOUND_HEAD; page.order=BUDDY_MAX_ORDER;
    for(unsigned i=0;i<UINT16_MAX;++i) many[i]=(bio_vec_t){.page=&page,.length=PAGE_SIZE<<BUDDY_MAX_ORDER};
    bio.vecs=many; bio.vec_count=UINT16_MAX; bio.op=BIO_OP_WRITE;
    request.opcode=IO_WRITE; request.vec_count=1; translations=0;
    assert(bio_submission_valid(&bio) && translations==UINT16_MAX);
    many[UINT16_MAX-1].length=0; translations=0;
    assert(!bio_submission_valid(&bio) && translations==UINT16_MAX-1);
    free(many);
    printf("BIO vectors: %u compound-order/flag/bounds/physical-failure oracle cases, early opcode rejection, empty flush, 65535 vectors and late invalid entry passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bio-vectors-') as directory:
    binary = str(Path(directory)/'test')
    command = ['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
               '-I'+str(root/'include'),'-fsanitize=address,undefined',
               '-fno-sanitize-recover=undefined','-fno-pie','-no-pie',
               '-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    negative = code.replace('            if (vec->page->order > BUDDY_MAX_ORDER) return false;', '')
    assert negative != code
    subprocess.run(command,input=negative,text=True,check=True)
    failure = subprocess.run([binary],capture_output=True,text=True,timeout=30)
    assert failure.returncode != 0
    print('Regression control: removing compound-order bound fails the vector oracle')
