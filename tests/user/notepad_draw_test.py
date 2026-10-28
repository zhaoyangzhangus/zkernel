"""Compare actual editor drawing operations to the pre-optimization implementation."""
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

ORACLE = r'''static void reference_ensure_cursor_visible(uint32_t columns, uint32_t visible_lines) {
    uint32_t line = 0U;
    uint32_t column = 0U;
    cursor_position(columns, &line, &column);
    (void)column;
    if (line < g_scroll_line) g_scroll_line = line;
    if (visible_lines != 0U && line >= g_scroll_line + visible_lines) {
        g_scroll_line = line - visible_lines + 1U;
    }
}

static void reference_draw_editor(void) {
    const uint32_t text_top = NOTEPAD_CONTENT_TOP;
    const uint32_t status_height = 0U;
    uint32_t columns = g_target_width > 20U ?
        (g_target_width - 20U) / OS_TEXT_WIDTH : 1U;
    uint32_t visible_lines = g_target_height > text_top + status_height ?
        (g_target_height - text_top - status_height) / OS_TEXT_HEIGHT : 1U;
    uint32_t line = 0U;
    uint32_t column = 0U;
    uint32_t cursor_line;
    uint32_t cursor_column;
    if (g_follow_cursor) reference_ensure_cursor_visible(columns, visible_lines);
    cursor_position(columns, &cursor_line, &cursor_column);
    fill_rect(0U, 0U, g_target_width, g_target_height,
              NOTEPAD_CONTENT_BACKGROUND);
    for (size_t index = 0U; index < g_text_length; ++index) {
        char character = g_text[index];
        size_t selection_start = g_selection_anchor < g_cursor ? g_selection_anchor : g_cursor;
        size_t selection_end = g_selection_anchor > g_cursor ? g_selection_anchor : g_cursor;
        if (g_selection_anchor != SIZE_MAX && index >= selection_start &&
            index < selection_end && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            uint32_t cells = character == '\t' ? 4U - (column & 3U) : 1U;
            if (cells > columns - column) cells = columns - column;
            fill_rect(10U + column * OS_TEXT_WIDTH,
                text_top + (line - g_scroll_line) * OS_TEXT_HEIGHT,
                cells * OS_TEXT_WIDTH, OS_TEXT_HEIGHT, OS_THEME_SELECTED);
        }
        if (character != '\n' && character != '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            draw_character(10U + column * OS_TEXT_WIDTH,
                           text_top + (line - g_scroll_line) * OS_TEXT_HEIGHT,
                           character, NOTEPAD_CONTENT_TEXT);
        }
        if (character == '\t' && line >= g_scroll_line &&
            line < g_scroll_line + visible_lines) {
            uint32_t spaces = 4U - (column & 3U);
            for (uint32_t space = 0U; space < spaces && column + space < columns; ++space) {
                draw_character(10U + (column + space) * OS_TEXT_WIDTH,
                               text_top + (line - g_scroll_line) * OS_TEXT_HEIGHT,
                               ' ', NOTEPAD_CONTENT_TEXT);
            }
        }
        advance_position(character, columns, &line, &column);
    }
    if (cursor_line >= g_scroll_line && cursor_line < g_scroll_line + visible_lines &&
        cursor_column < columns) {
        fill_rect(10U + cursor_column * OS_TEXT_WIDTH,
                  text_top + (cursor_line - g_scroll_line) * OS_TEXT_HEIGHT,
                  2U, OS_TEXT_HEIGHT, NOTEPAD_CURSOR);
    }
}
'''
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <uapi/utf8.h>
static char g_text[8192],g_file_path[256]="/notes.txt",g_status[128]="STATUS";
static size_t g_text_length,g_cursor,g_selection_anchor;
static uint32_t g_scroll_line,g_target_width,g_target_height;
static bool g_follow_cursor,g_dirty;
static uint32_t *g_target;
static unsigned width=8,height=20,caption=40,status=32;
static unsigned metrics,steps;
static uint32_t test_cell_width(void){++metrics;return width;}
static uint32_t test_cell_height(void){++metrics;return height;}
#define OS_TEXT_WIDTH test_cell_width()
#define OS_TEXT_HEIGHT test_cell_height()
#define USER_CLIENT_CHROME_HEIGHT caption
#define NOTEPAD_CONTENT_TOP 0U
#define NOTEPAD_STATUS_HEIGHT status
#define USER_CLIENT_CHROME_BACKGROUND 1U
#define USER_CLIENT_CHROME_SEPARATOR 2U
#define USER_CLIENT_CHROME_TITLE_Y 8U
#define USER_CLIENT_CHROME_TEXT 3U
#define USER_CLIENT_CHROME_CLOSE_BG 4U
#define USER_CLIENT_CHROME_CLOSE_FG 5U
#define NOTEPAD_CONTENT_BACKGROUND 6U
#define NOTEPAD_CONTENT_TEXT 7U
#define NOTEPAD_CURSOR 8U
#define NOTEPAD_STATUS_BACKGROUND 9U
#define NOTEPAD_STATUS_TEXT 10U
#define OS_THEME_SELECTED 11U
static uint32_t trace[32768][6],expected[32768][6];
static unsigned count;
static void record(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){
 assert(count<32768);uint32_t values[]={a,b,c,d,e,f};
 memcpy(trace[count++],values,sizeof(values));
}
static void fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t color){
 record(0,x,y,w,h,color);
}
static void draw_character(uint32_t x,uint32_t y,char c,uint32_t color){
 record(1,x,y,(unsigned char)c,color,0);
}
#define ui_text_advance() width
#define ui_text_draw_clipped(...) ((void)0)
#define user_client_chrome_close(...) ((void)0)
#define user_client_chrome_frame(...) ((void)0)
''' + extract('static uint32_t editor_columns(') + '\n' + extract(
    'static uint32_t editor_visible_lines(') + '\n' + extract('static void prepare_position(') + '\n' + extract('static void advance_position(').replace(
    "if (character == '\\n')", "++steps; if (character == '\\n')") + '\n' + extract(
    'static void cursor_position(') + '\n' + ORACLE + '\n' + extract(
    'static void ensure_cursor_visible(') + '\n' + extract('static void draw_editor(') + r'''
