"""Actual editor layout, navigation and scrolling at small/large-font bounds."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/notepad/main.c').read_text()
def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <uapi/utf8.h>
static unsigned font_width=8,font_height=20,caption=40,status=32;
#define OS_TEXT_WIDTH font_width
#define OS_TEXT_HEIGHT font_height
#define USER_CLIENT_CHROME_HEIGHT caption
#define NOTEPAD_CONTENT_TOP 0U
#define NOTEPAD_STATUS_HEIGHT status
static struct {uint32_t width,height;} g_window;
static char g_text[8192];
static size_t g_cursor,g_text_length;
static uint32_t g_scroll_line;
static bool g_follow_cursor;
static unsigned layout_advances;
''' + '\n'.join((extract(signature).replace('{','{++layout_advances;',1) if signature=='static void advance_position(' else extract(signature)) for signature in (
    'static uint32_t editor_columns(', 'static uint32_t editor_visible_lines(',
    'static void prepare_position(', 'static void advance_position(', 'static void cursor_position(',
    'static size_t offset_at_position(', 'static void ensure_cursor_visible(',
    'static void move_vertical_by(', 'static void scroll_editor(',
    'static void page_editor(', 'static size_t editor_mouse_offset(')) + r'''
static uint32_t seed=0x73824;
static uint32_t random_value(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void oracle_advance(char c,unsigned columns,unsigned *line,unsigned *col){
 if(c=='\n'){++*line;*col=0;}
 else{
  unsigned next=c=='\t'?4*(*col/4+1):*col+1;
  if(next>=columns){++*line;*col=0;}else *col=next;
 }
}
static size_t oracle_offset(unsigned columns,unsigned target_line,unsigned target_col){
 unsigned line=0,col=0;
 for(size_t i=0;i<=g_text_length;++i){
  if(line==target_line&&col>=target_col)return i;
  if(i==g_text_length)break;
  if(line>target_line)return i-1;
  oracle_advance(g_text[i],columns,&line,&col);
 }
 return g_text_length;
}
int main(void){
 for(unsigned w=0;w<4096;++w)for(unsigned advance=1;advance<=96;++advance){
  unsigned expected=w>20?(w-20)/advance:0;
  assert(editor_columns(w,advance)==(expected?expected:1));
 }
 assert(editor_columns(UINT32_MAX,1)==UINT32_MAX-20&&editor_columns(40,0)==1);
 const unsigned limits[]={0,1,20,40,72,96,120,UINT32_MAX-1,UINT32_MAX};
 for(unsigned a=0;a<9;++a)for(unsigned b=0;b<9;++b)for(unsigned c=0;c<9;++c)
  for(unsigned cell=1;cell<=96;++cell){
   uint64_t occupied=(uint64_t)limits[b]+limits[c];
   unsigned expected=occupied<limits[a]?(unsigned)((limits[a]-occupied)/cell):0;
   assert(editor_visible_lines(limits[a],limits[b],limits[c],cell)==expected);
  }
 assert(!editor_visible_lines(1000,40,32,0));
 for(unsigned round=0;round<10000;++round){
  font_width=7+random_value()%32;font_height=16+random_value()%48;
  caption=0;status=0;
  g_window.width=21+random_value()%200;g_window.height=caption+status+font_height*4;
  unsigned columns=editor_columns(g_window.width,font_width);
  g_text_length=random_value()%256;
  for(size_t i=0;i<g_text_length;++i){unsigned v=random_value()%12;g_text[i]=v==0?'\n':v==1?'\t':'x';}
  g_cursor=random_value()%(g_text_length+1);
  unsigned line=0,col=0;
  for(size_t i=0;i<g_cursor;++i)oracle_advance(g_text[i],columns,&line,&col);
  int direction=round%2?1:-1;unsigned distance=random_value()%12;
  unsigned target=direction<0?(distance>line?0:line-distance):line+distance;
  size_t expected=oracle_offset(columns,target,col);
  move_vertical_by(direction,distance);assert(g_cursor==expected);
  g_window.height=caption+status+random_value()%(font_height*8);
  unsigned rows=(g_window.height-caption-status)/font_height;
  int x=(int)(random_value()%1000)-100,y=(int)(random_value()%1000)-100;
  g_scroll_line=random_value()%20;
  size_t mouse_expected=g_cursor;
  if(rows){
   int bottom=(int)(caption+rows*font_height);
   if(y<(int)caption)y=(int)caption;if(y>=bottom)y=bottom-1;
   unsigned mouse_column=x>10?((unsigned)(x-10)+font_width/2)/font_width:0;
   if(mouse_column>=columns)mouse_column=columns-1;
   mouse_expected=oracle_offset(columns,g_scroll_line+(unsigned)(y-(int)caption)/font_height,mouse_column);
  }
  assert(editor_mouse_offset(x,y)==mouse_expected);
  g_window.height=caption+status+font_height-1;g_scroll_line=17;g_follow_cursor=true;
  scroll_editor(3);scroll_editor(-3);page_editor(1);page_editor(-1);
  ensure_cursor_visible(0,0);assert(g_scroll_line==17&&g_follow_cursor);
  g_window.height=caption+status+font_height*4;g_scroll_line=0;
  scroll_editor(INT32_MAX);assert(!g_follow_cursor);
  scroll_editor(INT32_MIN);assert(g_scroll_line==0);
  unsigned full_line=0,full_column=0;
  for(size_t i=0;i<g_text_length;++i)oracle_advance(g_text[i],columns,&full_line,&full_column);
  unsigned maximum=full_line+1>4?full_line+1-4:0;
  const int32_t deltas[]={0,1,-1,INT32_MIN,INT32_MAX};
  for(unsigned d=0;d<5;++d){
   g_scroll_line=round&1?UINT32_MAX:random_value()%32;g_follow_cursor=true;
   uint32_t previous=g_scroll_line;int64_t target=(int64_t)previous+deltas[d];
   if(target<0)target=0;if(target>UINT32_MAX)target=UINT32_MAX;
   uint32_t expected_scroll=deltas[d]?(target>maximum?maximum:(uint32_t)target):previous;
   scroll_editor(deltas[d]);assert(g_scroll_line==expected_scroll);
   assert(g_follow_cursor==(deltas[d]==0));
  }

 }
 /* 中文、四字节字符和制表符的前缀换行，与完整 UTF-8 扫描的结果一致。 */
 strcpy(g_text,"a中😀b\t\n中a😀\n\t尾");g_text_length=strlen(g_text);font_width=8;font_height=20;
 for(unsigned columns=1;columns<=12;++columns)for(unsigned rows=1;rows<=4;++rows){
  g_window.width=20+columns*8;g_window.height=rows*20;
  unsigned full_line=0,full_column=0;
  for(size_t i=0;i<g_text_length;){size_t used;uint32_t ch=os_utf8_next(g_text+i,g_text_length-i,&used);
   advance_position(ch,columns,&full_line,&full_column);i+=used?used:1;}
  uint32_t maximum=full_line+1>rows?full_line+1-rows:0;
  const int32_t deltas[]={0,1,-1,INT32_MIN,INT32_MAX};
  for(unsigned top=0;top<8;++top)for(unsigned d=0;d<5;++d){
   g_scroll_line=top;int64_t target=(int64_t)top+deltas[d];
   if(target<0)target=0;if(target>UINT32_MAX)target=UINT32_MAX;
   scroll_editor(deltas[d]);assert(g_scroll_line==(deltas[d]?(target>maximum?maximum:(uint32_t)target):top));
  }
 }
 /* 大文档靠前滚动只需证实目标视口；返回顶部无需读取正文。 */
 memset(g_text,'\n',8000);g_text_length=8000;font_width=8;font_height=20;
 g_window.width=100;g_window.height=40;g_scroll_line=0;layout_advances=0;
 scroll_editor(1);assert(g_scroll_line==1&&layout_advances==2);
 layout_advances=0;scroll_editor(-1);assert(!g_scroll_line&&!layout_advances);
 layout_advances=0;scroll_editor(INT32_MAX);assert(g_scroll_line==7999&&layout_advances==8000);
 puts("Scroll prefix: 50000 ASCII and 1920 UTF-8 full-scan oracle comparisons; 8000-character near-top scan=2, return-top scan=0, end clamp scan=8000 PASS");
 puts("Editor layout: 393216 column cases, 69984 overflow-safe viewport cases and 10000 navigation/zero-row scrolling cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-editor-layout-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(ROOT/'include'), '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
