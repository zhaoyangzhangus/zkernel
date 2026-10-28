"""Production mouse positioning, selection replacement and inline shell layout."""
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

root = Path(__file__).resolve().parents[2]
def extract(file, signature):
    source = (root / file).read_text()
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <uapi/window.h>
#include <uapi/utf8.h>
#define OS_TEXT_WIDTH 8U
#define OS_TEXT_HEIGHT 20U
static uint32_t test_caption_height=0U,status_height=0U;
#define NOTEPAD_CONTENT_TOP 0U
#define USER_CLIENT_CHROME_HEIGHT test_caption_height
#define NOTEPAD_STATUS_HEIGHT status_height
'''
note = common + r'''
#define NOTEPAD_TEXT_CAPACITY 128U
static struct { uint32_t width,height; } g_window={100,172};
static char g_text[NOTEPAD_TEXT_CAPACITY];
static size_t g_text_length,g_cursor,g_selection_anchor=SIZE_MAX;
static uint32_t g_scroll_line;
static bool g_follow_cursor,g_shift,g_dirty,g_mouse_selecting;
static unsigned edits,renders,hit_queries;
static void begin_edit(void){++edits;}
static void set_status(const char *s){(void)s;}
static void render(void){++renders;}
static void scroll_editor(int32_t delta){if(delta>0)++g_scroll_line;else if(g_scroll_line)--g_scroll_line;}
''' + '\n'.join((extract('user/desktop/notepad/main.c', signature).replace('{', '{++hit_queries;',1) if signature=='static size_t editor_mouse_offset(' else extract('user/desktop/notepad/main.c', signature)) for signature in (
    'static uint32_t editor_columns(', 'static uint32_t editor_visible_lines(',
    'static void prepare_position(', 'static void advance_position(', 'static size_t offset_at_position(',
    'static bool has_selection(', 'static void erase_selection(',
    'static void insert_character(', 'static void backspace(',
    'static void delete_character(', 'static size_t editor_mouse_offset(',
    'static bool handle_editor_mouse(')) + r'''