static uint32_t seed=0x531967;
static uint32_t random_value(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint64_t old_steps,new_steps,old_metrics,new_metrics;
static void compare(void){
 unsigned initial_scroll=g_scroll_line;
 count=metrics=steps=0;reference_draw_editor();
 unsigned expected_scroll=g_scroll_line,n=count,reference_steps=steps,reference_metrics=metrics;
 memcpy(expected,trace,n*sizeof(trace[0]));
 g_scroll_line=initial_scroll;count=metrics=steps=0;draw_editor();
 assert(g_scroll_line==expected_scroll&&count==n);
 assert(!memcmp(expected,trace,n*sizeof(trace[0])));
 assert(steps<=reference_steps);
 old_steps+=reference_steps;new_steps+=steps;
 old_metrics+=reference_metrics;new_metrics+=metrics;
}
int main(void){
 for(unsigned round=0;round<3000;++round){
  width=7+random_value()%24;height=16+random_value()%48;
  caption=0;status=0;
  g_target_width=20+width*(1+random_value()%100);
  // Compare unchanged layouts with at least one complete document row.
  // Zero-row layouts intentionally differ and are checked separately below.
  g_target_height=caption+status+(1+random_value()%25)*height+random_value()%height;
  g_text_length=random_value()%8192;
  for(size_t i=0;i<g_text_length;++i){
   unsigned value=random_value()%20;g_text[i]=value==0?'\n':value==1?'\t':(char)('a'+value);
  }
  g_text[g_text_length]=0;g_cursor=random_value()%(g_text_length+1);
  g_selection_anchor=round%3?random_value()%(g_text_length+1):SIZE_MAX;
  g_follow_cursor=round%2;g_scroll_line=random_value()%200;g_dirty=round%2;
  compare();
 }
 width=8;height=20;caption=0;status=0;
 g_target_width=420;g_target_height=120;g_text_length=8191;
 memset(g_text,'x',g_text_length);g_text[g_text_length]=0;
 g_cursor=0;g_selection_anchor=SIZE_MAX;g_follow_cursor=true;g_scroll_line=0;
 compare();assert(steps==300&&metrics==2);
 for(unsigned h=0;h<caption+status+height;++h){
  g_target_height=h;g_scroll_line=11;g_cursor=g_text_length;
  count=steps=metrics=0;draw_editor();
  assert(!steps&&g_scroll_line==11);
  for(unsigned i=0;i<count;++i){
   assert(trace[i][0]!=1); // No document glyphs.
   if(trace[i][0]==0)assert(trace[i][5]!=NOTEPAD_CURSOR&&trace[i][5]!=OS_THEME_SELECTED);
  }
 }
 printf("Editor draw: 3000 layouts and long-document viewport identical; position steps %llu -> %llu, metric lookups %llu -> %llu\n",
  (unsigned long long)old_steps,(unsigned long long)new_steps,
  (unsigned long long)old_metrics,(unsigned long long)new_metrics);
}
'''
with tempfile.TemporaryDirectory(prefix='system-editor-draw-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I'+str(ROOT/'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
