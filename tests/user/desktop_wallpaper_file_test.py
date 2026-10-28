"""执行真实 UI 解析与壁纸路径替换，验证特殊文件名及其余原文保留。"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
for name in ('imageview', 'wallpaper'):
    source=(root/'user/desktop'/name/'main.c').read_text()
    assert '"/usr/apps/desktop/wall.png"' in source, name
    assert '/etc/desktop/wall.png' not in source, name
program=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user/runtime/control_file.h"
int main(void) {
 const char *prefix="<!-- 保留注释 -->\n<container id='desktop' wallpaper='";
 const char *suffix="' background='surface'><text id='label' text='说明 &amp; 名称'/></container>\n";
 char source[1024];snprintf(source,sizeof(source),"%s/usr/apps/desktop/wall.png%s",prefix,suffix);
 const char *paths[]={"/usr/apps/desktop/wall.png","/图片/我的壁纸.png","/tmp/a&b'\"<>.png"};
 for(unsigned i=0;i<3;++i){
  char *next=NULL;size_t size=0;
  assert(ui_file_set_wallpaper(source,strlen(source),paths[i],&next,&size));
  assert(!memcmp(next,prefix,strlen(prefix)));
  assert(size>=strlen(suffix)&&!memcmp(next+size-strlen(suffix),suffix,strlen(suffix)));
  assert(next[size]==0);
  ui_file_document_t d={0};char path[256];
  assert(ui_file_parse(next,size,&d,0)&&ui_file_wallpaper_path(&d,path,sizeof(path)));
  assert(!strcmp(path,paths[i]));
  assert(d.request.count==2&&d.nodes[1].text_length==strlen("说明 & 名称"));
  assert(!memcmp(d.text+d.nodes[1].text_offset,"说明 & 名称",d.nodes[1].text_length));
  char unchanged[2]={'a','b'};assert(!ui_file_wallpaper_path(&d,unchanged,sizeof(unchanged)));
  assert(unchanged[0]=='a'&&unchanged[1]=='b');
  ui_file_destroy(&d);free(next);
 }
 const char *invalid[]={"relative.png","/","/tmp/../wall.png","/tmp/./wall.png","/tmp//wall.png","/tmp/wall.png/","/tmp/\n.png"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){
  char *out=NULL;size_t size=123;
  assert(!ui_file_set_wallpaper(source,strlen(source),invalid[i],&out,&size));assert(!out&&size==123);
 }
 const char *nested="<container><text wallpaper='/tmp/a.png'/></container>";
 const char *duplicate="<container wallpaper='/tmp/a.png' wallpaper='/tmp/b.png'/>";
 ui_file_document_t d={0};
 assert(!ui_file_parse(nested,strlen(nested),&d,0));
 assert(!ui_file_parse(duplicate,strlen(duplicate),&d,0));
 assert(ui_file_load("user/ui/desktop.ui",&d,0));char path[256];
 assert(ui_file_wallpaper_path(&d,path,sizeof(path))&&!strcmp(path,"/usr/apps/desktop/wall.png"));
 assert(d.resource_count==7);ui_file_destroy(&d);
 puts("Wallpaper UI: root resource, exact source preservation, Unicode/entities, invalid paths and desktop application bindings PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='wallpaper-ui-') as directory:
    work=Path(directory);(work/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root),'-I'+str(root/'include'),
                    str(work/'test.c'),str(root/'user/runtime/control_file.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],cwd=root,check=True)
