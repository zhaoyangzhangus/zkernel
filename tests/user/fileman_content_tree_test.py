"""执行生产文件卡片构建器：完整 UTF-8、共享裁剪命中及容量边界。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/fileman/main.c').read_text()
# 公共模态描述可以使用待替换占位，实际提交不含 Surface，由导航测试检查。
assert 'OS_CONTROL_SURFACE' not in (root/'user/desktop/fileman/content_tree.h').read_text()
assert 'ui_modal_tree_attach' in source
assert 'ui_text_draw' not in source and 'g_target' not in source
assert 'fm4_damage_all' not in source

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

macros = '\n'.join(line for line in source.splitlines() if line.startswith((
    '#define FM4_GRID_', '#define FM4_SIDEBAR_WIDTH', '#define FM4_HEADER_HEIGHT',
    '#define FM4_FRAME_NODE_COUNT', '#define FM4_FRAME_TEXT_CAPACITY', '#define FM4_ITEM_ID_BASE', '#define FM4_SCALE(')))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/all.h>
#include <uapi/control_validation.h>
#include <uapi/utf8.h>
#include <uapi/window_theme.h>
#include <kernel/control_tree.h>
#define OS_TEXT_DEFAULT_SIZE 16U
static uint32_t font=16;
static uint32_t ui_text_size(void){return font;}
static uint32_t ui_text_line_height(void){return font*3U/2U;}
static uint32_t user_caption_height(void){return font+24U;}
static struct {uint32_t width,height;} g_window;
typedef struct {os_file_info_t info;} fileman_entry_t;
static fileman_entry_t entries[1200],*g_entries[1200];
static uint32_t g_first_entry,g_entry_count=1200,g_selected;
static bool g_list_view,g_selection_valid=true,g_delete_dialog;
static char g_query[128];
''' + macros + '\n'
for signature in ('static uint32_t text_length(', 'static void copy_text(',
                  'static void append_character(', 'static void append_text(',
                  'static void append_decimal(', 'static uint32_t fm4_measure_range(',
                  'static uint32_t fm4_measure(', 'static void fm4_fit_text(',
                  'static uint32_t fm4_content_left(', 'static uint32_t fm4_grid_top(',
                  'static uint32_t fm4_columns(', 'static uint32_t fm4_visible_rows(',
                  'static bool fm4_item_rect(', 'static bool fm4_frame_text('):
    program += function(signature)
program += (root / 'user/desktop/fileman/content_tree.h').read_text().replace('#pragma once', '')
program += r'''
static struct {os_control_node_t nodes[OS_CONTROL_TREE_MAX_NODES];uint64_t guard;} storage;
static char text[OS_CONTROL_TREE_MAX_TEXT];
static control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
int main(void){
 for(unsigned i=0;i<1200;++i){
  g_entries[i]=entries+i;entries[i].info.type=i%2?OS_FILE_TYPE_DIRECTORY:OS_FILE_TYPE_REGULAR;
  snprintf(entries[i].info.name,sizeof entries[i].info.name,"中文文件-%u-abcdefghijklmnopqrstuvwxyz.txt",i);
  entries[i].info.size=123456789U+i;
 }
 unsigned cases=0;const unsigned widths[]={1,180,320,640,960,1920,3840};
 const unsigned heights[]={1,240,640,1080,2160};
 for(font=14;font<=50;font+=4)for(unsigned mode=0;mode<2;++mode)
 for(unsigned wi=0;wi<7;++wi)for(unsigned hi=0;hi<5;++hi)for(unsigned page=0;page<2;++page){
  g_window.width=widths[wi];g_window.height=heights[hi];g_list_view=mode;
  g_first_entry=page?1180:0;g_selected=g_first_entry;g_delete_dialog=page;
  memset(storage.nodes,0,sizeof storage.nodes);storage.guard=0x123456789ABCDEF0ULL;
  storage.nodes[0]=(os_control_node_t){.identifier=1,.kind=OS_CONTROL_CONTAINER,
   .parent=OS_CONTROL_NO_PARENT,.flags=OS_CONTROL_CLIP_CHILDREN|OS_CONTROL_POINTER_PASSTHROUGH,
   .width=OS_CONTROL_FILL,.height=OS_CONTROL_FILL};
  unsigned count=1,bytes=0;
  assert(fm4_append_content(storage.nodes,text,&count,&bytes));
  assert(storage.guard==0x123456789ABCDEF0ULL);
  assert(os_control_tree_validate(storage.nodes,count,bytes));
  Rect viewport={0,0,g_window.width,g_window.height+user_caption_height()};
  assert(control_tree_layout(storage.nodes,count,bytes,viewport,boxes));
  unsigned cards=0;
  for(unsigned i=1;i<count;++i){
   const os_control_node_t *n=storage.nodes+i;
   assert(n->kind!=OS_CONTROL_SURFACE);
   if(n->kind==OS_CONTROL_LIST_ITEM){
    assert(!(n->flags&OS_CONTROL_DISABLED));
    ++cards;assert(n->user_data>=g_first_entry&&n->user_data<g_entry_count);
    assert(n->identifier==FM4_ITEM_ID_BASE+n->user_data);
    assert(!!(n->flags&OS_CONTROL_CHECKED)==(n->user_data==g_selected));
    if(boxes[i].visible&&boxes[i].enabled){
     int x=(boxes[i].bounds.x0+boxes[i].bounds.x1)/2,y=(boxes[i].bounds.y0+boxes[i].bounds.y1)/2;
     if(x<viewport.x1&&y<viewport.y1){
      control_tree_box_t hit;
      assert(control_tree_locate(storage.nodes,count,viewport,x,y,&hit)==i);
     }
    }
   }else{
    assert(n->flags&OS_CONTROL_POINTER_PASSTHROUGH);
    assert(storage.nodes[n->parent].kind==OS_CONTROL_LIST_ITEM);
    if(n->kind==OS_CONTROL_TEXT)for(size_t at=0;at<n->text_length;){
     size_t used=0;uint32_t code=os_utf8_next(text+n->text_offset+at,n->text_length-at,&used);
     assert(used&&code!=0xFFFDU);at+=used;
    }
   }
  }
  unsigned available=g_entry_count-g_first_entry,capacity=fm4_columns()*fm4_visible_rows();
  assert(cards==(available<capacity?available:capacity));++cases;
 }
 unsigned count=OS_CONTROL_TREE_MAX_NODES,bytes=0;
 assert(!fm4_append_content(storage.nodes,text,&count,&bytes));
 count=1;bytes=OS_CONTROL_TREE_MAX_TEXT;
 assert(!fm4_append_content(storage.nodes,text,&count,&bytes));
 assert(storage.guard==0x123456789ABCDEF0ULL);
 g_entry_count=0;count=1;bytes=0;
 assert(fm4_append_content(storage.nodes,text,&count,&bytes)&&count==2);
 assert(strstr(text,"文件夹为空"));
 printf("Fileman content: %u font/grid/list/viewport/page cases; no Surface, UTF-8, clipped metadata hits, modal state and capacity guards PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-content-') as directory:
    c = Path(directory) / 'test.c'
    c.write_text(program)
    binary = Path(directory) / 'test'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(c),
                    str(root / 'kernel/graphics/control_tree.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
