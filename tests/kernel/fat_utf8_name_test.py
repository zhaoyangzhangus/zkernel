"""执行真实 FAT 名称编解码，覆盖中文、代理对、跨目录项和非法 UTF-8。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/nativefs/directory_codec.c').read_text().replace('#include "internal.h"','')
program=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;
typedef char CHAR8;typedef uint8_t BOOLEAN;
#define OS_FILE_NAME_MAX 256U
''' + source + r'''
static UINT32 encoded_length(const char *name,UINT32 length){
 UINT16 encoded[260];return fat32_encode_long_name(name,length,encoded);
}
static void roundtrip(const char *name){
 UINT16 encoded[260];
 UINT32 length=strlen(name),units=fat32_encode_long_name(name,length,encoded);
 assert(units && units<=255);
 UINT32 count=(units+12)/13;UINT8 entry[32];UINT16 decoded[260];
 BOOLEAN valid=0;UINT32 expected=0;UINT8 checksum=0;
 fat32_lfn_reset(decoded,&valid,&expected,&checksum);
 for(UINT32 ordinal=count;ordinal;--ordinal){
  fat32_make_lfn_entry(encoded,units,ordinal,0x5A,entry);
  fat32_lfn_store_entry(entry,decoded,&valid,&expected,&checksum);
 }
 assert(valid && expected==0 && checksum==0x5A);
 assert(fat32_lfn_units_equal(decoded,encoded,units));
 char text[256];assert(fat32_lfn_name_text(decoded,text)==length);
 assert(!strcmp(text,name));
}
int main(void){
 roundtrip("chrome-download.txt");roundtrip("下载 验证.bin");
 roundtrip("表情😀.png");roundtrip("abcdefghijkl😀跨项.txt");
 char maximum[256];memset(maximum,'x',255);maximum[255]=0;roundtrip(maximum);
 char chinese[256];for(unsigned i=0;i<85;++i)memcpy(chinese+3*i,"文",3);
 chinese[255]=0;roundtrip(chinese);assert(encoded_length(chinese,255)==85);
 const char *bad[]={"\x80","\xc0\xaf","\xe0\x80\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xf0\x9f","\xff"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
  assert(!encoded_length(bad[i],strlen(bad[i])));
 /* 扩展名与主文件名必须使用相同的短名称规则，否则空格会在枚举时丢失。 */
 UINT8 alias[11];
 const char *long_names[]={"a.t t","a.+","a.,","a.;","a.=","a.[","a.]"};
 for(unsigned i=0;i<sizeof(long_names)/sizeof(long_names[0]);++i){
  assert(!fat32_make_short_name(long_names[i],strlen(long_names[i]),alias));
  roundtrip(long_names[i]);
 }
 /* U+FFFF 是磁盘长名称的填充标记，不能作为实际文件名落盘。 */
 assert(!encoded_length("prefix\xef\xbf\xbf.txt",13));
 assert(!fat32_make_short_name(NULL,1,alias));
 assert(!fat32_make_short_name("a",1,NULL));
 assert(!fat32_make_short_name("中文.txt",10,alias));
 assert(fat32_make_long_name_alias("下载 验证.bin",strlen("下载 验证.bin"),1,alias));
 assert(!memcmp(alias,"_~1     BIN",11));
 assert(fat32_make_short_name("a.txt",5,alias)&&!memcmp(alias,"A       TXT",11));
 UINT16 invalid[260]={0xD800,0};char text[256];
 assert(fat32_lfn_name_text(invalid,text)==3&&!strcmp(text,"\xef\xbf\xbd"));
 puts("FAT UTF-8: Chinese/emoji, surrogate split, 255-byte boundaries, disk roundtrip, ASCII aliases and malformed input passed");
}
'''
with tempfile.TemporaryDirectory(prefix='fat-utf8-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True)
