"""真实容器、解压、反滤波与像素转换，独立生成五种颜色格式的 PNG。"""
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

root = Path(__file__).resolve().parents[2]
decoder = (root/'kernel/graphics/png.c').read_text()
decoder = decoder.replace('#include <kernel/kmem.h>','').replace('#include "internal.h"','')
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct {
 void *storage;const uint8_t *pixels;uint32_t width,height,stride;
} desktop_asset_image_t;
static unsigned calls,live,fail_at;
static void *kmalloc(size_t n,unsigned flags) {
 assert(!flags);if(++calls==fail_at)return 0;
 void *p=malloc(n);if(p)++live;return p;
}
static void kfree(void *p){if(p){assert(live);--live;free(p);}}
'''
main = r'''
static uint8_t *load(const char *path,size_t *size) {
 FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));
 long n=ftell(f);assert(n>0);rewind(f);*size=(size_t)n;
 uint8_t *p=malloc(*size);assert(p&&fread(p,1,*size,f)==*size);assert(!fclose(f));return p;
}
int main(int argc,char **argv) {
 assert(argc==5);size_t encoded_size,expected_size;
 uint8_t *encoded=load(argv[1],&encoded_size),*expected=load(argv[2],&expected_size);
 unsigned width=(unsigned)strtoul(argv[3],0,10),height=(unsigned)strtoul(argv[4],0,10);
 desktop_asset_image_t asset={0};
 assert(desktop_png_decode(encoded,encoded_size,&asset));
 assert(live==1&&asset.width==width&&asset.height==height&&asset.stride==width*4);
 assert(expected_size==(size_t)width*height*4&&!memcmp(asset.pixels,expected,expected_size));
 kfree(asset.storage);asset=(desktop_asset_image_t){0};assert(!live);
 for(unsigned failure=1;failure<=3;++failure) {
  calls=0;fail_at=failure;
  assert(!desktop_png_decode(encoded,encoded_size,&asset)&&!live&&!asset.storage&&!asset.pixels);
 }
 fail_at=0;encoded[0]^=1;
 assert(!desktop_png_decode(encoded,encoded_size,&asset)&&!live&&!asset.storage);
 free(encoded);free(expected);
}
'''

def chunk(kind, content):
    return struct.pack('>I',len(content))+kind+content+struct.pack('>I',zlib.crc32(kind+content))

def paeth(a,b,c):
    p=a+b-c
    distances=[abs(p-a),abs(p-b),abs(p-c)]
    return (a,b,c)[distances.index(min(distances))]

def fixture(color,width,height,first_filter):
    channels={0:1,2:3,3:1,4:2,6:4}[color]
    palette=bytes((10,20,30,40,50,60,70,80,90,100,110,120))
    alpha=bytes((0,127,255,67))
    rows=[]
    for y in range(height):
        rows.append(bytes(((x*19+y*53+i*71)%256 if color!=3 else (x+y)%4)
                          for x in range(width) for i in range(channels)))
    transparent=rows[0][:channels]
    raw=bytearray()
    expected=bytearray()
    for y,row in enumerate(rows):
        mode=(first_filter+y)%5
        raw.append(mode)
        for i,value in enumerate(row):
            a=row[i-channels] if i>=channels else 0
            b=rows[y-1][i] if y else 0
            c=rows[y-1][i-channels] if y and i>=channels else 0
            prediction=(0,a,b,(a+b)//2,paeth(a,b,c))[mode]
            raw.append((value-prediction)&255)
        for x in range(width):
            p=row[x*channels:(x+1)*channels]
            if color==0:
                rgba=(p[0],)*3+(0 if p==transparent else 255,)
            elif color==2:
                rgba=tuple(p)+(0 if p==transparent else 255,)
            elif color==3:
                rgba=tuple(palette[p[0]*3:p[0]*3+3])+(alpha[p[0]],)
            elif color==4:
                rgba=(p[0],)*3+(p[1],)
            else:
                rgba=tuple(p)
            expected.extend(rgba)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,color,0,0,0))
    if color==3:
        png+=chunk(b'PLTE',palette)+chunk(b'tRNS',alpha)
    elif color in (0,2):
        png+=chunk(b'tRNS',b''.join(struct.pack('>H',value) for value in transparent))
    encoded=zlib.compress(raw)
    # 多个 IDAT 必须拼成同一条压缩流，不改变像素和透明度。
    for start in range(0,len(encoded),17):
        png+=chunk(b'IDAT',encoded[start:start+17])
    return png+chunk(b'IEND',b''),expected

with tempfile.TemporaryDirectory(prefix='png-decode-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+decoder+main)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/graphics'),
                    str(path/'test.c'),str(root/'kernel/graphics/png_chunks.c'),
                    '-o',str(path/'test')],check=True)
    count=0
    for color in (0,2,3,4,6):
        for width in (1,2,19):
            for first in range(5):
                png,expected=fixture(color,width,11,first)
                (path/'image.png').write_bytes(png)
                (path/'expected.rgba').write_bytes(expected)
                subprocess.run([str(path/'test'),str(path/'image.png'),str(path/'expected.rgba'),
                                str(width),'11'],check=True,timeout=10)
                count+=1
    print(f'PNG decode: {count} independent fixtures, five color types and filters, transparency, split IDAT, allocation failure and corrupt signature PASS')