int main(void) {
 strcpy(g_text,"abc\ndef\tz\n");g_text_length=strlen(g_text);
 os_window_event_t e={.input={.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS},.pointer_x=26,.pointer_y=0};
 assert(handle_editor_mouse(&e) && g_cursor==2 && g_selection_anchor==2);
 assert(hit_queries==1);
 e.input.type=OS_INPUT_EVENT_RELATIVE;e.input.code=OS_INPUT_REL_X;e.pointer_x=42;e.pointer_y=20;
 assert(handle_editor_mouse(&e) && g_cursor==8);
 e.input.type=OS_INPUT_EVENT_BUTTON;e.input.code=OS_INPUT_BUTTON_LEFT;e.input.value=OS_INPUT_VALUE_RELEASE;
 assert(handle_editor_mouse(&e) && !g_mouse_selecting);
 test_caption_height=0;status_height=0;g_window.height=260;g_scroll_line=0;
 assert(editor_mouse_offset(10,0)==0);
 assert(editor_mouse_offset(10,20)==4);
 e.input.value=OS_INPUT_VALUE_PRESS;e.pointer_y=-1;
 assert(!handle_editor_mouse(&e)); // server caption is outside client coordinates
 test_caption_height=0;status_height=0;g_window.height=172;
 insert_character('X');assert(!strcmp(g_text,"abXz\n") && edits==1 && g_cursor==3);
 g_selection_anchor=g_cursor;backspace();assert(g_selection_anchor==SIZE_MAX && !strcmp(g_text,"abz\n"));
 g_selection_anchor=0;g_cursor=g_text_length;delete_character();assert(!g_text_length && !g_text[0]);
 strcpy(g_text,"abc\ndef");g_text_length=strlen(g_text);
 assert(editor_mouse_offset(-100,-100)==0);
 assert(editor_mouse_offset(100000,100000)==g_text_length);
 g_scroll_line=1;assert(editor_mouse_offset(10,0)==4);
 g_cursor=5;g_shift=true;g_selection_anchor=SIZE_MAX;e.input.value=OS_INPUT_VALUE_PRESS;e.pointer_x=10;e.pointer_y=0;
 assert(handle_editor_mouse(&e) && g_selection_anchor==5 && g_cursor==4);
 e.input.value=OS_INPUT_VALUE_RELEASE;e.pointer_x=-200;e.pointer_y=-200;
 assert(handle_editor_mouse(&e) && !g_mouse_selecting);
 g_cursor=3;g_window.height=test_caption_height+status_height+OS_TEXT_HEIGHT-1;
 assert(editor_mouse_offset(-100,-100)==3&&editor_mouse_offset(100000,100000)==3);
 e.input.value=OS_INPUT_VALUE_PRESS;e.pointer_x=26;e.pointer_y=(int32_t)test_caption_height;
 assert(!handle_editor_mouse(&e)&&!g_mouse_selecting);
 g_mouse_selecting=true;e.input.value=OS_INPUT_VALUE_RELEASE;
 assert(handle_editor_mouse(&e)&&!g_mouse_selecting&&g_cursor==3);
 g_scroll_line=0;g_window.height=test_caption_height+status_height+2*OS_TEXT_HEIGHT-1;
 assert(editor_mouse_offset(10,(int32_t)(test_caption_height+OS_TEXT_HEIGHT+2))==0);
 assert(editor_mouse_offset(26,100000)==2); // Partial final row maps to the last complete row.
 /* 端点未变但视口滚动仍须重绘；原地移动和松开不重建相同选区。 */
 strcpy(g_text,"abc\ndef\nghi");g_text_length=strlen(g_text);
 g_window.height=40;g_scroll_line=0;g_cursor=g_text_length;g_selection_anchor=0;g_mouse_selecting=true;
 unsigned before=renders;
 e.input=(os_input_event_t){.type=OS_INPUT_EVENT_RELATIVE,.code=OS_INPUT_REL_Y};
 e.pointer_x=10000;e.pointer_y=40;
 assert(handle_editor_mouse(&e)&&g_cursor==g_text_length&&g_scroll_line==1&&renders==before+1);
 e.pointer_y=20;e.input.code=OS_INPUT_REL_X;before=renders;
 assert(handle_editor_mouse(&e)&&renders==before);
 e.input=(os_input_event_t){.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_RELEASE};
 assert(handle_editor_mouse(&e)&&!g_mouse_selecting&&renders==before);
 e.input.value=OS_INPUT_VALUE_PRESS;g_shift=false;hit_queries=0;
 assert(handle_editor_mouse(&e)&&hit_queries==1&&renders==before+1&&g_selection_anchor==g_cursor);
 before=renders;e.input.value=OS_INPUT_VALUE_RELEASE;
 assert(handle_editor_mouse(&e)&&renders==before);
 /* 中文双列和四字节字符的点击、删除必须始终停在 UTF-8 字符边界。 */
 strcpy(g_text,"a中😀b");g_text_length=strlen(g_text);g_scroll_line=0;g_window.height=172;
 const size_t offsets[]={0,1,4,4,8,8,9};
 for(unsigned column=0;column<7;++column)
  assert(editor_mouse_offset(10+(int32_t)column*8,0)==offsets[column]);
 g_selection_anchor=SIZE_MAX;g_cursor=8;backspace();
 assert(!strcmp(g_text,"a中b") && g_cursor==4);
 g_cursor=1;delete_character();assert(!strcmp(g_text,"ab") && g_cursor==1);
}
'''
shell = common + r'''
#include <uapi/window_theme.h>
#define SHELL_LINE_CAPACITY 256U
#define SHELL_DISPLAY_CAPACITY 528U
#define SHELL_OUTPUT_LINES 128U
#define SHELL_INPUT_TEXT OS_THEME_TEXT
#define SHELL_CURSOR OS_THEME_ACCENT
#define SHELL_OUTPUT_TEXT OS_THEME_TEXT
#define USER_CLIENT_CHROME_BACKGROUND OS_THEME_HEADER
#define USER_CLIENT_CHROME_SEPARATOR OS_THEME_BORDER
#define USER_CLIENT_CHROME_TITLE_Y 8U
#define USER_CLIENT_CHROME_ICON_TERMINAL 1U
#define USER_CLIENT_CHROME_TEXT OS_THEME_TEXT
#define USER_CLIENT_CHROME_CLOSE_BG OS_THEME_HEADER
#define USER_CLIENT_CHROME_CLOSE_FG OS_THEME_MUTED
static struct {uint32_t width,height,color;} g_shell={320,200,OS_THEME_SURFACE};
static char g_command[256],g_display_line[528],g_output[128][256];
static size_t g_command_length,g_command_cursor,g_input_anchor=SIZE_MAX;
static size_t g_output_anchor=SIZE_MAX,g_output_selection_end;
static uint32_t g_output_count,g_output_scroll,g_view_first,g_view_lines,g_input_y,g_input_view_start,g_prompt_length;
static uint32_t g_output_head;
static bool g_foreground_wait;
static uint32_t g_mouse_selecting;
static uint32_t *g_target,g_target_width=320,g_target_height=200;
static unsigned painted_prompts;
static uint32_t text_length(const char*s){return (uint32_t)strlen(s);}
static uint32_t shell_text_width(const char*s,size_t n){(void)s;return (uint32_t)n*8U;}
static void fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){(void)x;(void)y;(void)w;(void)h;(void)c;}
static void draw_text(uint32_t x,uint32_t y,const char*s,uint32_t c){(void)x;(void)y;(void)s;(void)c;}
#define user_client_chrome_app_icon(...) ((void)0)
#define user_client_chrome_close(...) ((void)0)
#define user_client_chrome_frame(...) ((void)0)
static void draw_input_line(void){++painted_prompts;}
''' + '\n'.join(extract('user/desktop/gshell/main.c', signature) for signature in (
    'static uint32_t shell_display_length(',
    'static uint32_t shell_scroll_maximum(',
    'static char *shell_output_line(',
    'static void draw_terminal(', 'static bool shell_has_selection(',
    'static void shell_erase_selection(', 'static void insert_command_text(',
    'static void backspace_command_character(', 'static void delete_command_character(',
    'static size_t shell_mouse_text_offset(', 'static size_t shell_mouse_command_offset(',
    'static size_t shell_mouse_output_offset(')) + r'''
