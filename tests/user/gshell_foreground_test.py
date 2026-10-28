"""Actual foreground pipe/wait loops must keep the GUI event pump alive."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/gshell/main.c').read_text()
body = source[source.index('static void foreground_window_event('):
              source.index('static bool launch_program(const char *path,')]
launch = source[source.index('static bool launch_program(const char *path,'):
                source.index('static void remember_history(')]
guard = source[source.index('static bool handle_key('):
               source.index('    if (g_output_scroll)', source.index('static bool handle_key('))]
guard = guard.replace('static bool handle_key(', 'static bool accepts_key(')
guard = guard.replace('    char character;', '')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "uapi/all.h"
#define SHELL_LINE_CAPACITY 256U
#define SHELL_FOREGROUND_WAIT_NS 20000000ULL
#define SHELL_OUTPUT_EVENT_BUDGET 32U
#define SHELL_MAX_ARGUMENTS 8U
#define SHELL_BACKGROUND_REAP_BUDGET 32U
typedef struct {char path[256];char argument_storage[8][256];char *arguments[9];os_handle_t output_read,output_write;bool redirect_output;} shell_launch_context_t;
static struct {uint32_t identifier;} g_shell={7};
static bool g_foreground_wait,g_ctrl,g_shift;
static uint32_t g_background_children;
static int reap_ready,reap_error,reap_calls;
static void copy_text(char *target,const char *source){strcpy(target,source);}
static char g_file_buffer[128];
static int scenario,reads,waits,events,renders,closed,mirrored,failures,exit_status;
static unsigned remaining,chunks_since_event,max_chunks;
static uint64_t consumed;
static void render_window(void){++renders;}
/* 前台输入检查不应触发剪贴板；误入这些分支时立即失败。 */
static void shell_copy_selection(void){assert(0);}
static void shell_paste_selection(void){assert(0);}
static bool handle_event(const os_window_event_t *e){assert(g_foreground_wait);assert(e->type==OS_WINDOW_EVENT_RESIZE);++events;return true;}
static void append_output(const char *s){(void)s;++failures;}
static bool append_text_chunk(const char *b,uint64_t n,char *line,uint32_t *length){(void)b;(void)line;(void)length;consumed+=n;return scenario<6;}
static bool finish_text_line(char *line,uint32_t *length){(void)line;(void)length;return scenario==7;}
static void append_status_line(const char *s,int64_t status){assert(!g_foreground_wait);assert(!strcmp(s,"PROGRAM EXIT STATUS "));exit_status=(int)status;}
static bool text_equals_ignore_case(const char *a,const char *b){return !strcmp(a,b);}
static void launch_program_entry(uint64_t p){(void)p;assert(0);}
static int64_t gshell_syscall_one(uint64_t call,uint64_t argument){
 if(call==OS_SYS_PIPE_CREATE){
  if(scenario==4)return -12;
  os_pipe_create_t *p=(void*)(uintptr_t)argument;p->read_handle=11;p->write_handle=12;return 0;
 }
 if(call==OS_SYS_PROCESS_FORK)return scenario==5?-12:42;
 if(call==OS_SYS_HANDLE_CLOSE){assert(argument==11||argument==12);++closed;return 0;}
 assert(call==OS_SYS_WINDOW_EVENT_READ&&g_foreground_wait);
 os_window_event_read_t *r=(void*)(uintptr_t)argument;
 assert(r->identifier==7);
 assert(r->timeout_ns==0||r->timeout_ns==SHELL_FOREGROUND_WAIT_NS);
 if(chunks_since_event>max_chunks)max_chunks=chunks_since_event;
 chunks_since_event=0;r->event.type=OS_WINDOW_EVENT_RESIZE;return 0;
}
static int64_t gshell_syscall_two(uint64_t call,uint64_t buffer,uint64_t bytes){
 assert(call==OS_SYS_DEBUG_WRITE&&buffer==(uint64_t)(uintptr_t)g_file_buffer&&bytes==128);++mirrored;return 0;
}
static int64_t gshell_syscall_three(uint64_t call,uint64_t pid,uint64_t options,uint64_t result){
 if(pid==UINT64_MAX){
  assert(call==OS_SYS_PROCESS_WAIT&&options==OS_PROCESS_WAIT_NOHANG&&!g_foreground_wait);
  ++reap_calls;
  if(reap_error)return reap_error;
  if(reap_ready){--reap_ready;return 100+reap_ready;}
  return 0;
 }
 assert(call==OS_SYS_PROCESS_WAIT&&pid==42&&options==OS_PROCESS_WAIT_NOHANG&&g_foreground_wait);
 ++waits;if(waits<=2)return 0;
 if(scenario==3)return -10;
 *(int32_t*)(uintptr_t)result=7<<8;return 42;
}
static int64_t gshell_syscall_five(uint64_t call,uint64_t handle,uint64_t buffer,
                                 uint64_t length,uint64_t timeout,uint64_t count){
 assert(call==OS_SYS_PIPE_READ&&handle==11&&buffer==(uint64_t)(uintptr_t)g_file_buffer);
 assert(length==128&&timeout==SHELL_FOREGROUND_WAIT_NS&&g_foreground_wait);
 ++reads;
 if(scenario==1&&reads<=3)return reads==2?-11:-110;
 if(scenario==6&&reads==10)return -110;
 if(scenario==2)return -5;
 if(remaining){--remaining;++chunks_since_event;*(uint64_t*)(uintptr_t)count=128;return 0;}
 *(uint64_t*)(uintptr_t)count=0;return 0;
}
''' + body + launch + guard + r'''
 return true;
}
int main(void){
 for(scenario=0;scenario<=7;++scenario){
  shell_launch_context_t context={.path="/usr/apps/chrome/chrome"};
  reads=waits=events=renders=closed=mirrored=failures=exit_status=0;
  remaining=65;consumed=chunks_since_event=max_chunks=0;g_foreground_wait=false;
  bool success=launch_program_foreground(&context);
  assert(success==(scenario<2||scenario>=6));
  assert(!g_foreground_wait);
  if(scenario==4||scenario==5){assert(!reads&&!waits&&!events);assert(closed==(scenario==5?2:0));continue;}
  assert(closed==2&&waits==3&&events>=2&&renders>=1);
  assert(!context.redirect_output&&context.output_read==OS_INVALID_HANDLE&&context.output_write==OS_INVALID_HANDLE);
  if(scenario!=2){assert(consumed==65*128&&mirrored==65&&max_chunks<=32);}
  if(scenario==1)assert(events>=7);
  if(scenario==6)assert(renders==1&&events==4&&max_chunks<=32);
  if(scenario==7)assert(renders==2&&events==4&&max_chunks<=32);
  assert(exit_status==(scenario==3?0:7));
 }
 char *arguments[]={"nasm","-v"};
 scenario=0;g_background_children=0;
 assert(!launch_program(0,arguments,2,false));
 assert(!launch_program("/usr/apps/nasm/nasm",arguments,0,false));
 for(unsigned i=0;i<100;++i)assert(launch_program("/usr/apps/nasm/nasm",arguments,2,true));
 assert(g_background_children==100);
 for(unsigned i=0;i<100;++i){
  waits=reads=0;remaining=0;g_foreground_wait=false;
  assert(launch_program("/usr/apps/nasm/nasm",arguments,2,false));
  assert(g_background_children==100);
 }
 scenario=5;assert(!launch_program("/usr/apps/nasm/nasm",arguments,2,true));
 assert(g_background_children==100);
 reap_ready=100;reap_calls=0;g_foreground_wait=true;
 reap_background_children();assert(!reap_calls&&g_background_children==100);
 g_foreground_wait=false;
 reap_background_children();assert(reap_calls==32&&g_background_children==68);
 reap_background_children();assert(reap_calls==64&&g_background_children==36);
 reap_background_children();assert(reap_calls==96&&g_background_children==4);
 reap_background_children();assert(reap_calls==100&&g_background_children==0);
 reap_background_children();assert(reap_calls==100);
 g_background_children=1;reap_ready=0;
 reap_background_children();assert(reap_calls==101&&g_background_children==1);
 reap_error=-11;reap_background_children();assert(reap_calls==102&&g_background_children==1);
 reap_error=-10;reap_background_children();assert(reap_calls==103&&g_background_children==0);
 reap_error=0;g_background_children=UINT32_MAX;
 assert(!launch_program("/usr/apps/nasm/nasm",arguments,2,true));
 os_window_event_t e={.type=OS_WINDOW_EVENT_INPUT};
 e.input.type=OS_INPUT_EVENT_KEY;e.input.value=OS_INPUT_VALUE_PRESS;
 g_foreground_wait=true;g_ctrl=false;
 for(unsigned code=0;code<256;++code){
  e.input.code=code;
  assert(!accepts_key(&e));
 }
 for(unsigned code=0;code<256;++code){
  g_ctrl=true;e.input.code=code;
  bool allowed=code==0x2e||code==0x2d||code==0x14||code=='Q';
  assert(accepts_key(&e)==allowed);
 }
 g_foreground_wait=false;e.input.code=0x28;assert(accepts_key(&e));
 puts("Shell lifecycle: responsive foreground, 100 foreground/background launches, bounded reap, PID ownership, no-child/transient errors and counter overflow passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-shell-foreground-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(ROOT / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
