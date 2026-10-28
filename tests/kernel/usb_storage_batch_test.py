"""Actual USB block callbacks against an in-memory SCSI disk, no host I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/storage.c').read_text()
body = source[source.index('static kstatus_t usb_msc_rw_blocks('):
              source.index('static BOOLEAN usb_msc_block_flush(')]
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int kstatus_t;
typedef void VOID;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef unsigned char BOOLEAN;
#define PAGE_SIZE 4096U
#define XHCI_MSC_MAX_TRANSFER (1024U*1024U)
#define K_OK 0
#define K_EIO -5
#define K_EINVAL -22
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a
typedef struct {uint32_t block_size;uint64_t block_count;uint8_t slot;} usb_msc_device_t;
static uint8_t disk[256*4096],buffer[256*4096],expected[256*4096];
static unsigned calls,enters,leaves,fail_at,short_at;
static bool locked,deny;
static unsigned capacity=65536;
static uint32_t xhci_usb_msc_prepare_buffer(uint8_t slot,uint32_t requested) {
    (void)slot;assert(locked && requested<=XHCI_MSC_MAX_TRANSFER);return capacity;
}
static uint64_t next_lba;
static void msc_store_be32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(24-8*i));
}
static void msc_store_be16(uint8_t *p,uint16_t v) {p[0]=v>>8;p[1]=v;}
static bool usb_msc_enter(usb_msc_device_t *d) {
    (void)d;++enters;assert(!locked);if(deny)return false;locked=true;return true;
}
static void usb_msc_leave(usb_msc_device_t *d) {(void)d;assert(locked);locked=false;++leaves;}
static kstatus_t usb_msc_bot_locked(usb_msc_device_t *d,const uint8_t *cmd,
    uint8_t cmd_len,void *data,uint32_t len,bool in,uint32_t *actual) {
    assert(locked && cmd_len==10 && len && len<=capacity);++calls;
    uint32_t lba=0;for(unsigned i=0;i<4;++i)lba=(lba<<8)|cmd[2+i];
    uint32_t n=((uint32_t)cmd[7]<<8)|cmd[8];
    assert(lba==next_lba && n && n*d->block_size==len);
    next_lba+=n;
    assert(cmd[0]==(in?SCSI_READ_10:SCSI_WRITE_10));
    if(calls==fail_at)return K_EIO;
    if(in)memcpy(data,disk+(size_t)lba*d->block_size,len);
    else memcpy(disk+(size_t)lba*d->block_size,data,len);
    *actual=calls==short_at ? len-1:len;return K_OK;
}
''' + body + r'''
static void reset(unsigned lba) {
    calls=enters=leaves=fail_at=short_at=0;locked=deny=false;next_lba=lba;
}
int main(void) {
    for(unsigned size=512;size<=4096;size*=2) {
        usb_msc_device_t d={.block_size=size,.block_count=256};
        for(unsigned n=1;n<=129;++n) {
            for(unsigned i=0;i<sizeof(disk);++i)disk[i]=(uint8_t)(i*37+(i>>9));
            memset(buffer,0xa5,sizeof(buffer));
            reset(3);assert(usb_msc_block_read(&d,3,n,buffer));
            assert(!memcmp(buffer,disk+3*size,n*size));
            assert(buffer[n*size]==0xa5 && calls==(n+capacity/size-1)/(capacity/size));
            assert(enters==1 && leaves==1 && !locked);
            memcpy(expected,disk,sizeof(disk));
            memset(buffer,0x79,n*size);memcpy(expected+3*size,buffer,n*size);
            reset(3);assert(usb_msc_block_write(&d,3,n,buffer));
            assert(!memcmp(disk,expected,sizeof(disk)));
            assert(calls==(n+capacity/size-1)/(capacity/size) && leaves==1);
        }
        capacity=2*size;
        for(unsigned failure=1;failure<=3;++failure) {
            reset(0);fail_at=failure;
            assert(!usb_msc_block_read(&d,0,24,buffer) && calls==failure && leaves==1);
            reset(0);short_at=failure;
            assert(!usb_msc_block_write(&d,0,24,buffer) && calls==failure && leaves==1);
        }
        capacity=0;reset(0);assert(!usb_msc_block_read(&d,0,1,buffer) && leaves==1 && !calls);
        capacity=65536;
        reset(0);deny=true;assert(!usb_msc_block_read(&d,0,1,buffer) && !calls && !leaves);
        reset(0);assert(!usb_msc_block_read(&d,256,1,buffer));
        assert(!usb_msc_block_read(&d,255,2,buffer));
        assert(!usb_msc_block_read(&d,UINT64_MAX,2,buffer));
        assert(!usb_msc_block_read(&d,0,0,buffer) && !enters);
    }
    usb_msc_device_t full={.block_size=512,.block_count=2048};
    capacity=XHCI_MSC_MAX_TRANSFER;reset(0);
    assert(usb_msc_block_read(&full,0,2048,buffer) && calls==1 && leaves==1);
    assert(!memcmp(buffer,disk,1048576));
    usb_msc_device_t d={.block_size=0,.block_count=UINT64_MAX};reset(0);
    assert(!usb_msc_block_read(&d,0,1,buffer));
    d.block_size=8192;assert(!usb_msc_block_read(&d,0,1,buffer));
    d.block_size=512;
    assert(!usb_msc_block_read(&d,UINT32_MAX,2,buffer) && !enters);
    assert(!usb_msc_block_read(0,0,1,buffer));
    assert(!usb_msc_block_read(&d,0,1,0));
    assert(usb_msc_rw_blocks(&d,0,2049,buffer,false)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='usb-storage-batch-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
print('USB batching: 516 read/write sizes, exact data and guard bytes, bounded transfers, early stop, short writes, range overflow and balanced ownership PASS')
