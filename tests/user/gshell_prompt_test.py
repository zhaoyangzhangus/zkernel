"""Actual shell rendering reserves an input row only when it accepts commands."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/gshell/main.c').read_text()
assert 'append_output("LITEOS' not in source, 'Shell output must not contain project branding'
prompt_code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define SHELL_LINE_CAPACITY 256U
#define SHELL_DISPLAY_CAPACITY 528U
static char g_cwd[256] = "/";
static char g_command[256],g_display_line[528],echoed[528];
static uint32_t g_target_width=1440,echo_count;
static uint32_t shell_text_width(const char *s,size_t n){(void)s;return (uint32_t)n*8U;}
static void append_output(const char *s){
 assert(strlen(s)<SHELL_LINE_CAPACITY);
 assert(((uint8_t)s[0]&0xC0U)!=0x80U);
 assert(strlen(echoed)+strlen(s)<sizeof(echoed));
 strcat(echoed,s);++echo_count;
}
''' + source[source.index('static void append_text('):
             source.index('static void append_character(')] + source[
                 source.index('static void build_prompt('):
                 source.index('static uint32_t shell_text_width(')] + source[
                     source.index('static void append_command_echo('):
                     source.index('static void append_history_line(')] + r'''
int main(void) {
 char prompt[SHELL_DISPLAY_CAPACITY];
 build_prompt(0);
 build_prompt(prompt); assert(!strcmp(prompt,"/# "));
 strcpy(g_cwd,"/tmp");
 build_prompt(prompt); assert(!strcmp(prompt,"/tmp# "));
 strcpy(g_cwd,"/home/user/projects");
 build_prompt(prompt); assert(!strcmp(prompt,"/home/user/projects# "));
 memset(g_cwd,'x',255);g_cwd[0]='/';g_cwd[255]=0;
 build_prompt(prompt);assert(strlen(prompt)==257);
 assert(!memcmp(prompt,g_cwd,255)&&!strcmp(prompt+255,"# "));
 assert(!strstr(prompt,"LITEOS"));
 assert(shell_display_length(prompt)==257);
 char command[256];memset(command,'c',255);command[255]=0;
 append_text(prompt,SHELL_DISPLAY_CAPACITY,command);
 assert(shell_display_length(prompt)==512);
 memset(prompt,'x',sizeof(prompt));
 assert(shell_display_length(prompt)==SHELL_DISPLAY_CAPACITY-1);
 assert(shell_display_length(0)==0);
 unsigned cases=0;
 const unsigned widths[]={0,8,20,28,60,1440,4096};
 for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);++w)
 for(unsigned path_length=1;path_length<256;path_length+=3)
 for(unsigned command_length=0;command_length<256;command_length+=3){
  memset(g_cwd,'p',path_length);g_cwd[0]='/';g_cwd[path_length]=0;
  memset(g_command,'c',command_length);g_command[command_length]=0;
  echoed[0]=0;echo_count=0;g_target_width=widths[w];
  build_prompt(prompt);append_text(prompt,sizeof(prompt),g_command);
  append_command_echo();assert(!strcmp(echoed,prompt)&&echo_count>0);
  if(widths[w]>20){
   unsigned capacity=(widths[w]-20)/8;
   if(capacity>255)capacity=255;
   assert(echo_count==(strlen(prompt)+capacity-1)/capacity);
  }
  ++cases;
 }
 strcpy(g_cwd,"/");
 for(unsigned n=0;n<80;++n)memcpy(g_command+n*3,"\xe4\xb8\xad",3);
 g_command[240]=0;
 for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);++w){
  echoed[0]=0;echo_count=0;g_target_width=widths[w];
  build_prompt(prompt);append_text(prompt,sizeof(prompt),g_command);
  append_command_echo();assert(!strcmp(echoed,prompt));
 }
 printf("Shell prompt/echo: %u path/command/viewport combinations retain all bytes; UTF-8 and tiny viewports passed\n",cases);
}
'''
draw = source[source.index('static uint32_t shell_scroll_maximum('):
              source.index('static void update_window(void)')]
mouse_start = source.index('static bool handle_shell_mouse(')
offsets = source[source.index('static size_t shell_mouse_text_offset('):mouse_start]
mouse = source[mouse_start:source.index('static bool shell_control_event_valid(', mouse_start)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "uapi/window.h"
#define USER_CLIENT_CHROME_BACKGROUND 1U
#define USER_CLIENT_CHROME_SEPARATOR 2U
#define USER_CLIENT_CHROME_TEXT 3U
#define SHELL_OUTPUT_TEXT 4U
#define OS_THEME_SELECTED 5U
#define SHELL_LINE_CAPACITY 256U
#define SHELL_DISPLAY_CAPACITY 528U
#define SHELL_OUTPUT_LINES 128U
#define OS_TEXT_HEIGHT line_metric()
#define user_client_chrome_close(...) ((void)0)
#define user_client_chrome_frame(...) ((void)0)
static struct {uint32_t color,height;} g_shell;
static bool g_foreground_wait;
static uint32_t g_target_width=800,g_target_height,g_output_count,g_output_scroll;
static uint32_t g_view_first,g_view_lines,g_input_y;
static char g_output[128][256];
static uint32_t g_output_head;
static size_t g_output_anchor=SIZE_MAX,g_output_selection_end;
static unsigned prompts,output_rows;
static bool g_ctrl,g_shift;
static uint32_t g_mouse_selecting;
static size_t g_input_anchor=SIZE_MAX,g_command_cursor;
static uint32_t g_input_view_start,g_prompt_length;
static size_t g_command_length;
static char g_display_line[SHELL_DISPLAY_CAPACITY];
static unsigned renders;
static unsigned caption_height,line_height,metric_queries;
static uint32_t line_metric(void){++metric_queries;return line_height;}
static void fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){(void)x;(void)y;(void)w;(void)h;(void)c;}
static void draw_text(uint32_t x,uint32_t y,const char *t,uint32_t c){(void)x;(void)y;(void)t;if(c==SHELL_OUTPUT_TEXT)++output_rows;}
static uint32_t shell_text_width(const char *t,size_t n){(void)t;return n*8;}
static size_t text_length(const char *t){return strlen(t);}
static void draw_input_line(void){assert(!g_foreground_wait);++prompts;}
''' + source[source.index('static char *shell_output_line('):
             source.index('static void append_output(')] + draw + r'''
static void render_window(void){++renders;draw_terminal();}
''' + source[source.index('static uint32_t shell_display_length('):
             source.index('static uint32_t shell_text_width(')] + offsets + mouse + r'''
int main(void){
 const unsigned heights[]={14,19,20,59};
 unsigned cases=0;
 for(unsigned font=0;font<4;++font){
 line_height=heights[font];caption_height=0;
 for(unsigned busy=0;busy<2;++busy)
 for(unsigned visible=0;visible<50;++visible)
 for(unsigned count=0;count<128;++count){
  g_foreground_wait=busy;g_target_height=caption_height+visible*line_height;
  g_output_count=count;g_output_scroll=0;prompts=output_rows=0;
  metric_queries=0;draw_terminal();assert(metric_queries==1);
  unsigned rows=count+(!busy&&visible?1:0);
  unsigned first=rows>visible?rows-visible:0;
  assert(g_view_first==first&&g_view_lines==visible);
  unsigned expected=count>first?count-first:0;
  assert(output_rows==expected);
  assert(g_view_first<=g_output_count);
  assert(g_input_y==caption_height+expected*line_height&&g_input_y<=g_target_height);
  assert(prompts==(!busy&&visible>0?1U:0U));
  g_output_scroll=UINT32_MAX;prompts=output_rows=0;
  metric_queries=0;draw_terminal();assert(metric_queries==1);
  assert(g_view_first==0);
  assert(g_output_scroll==(rows>visible?rows-visible:0));
  assert(output_rows==(count<visible?count:visible));
  assert(g_input_y==caption_height+output_rows*line_height&&g_input_y<=g_target_height);
  assert(prompts==(!busy&&count<visible?1U:0U));
  unsigned maximum=rows>visible?rows-visible:0;
  const uint32_t initial[]={0,maximum,UINT32_MAX};
  for(unsigned n=0;n<3;++n)for(int direction=-1;direction<=1;++direction){
   g_output_scroll=initial[n];unsigned wanted=initial[n]>maximum?maximum:initial[n];
   if(direction>0)wanted=maximum-wanted>3?wanted+3:maximum;
   if(direction<0)wanted=wanted>3?wanted-3:0;
   assert(shell_scroll_output(direction)==(wanted!=initial[n]));
   assert(g_output_scroll==wanted&&g_output_scroll<=maximum);
  }
  os_window_event_t event={0};
  event.input.type=OS_INPUT_EVENT_RELATIVE;event.input.code=OS_INPUT_REL_WHEEL;
  event.input.value=1;g_output_scroll=maximum;renders=0;
  assert(handle_shell_mouse(&event)&&!renders);
  event.input.value=-1;g_output_scroll=0;
  assert(handle_shell_mouse(&event)&&!renders);
  event.input.value=1;g_output_scroll=0;
  assert(handle_shell_mouse(&event));
  assert(renders==(maximum&&visible?1U:0U));
  g_ctrl=true;renders=0;
  assert(!handle_shell_mouse(&event)&&!renders);g_ctrl=false;
  ++cases;
 }
 }
 g_foreground_wait=false;g_output_count=128;line_height=59;caption_height=0;
 for(unsigned height=0;height<caption_height+line_height;++height){
  g_target_height=height;g_output_scroll=0;prompts=output_rows=0;g_mouse_selecting=2;
  draw_terminal();
  assert(g_view_first==128&&g_view_lines==0&&g_input_y==0&&!prompts&&!output_rows&&!g_mouse_selecting);
  assert(shell_mouse_output_offset(INT32_MAX,INT32_MAX)==0);
  os_window_event_t event={0};
  event.input.type=OS_INPUT_EVENT_BUTTON;event.input.code=OS_INPUT_BUTTON_LEFT;
  event.input.value=OS_INPUT_VALUE_PRESS;event.pointer_y=76;g_shell.height=height;
  renders=0;
  assert(!handle_shell_mouse(&event)&&!g_mouse_selecting&&!renders);
 }
 line_height=0;g_target_height=800;g_output_scroll=0;prompts=output_rows=0;
 draw_terminal();assert(g_view_lines==0&&!prompts&&!output_rows&&g_input_y==0);
 assert(shell_mouse_output_offset(INT32_MAX,INT32_MAX)==0);
 printf("Shell viewport: %u idle/busy/font/viewport cases, zero-height bounds, cached metrics, wheel clamps and unchanged-scroll suppression passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='system-shell-prompt-') as directory:
    prompt_binary = str(Path(directory) / 'prompt')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', prompt_binary],
                   input=prompt_code, text=True, check=True)
    subprocess.run([prompt_binary], check=True)
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(ROOT / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
