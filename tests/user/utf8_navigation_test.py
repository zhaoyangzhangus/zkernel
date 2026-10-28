"""全 Unicode 标量编码与 Python 对照，并检查损坏字节串的双向边界。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
expected = 2166136261
for code in range(1, 0x110000):
    if 0xD800 <= code <= 0xDFFF:
        continue
    for byte in chr(code).encode('utf-8'):
        expected = ((expected ^ byte) * 16777619) & 0xFFFFFFFF

program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <uapi/utf8.h>
int main(void){
 uint32_t hash=2166136261U;
 for(uint32_t code=1;code<=0x10ffffU;++code){
  if(code>=0xd800U&&code<=0xdfffU)continue;
  char bytes[6]={(char)0x27,0,0,0,0,(char)0x39};
  size_t count=os_utf8_encode(code,bytes+1),used=0;
  assert(bytes[0]==0x27&&bytes[5]==0x39);
  assert(count==(code<0x80?1U:code<0x800?2U:code<0x10000?3U:4U));
  assert(os_utf8_next(bytes+1,count,&used)==code&&used==count);
  assert(os_utf8_advance(bytes+1,count,0)==count);
  assert(os_utf8_previous(bytes+1,count,count)==0);
  for(size_t i=0;i<count;++i)hash=(hash^(uint8_t)bytes[i+1])*16777619U;
 }
 uint32_t seed=0xa87152U;
 for(unsigned sample=0;sample<100000;++sample){
  char text[64];size_t positions[65],count=0,offset=0,length=sample%65;
  for(size_t i=0;i<length;++i){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;text[i]=(char)seed;}
  positions[count++]=0;
  while(offset<length){size_t next=os_utf8_advance(text,length,offset);assert(next>offset&&next<=length);
   positions[count++]=next;offset=next;}
  while(count>1){assert(os_utf8_previous(text,length,offset)==positions[count-2]);offset=positions[--count-1];}
  assert(os_utf8_previous(text,length,0)==0&&os_utf8_advance(text,length,length+1)==length);
 }
 char out[4];size_t used;
 assert(os_utf8_encode(0xd800,out)==3&&os_utf8_next(out,3,&used)==0xfffd);
 assert(os_utf8_encode(0x110000,out)==3&&os_utf8_next(out,3,&used)==0xfffd);
 printf("%u\n",hash);
}
'''
with tempfile.TemporaryDirectory(prefix='utf8-navigation-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(program)
    exe = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(c), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert int(result.stdout) == expected, (result.stdout, expected)
print('UTF-8: every Unicode scalar matches Python encoding; 100000 malformed buffers, bounds and bidirectional navigation PASS')
