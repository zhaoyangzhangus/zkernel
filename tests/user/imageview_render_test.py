"""执行图片查看器真实渲染：透明 Surface、控件树及无变化时的零重绘。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/imageview/main.c').read_text()
state = source[source.index('#define IMAGEVIEW_DEFAULT_PATH'):source.index('static void copy_path(')]
body = source[source.index('static void clamp_pan('):source.index('static bool create_window(')]
prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <uapi/all.h>
#include <uapi/window_theme.h>
#include <uapi/control_validation.h>
#include <kernel/control_tree.h>
#include "user/runtime/file_picker.h"
#include "user/runtime/modal_tree.h"
#include "user/runtime/caption_tree.h"
static unsigned font=20, submissions, updates, test_width=4, test_height=40;
static bool fail_tree,fail_update;
unsigned ui_text_size(void){return font;}
unsigned ui_text_line_height(void){return font+4;}
static unsigned user_caption_height(void){return font+20<40?40:font+20;}
static int64_t __libc_native_syscall6(uint64_t n,uint64_t address,uint64_t b,
 uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 assert(!b&&!c&&!d&&!e&&!f);
 if(n==OS_SYS_WINDOW_CONTROL_TREE){
  os_window_control_tree_t *t=(void *)(uintptr_t)address;
  os_control_node_t *nodes=(void *)(uintptr_t)t->nodes;
  assert(t->count==11 && os_control_tree_validate(nodes,t->count,t->text_size));
  control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
  Rect viewport=control_tree_window_viewport(nodes,t->count,test_width,test_height,OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
  assert(control_tree_layout(nodes,t->count,t->text_size,viewport,boxes));
  assert(boxes[1].bounds.y0==0 && boxes[1].bounds.y1==(int)test_height);
  assert(nodes[2].flags&OS_CONTROL_CHECKERBOARD);
  assert(nodes[3].kind==OS_CONTROL_SURFACE && nodes[3].surface==1);
  assert(nodes[3].flags&OS_CONTROL_BACKGROUND_ALPHA);
  assert(nodes[5].kind==OS_CONTROL_TEXT && t->text_size>0);
  assert(nodes[0].flags&OS_CONTROL_WINDOW_FRAME);
  assert(nodes[6].kind==OS_CONTROL_BUTTON && nodes[6].identifier==7U);
  assert(nodes[6].text_length==6U && !memcmp((void *)(uintptr_t)(t->text+nodes[6].text_offset),"打开",6U));
  assert(nodes[7].kind==OS_CONTROL_TEXT && nodes[7].font_size==font &&
      nodes[7].text_length==sizeof("图片查看器")-1U &&
      !memcmp((void *)(uintptr_t)(t->text+nodes[7].text_offset),"图片查看器",sizeof("图片查看器")-1U));
  for(unsigned i=0;i<3;++i) {
   unsigned index=8U+i;Rect b=boxes[index].bounds;
   assert(nodes[index].kind==OS_CONTROL_WINDOW_BUTTON && nodes[index].value==OS_CONTROL_WINDOW_MINIMIZE+i);
   if(test_width>=3U) {
    assert(b.x0>=0 && b.x1<=(int)test_width && b.y0==-(int)user_caption_height() && b.y1==0);
    assert(control_tree_locate(nodes,t->count,viewport,(b.x0+b.x1)/2,(b.y0+b.y1)/2,0)==index);
   }
  }
  unsigned side=os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
  bool fits=test_width>=3U*side+2U*font+24U+16U;
  assert(((nodes[6].flags&OS_CONTROL_HIDDEN)==0)==fits);
  if(fits) {
   Rect b=boxes[6].bounds;
   assert(b.x1<=boxes[8].bounds.x0 && b.y0<0 && b.y1<=0);
   assert(control_tree_locate(nodes,t->count,viewport,(b.x0+b.x1)/2,(b.y0+b.y1)/2,0)==6U);
  }
  ++submissions;t->revision=submissions;
  return fail_tree?-1:0;
 }
 assert(n==OS_SYS_WINDOW_UPDATE);++updates;return fail_update?-1:0;
}
'''
main = r'''
int main(void){
 /* 页保护让任何重复清屏或图像重画立即失败，而非仅检查调用计数。 */
 uint32_t *pixels=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(pixels!=MAP_FAILED);
 unsigned char image[]={200,100,50,128, 20,40,60,255,
                        255,255,255,0, 10,20,30,255};
 g_window=(imageview_window_t){.identifier=4,.width=4,.height=40,.pixels=pixels};
 g_image_pixels=image;g_image_width=2;g_image_height=2;g_image_stride=8;g_zoom_percent=100;
 strcpy(g_path,"/alpha.png");
 for(unsigned i=0;i<160;++i)pixels[i]=0xdeadbeef;
 render();assert(updates==1&&submissions==1&&g_content_valid);
 /* 内容区高四像素，图像居中；状态区不允许应用写入。 */
 assert(pixels[5]==0x80643219 && pixels[6]==0xff14283c);
 assert(pixels[9]==0 && pixels[10]==0xff0a141e);
 assert(pixels[0]==0 && pixels[15]==0);
 for(unsigned i=16;i<160;++i)assert(pixels[i]==0xdeadbeef);
 assert(mprotect(pixels,65536,PROT_READ)==0);
 for(unsigned i=0;i<20;++i)render();
 assert(updates==1&&submissions==21);
 strcpy(g_path,"/renamed.png");render();assert(updates==1);
 assert(mprotect(pixels,65536,PROT_READ|PROT_WRITE)==0);
 /* 失败必须使缓存失效，下次重新提交图像，不能永久跳过发布。 */
 g_window.width=test_width=320;g_zoom_percent=200;fail_update=true;render();assert(!g_content_valid&&updates==2);
 fail_update=false;render();assert(g_content_valid&&updates==3);
 fail_tree=true;render();assert(!g_content_valid&&!g_tree.count&&updates==3);
 fail_tree=false;render();assert(g_content_valid&&updates==4);
 /* 字号和几何变化重新计算状态栏，极小窗口仍不能越界。 */
 for(font=8;font<=64;font+=8)for(unsigned size=1;size<=40;size+=13){
  g_window.width=test_width=size;g_window.height=test_height=size;g_content_valid=false;
  render();assert(g_content_valid);
  assert(g_nodes[4].y>=0 && (unsigned)g_nodes[4].y<=size);
  assert((unsigned)g_nodes[4].y+(unsigned)g_nodes[4].height==size);
 }
 reset_view();change_zoom(10000);assert(g_zoom_percent==400);
 change_zoom(-10000);assert(g_zoom_percent==10);
 (void)g_shift;(void)g_ctrl;(void)g_running;(void)g_picker;
 assert(munmap(pixels,65536)==0);
 puts("Image view: premultiplied alpha, tree layout, status isolation, read-only no-op, retry and tiny geometry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='imageview-render-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(prefix + state + body + main)
    exe = Path(directory) / 'test'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root), str(c),
                    str(root / 'kernel/graphics/control_tree.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
