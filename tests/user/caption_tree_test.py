"""真实应用标题栏构建器：左右布局、缩放、命中及失败时不修改文档。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/runtime/caption_tree.h"
#include "user/runtime/control_file.h"
#include <kernel/control_tree.h>
int main(void) {
 unsigned cases=0;
 for(unsigned left=0;left<2;++left) for(unsigned font=12;font<=96;font+=2)
 for(unsigned width=9;width<1700;width+=43) {
  os_control_node_t n[8]={{.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
    .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL,.font_size=font},
   {.identifier=2,.parent=0,.kind=OS_CONTROL_TEXT,.width=40,.height=20}};
  unsigned count=2,used=0,height=font+24;
  if(height<40)height=40;
  char text[128];
  assert(user_caption_tree(n,&count,8,text,&used,128,(user_caption_layout_t){
   .title="窗口",.width=width,.height=height,.font=font,.buttons_left=left}));
  assert(count==7&&n[0].identifier==1&&n[1].parent==0&&n[2].parent==1);
  assert(control_tree_validate(n,count,used));
  Rect viewport=control_tree_window_viewport(n,count,width,300,
   OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(height));
  control_tree_box_t boxes[8];assert(control_tree_layout(n,count,used,viewport,boxes));
  for(unsigned i=4;i<7;++i) {
   Rect b=boxes[i].bounds;
   assert(b.x0>=0&&b.x1<=(int)width&&b.y0==-(int)height&&b.y1==0);
   assert(control_tree_locate(n,count,viewport,(b.x0+b.x1)/2,(b.y0+b.y1)/2,0)==i);
   assert(n[i].value==OS_CONTROL_WINDOW_MINIMIZE+i-4);
  }
  assert(boxes[4].bounds.x0==(left?0:(int)width-3*n[4].width));
  assert(boxes[1].bounds.y0==0&&boxes[1].bounds.y1==300);
  ++cases;
 }
 os_control_node_t n[2]={{.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER}},saved[2];
 memcpy(saved,n,sizeof(n));unsigned count=1,used=0;char text[8];
 assert(!user_caption_tree(n,&count,2,text,&used,8,(user_caption_layout_t){.title="窗口",.width=640,.height=40,.font=16}));
 assert(count==1&&!used&&!memcmp(n,saved,sizeof(n)));
 /* 应用覆盖默认顺序并添加自己的标题栏按钮，不另建一条绘制路径。 */
 n[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 count=1;used=0;
 char custom_text[128];
 os_control_node_t custom[8];custom[0]=n[0];
 assert(user_caption_tree(custom,&count,8,custom_text,&used,128,(user_caption_layout_t){
  .title="窗口",.width=640,.height=40,.font=16,.buttons_left=true,
  .order={OS_CONTROL_WINDOW_CLOSE,OS_CONTROL_WINDOW_MINIMIZE,OS_CONTROL_WINDOW_MAXIMIZE}}));
 assert(custom[3].value==OS_CONTROL_WINDOW_CLOSE&&custom[4].value==OS_CONTROL_WINDOW_MINIMIZE&&custom[5].value==OS_CONTROL_WINDOW_MAXIMIZE);
 custom[count++]=(os_control_node_t){.identifier=42,.parent=0,.kind=OS_CONTROL_BUTTON,
  .x=240,.width=64,.height=40};
 assert(control_tree_validate(custom,count,used));
 assert(control_tree_locate(custom,count,(Rect){0,-40,640,300},260,-20,0)==count-1);
 const char *source="<container id=\"frame\" flags=\"window-frame\"><window-button id=\"close\" value=\"3\" width=\"32\" height=\"40\"/></container>";
 ui_file_document_t d={0};assert(ui_file_parse(source,strlen(source),&d,0));
 assert(d.nodes[1].kind==OS_CONTROL_WINDOW_BUTTON&&d.nodes[1].value==OS_CONTROL_WINDOW_CLOSE);
 ui_file_destroy(&d);
 printf("Caption tree: %u left/right scaled layouts and exact hits, bounded failure and UI file PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='caption-tree-') as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-pie','-no-pie','-I'+str(root),'-I'+str(root/'include'),str(p/'test.c'),
  str(root/'kernel/graphics/control_tree.c'),str(root/'user/runtime/control_file.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
