"""验证记事本接入公共路径选择器，失败不更换当前路径或丢失正文。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/notepad/main.c').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/runtime/file_picker.h"
static ui_message_box_t g_message;
static ui_file_picker_t g_picker;
static bool g_named_document,g_picker_overwrite,g_dirty,g_mouse_selecting,g_ctrl,g_shift;
static char g_file_path[256],text[64];
enum notepad_pending { NOTEPAD_PENDING_NONE,NOTEPAD_PENDING_CLOSE,
    NOTEPAD_PENDING_NEW,NOTEPAD_PENDING_RELOAD,NOTEPAD_PENDING_OPEN };
static enum notepad_pending g_pending;
static unsigned saves,loads,exits,renders;
static bool fail_io;
static int64_t stat_status;
static uint32_t stat_type;
uint32_t ui_text_size(void){return 16;}
uint32_t ui_text_line_height(void){return 20;}
static int64_t notepad_syscall_one(uint64_t number,uint64_t address) {
    if(number==OS_SYS_FILE_ENUMERATE_BATCH)return -2;
    assert(number==OS_SYS_FILE_STAT);
    os_file_stat_t *r=(void *)(uintptr_t)address;
    if(!strcmp((const char *)(uintptr_t)r->path,"/")) {
        r->info.type=OS_FILE_TYPE_DIRECTORY;return 0;
    }
    assert(!strcmp((const char *)(uintptr_t)r->path,"/new.txt"));
    r->info.type=stat_type;return stat_status;
}
static bool save_document(void){++saves;if(!fail_io)g_dirty=false;return !fail_io;}
static bool load_document(void){++loads;if(!fail_io){strcpy(text,"loaded");g_dirty=false;}return !fail_io;}
static void notepad_exit(uint64_t status){assert(!status);++exits;}
static void new_document(void){text[0]=0;g_dirty=true;g_named_document=false;}
static void set_status(const char *status){(void)status;}
static void render(void){++renders;}
bool ui_text_adjust(int delta){(void)delta;return false;}
static void reflow_editor(void){}
static char key_to_ascii(uint32_t code,bool shift){(void)shift;return code==4?'a':0;}
''' + ''.join(function(signature) for signature in (
    'static void show_file_picker(', 'static void request_action(',
    'static void complete_message(', 'static void commit_file_picker(',
    'static bool handle_message_event(', 'static bool handle_caption_event(', 'static bool handle_file_picker_event(')) + r'''
static void reset(bool save) {
    ui_file_picker_close(&g_picker);g_message.active=false;g_picker_overwrite=false;
    strcpy(text,"unsaved");strcpy(g_file_path,"/old.txt");g_dirty=true;g_named_document=true;
    g_pending=NOTEPAD_PENDING_NONE;fail_io=false;stat_status=-2;stat_type=OS_FILE_TYPE_REGULAR;
    saves=loads=exits=renders=0;show_file_picker(save);strcpy(g_picker.name,"/new.txt");
    g_picker.active=false;g_picker.accepted=true;
}
static void answer(uint32_t button) {
    os_window_event_t event={.type=OS_WINDOW_EVENT_CONTROL,
        .control={.action=OS_CONTROL_CLICK,.target=ui_message_target(&g_message,button)}};
    assert(handle_message_event(&event));
}
int main(void) {
    reset(false);g_dirty=false;
    os_window_event_t click={.type=OS_WINDOW_EVENT_CONTROL,
        .control={.target=0x1ffffff1U,.action=OS_CONTROL_CLICK}};
    assert(handle_caption_event(&click)&&g_picker.active&&!g_picker.save&&renders==1);
    reset(false);assert(handle_caption_event(&click)&&g_message.active&&g_pending==NOTEPAD_PENDING_OPEN);
    answer(1);assert(!g_message.active&&!g_picker.active&&!loads);
    reset(false);g_dirty=false;click.control.action=OS_CONTROL_DOWN;
    assert(handle_caption_event(&click)&&!g_picker.active&&!g_message.active&&!renders);
    click.control.target=0;assert(!handle_caption_event(&click));
    reset(false);g_dirty=false;request_action(NOTEPAD_PENDING_OPEN);
    assert(g_picker.active && !g_picker.save);
    reset(false);commit_file_picker(false);
    assert(loads==1 && !saves && !strcmp(text,"loaded") && !strcmp(g_file_path,"/new.txt"));
    assert(!g_picker.active && !g_picker.entries && g_named_document);
    reset(false);fail_io=true;commit_file_picker(false);
    assert(g_picker.active && !strcmp(text,"unsaved") && !strcmp(g_file_path,"/old.txt") && g_dirty);
    reset(true);commit_file_picker(false);
    assert(saves==1 && !loads && !g_dirty && !strcmp(g_file_path,"/new.txt"));
    reset(true);fail_io=true;commit_file_picker(false);
    assert(saves==1 && g_dirty && g_picker.active && !strcmp(g_file_path,"/old.txt"));
    /* 保存失败后重试仍须完成原来的关闭操作，不能丢失待办动作。 */
    reset(true);g_pending=NOTEPAD_PENDING_CLOSE;fail_io=true;commit_file_picker(false);
    assert(g_picker.active && g_dirty && !exits && g_pending==NOTEPAD_PENDING_CLOSE);
    assert(g_picker.error[0] && !strcmp(text,"unsaved") && !strcmp(g_file_path,"/old.txt"));
    fail_io=false;assert(ui_file_picker_choose(&g_picker));
    g_picker.active=false;commit_file_picker(false);
    assert(saves==2 && exits==1 && !g_pending && !g_picker.active);
    reset(true);g_pending=NOTEPAD_PENDING_NEW;fail_io=true;commit_file_picker(false);
    assert(g_pending==NOTEPAD_PENDING_NEW && !strcmp(text,"unsaved"));
    fail_io=false;assert(ui_file_picker_choose(&g_picker));
    g_picker.active=false;commit_file_picker(false);
    assert(saves==2 && !exits && !g_pending && !text[0] && !g_named_document);
    reset(true);stat_status=0;commit_file_picker(false);
    assert(g_picker_overwrite && g_message.active && !saves);
    answer(3);assert(!g_picker_overwrite && g_picker.active && !saves && !strcmp(g_file_path,"/old.txt"));
    g_picker.active=false;commit_file_picker(false);answer(2);
    assert(saves==1 && !g_message.active && !g_picker.active && !strcmp(g_file_path,"/new.txt"));
    reset(true);stat_status=-13;commit_file_picker(false);
    assert(!saves && g_picker.active && !strcmp(g_file_path,"/old.txt"));
    reset(true);stat_status=0;stat_type=OS_FILE_TYPE_DIRECTORY;commit_file_picker(false);
    assert(!saves && g_picker.active && g_picker.error[0]);
    reset(true);g_pending=NOTEPAD_PENDING_CLOSE;commit_file_picker(false);
    assert(saves==1 && exits==1 && !g_pending);
    reset(true);g_named_document=false;g_pending=NOTEPAD_PENDING_CLOSE;
    complete_message(OS_MESSAGE_SAVE);assert(g_picker.active && g_picker.save && !saves && !exits);
    reset(false);g_picker.active=true;unsigned before=renders;
    os_window_event_t quiet={.type=OS_WINDOW_EVENT_INPUT,
        .input={.type=OS_INPUT_EVENT_KEY,.code=0xe0,.value=OS_INPUT_VALUE_PRESS}};
    assert(handle_file_picker_event(&quiet)&&g_picker.ctrl&&renders==before);
    quiet.input.value=OS_INPUT_VALUE_RELEASE;
    assert(handle_file_picker_event(&quiet)&&!g_picker.ctrl&&renders==before);
    quiet.input.code=4;assert(handle_file_picker_event(&quiet)&&renders==before);
    quiet.input.value=OS_INPUT_VALUE_PRESS;
    assert(handle_file_picker_event(&quiet)&&renders==before+1);
    quiet.input.code=0x39;assert(handle_file_picker_event(&quiet)&&renders==before+1);
    ui_file_picker_close(&g_picker);
    puts("Notepad picker: full-path result, open/save, failure rollback, overwrite refusal/approval and unsaved continuation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='notepad-picker-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