int main(void) {
 draw_terminal();assert(g_input_y==0 && painted_prompts==1);
 g_output_count=4;draw_terminal();assert(g_input_y==80 && g_view_first==0);
 g_output_count=20;draw_terminal();assert(g_view_first==11 && g_input_y==180 && g_view_lines==10);
 g_output_scroll=3;painted_prompts=0;draw_terminal();assert(g_view_first==8 && !painted_prompts);
 g_target_height=80;g_output_scroll=0;draw_terminal();assert(g_view_first==17 && g_input_y==60);
 strcpy(g_command,"abcdef");g_command_length=6;g_command_cursor=4;g_input_anchor=1;
 insert_command_text("X");assert(!strcmp(g_command,"aXef") && g_command_cursor==2 && g_input_anchor==SIZE_MAX);
 g_input_anchor=2;backspace_command_character();assert(!strcmp(g_command,"aef") && g_input_anchor==SIZE_MAX);
 g_input_anchor=0;g_command_cursor=3;delete_command_character();assert(!g_command_length);
 strcpy(g_display_line,"/# abc");g_prompt_length=3;g_input_view_start=0;g_command_length=3;
 assert(shell_mouse_command_offset(10)==0 && shell_mouse_command_offset(42)==1);
 assert(shell_mouse_command_offset(10000)==3);
 g_input_view_start=2;assert(shell_mouse_command_offset(26)==1);
 /* A maximum-length path must not truncate command click positions at 255. */
 memset(g_display_line,'p',255);memcpy(g_display_line+255,"# abc",6);
 g_prompt_length=257;g_input_view_start=250;g_command_length=3;
 assert(shell_mouse_command_offset(74)==1);
 assert(shell_mouse_command_offset(90)==3);
 g_input_view_start=259;assert(shell_mouse_command_offset(10)==2);
 assert(shell_mouse_command_offset(18)==3);
 g_view_first=0;g_view_lines=8;g_output_count=2;strcpy(g_output[0],"one");strcpy(g_output[1],"second");
 assert(shell_mouse_output_offset(26,0)==2);
 assert(shell_mouse_output_offset(-20,-20)==0);
 assert(shell_mouse_output_offset(10000,10000)==256+6);
 for(unsigned length=0;length<256;++length)
  for(int x=-16;x<2100;x+=7) {
   size_t result=shell_mouse_text_offset("",length,x);
   assert(result<=length);
   if(x<=10)assert(result==0);
  }
}
'''
with tempfile.TemporaryDirectory(prefix='text-mouse-') as directory:
    path = Path(directory)
    for name, program in [('notepad', note), ('terminal', shell)]:
        source = path / (name + '.c'); source.write_text(program)
        binary = path / name
        command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                        str(source), '-o', str(binary)]
        subprocess.run(command,check=True)
        subprocess.run([str(binary)], check=True)
        print(name + ': production mouse offsets, selection replacement and boundaries passed')
        if name=='notepad':
            for label,broken in (
                ('scroll-only-change',program.replace(' || previous_scroll!=g_scroll_line','')),
                ('unchanged-mouse-redraw',program.replace(
                    'if(previous_cursor!=g_cursor || previous_anchor!=g_selection_anchor || previous_scroll!=g_scroll_line)',
                    'if(true)')),
            ):
                assert broken!=program
                source.write_text(broken);subprocess.run(command,check=True)
                result=subprocess.run([str(binary)],capture_output=True,text=True,
                    preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
                assert result.returncode==-signal.SIGABRT and 'renders==' in result.stderr,result.stderr
                print('Negative control rejected: '+label)

print('Terminal: inline prompt after output, scrollback and short-window layout passed')
