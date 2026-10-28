"""Actual editor confirmation/file functions; inject failures without guest data."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/desktop/notepad/main.c').read_text()
assert 'NOTEPAD_STATUS_' not in source and 'g_status' not in source
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
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <uapi/all.h>
#include "user/runtime/message_box.h"
#define NOTEPAD_TEXT_CAPACITY 8192U
#define NOTEPAD_PATH_CAPACITY 256U
static char g_text[8192],g_load_text[8192],g_file_path[]="/tmp/dialog-test";
static size_t g_text_length,g_cursor,g_selection_anchor;
static uint32_t g_scroll_line;
static bool g_dirty,g_follow_cursor,g_mouse_selecting,g_named_document=true;
static unsigned picker_calls;static bool picker_save;
static void show_file_picker(bool save){++picker_calls;picker_save=save;}
static ui_message_box_t g_message;
enum notepad_pending {NOTEPAD_PENDING_NONE,NOTEPAD_PENDING_CLOSE,NOTEPAD_PENDING_NEW,NOTEPAD_PENDING_RELOAD,NOTEPAD_PENDING_OPEN};
static enum notepad_pending g_pending;
static unsigned mode,position,stored_size;
static char stored[8193];
static jmp_buf exited;
static void notepad_exit(uint64_t status){assert(!status);longjmp(exited,1);}
static void history_reset(void){g_selection_anchor=SIZE_MAX;g_mouse_selecting=false;}
static void mark_saved(void){g_dirty=false;}
static void begin_edit(void){}
static int64_t notepad_syscall_one(uint64_t number,uint64_t value){
 (void)value;return number==OS_SYS_FILE_FSYNC&&mode==3?-5:0;
}
static int64_t notepad_syscall_four(uint64_t number,uint64_t a,uint64_t b,uint64_t c,uint64_t d){
 (void)a;
 if(number==OS_SYS_FILE_OPEN){if(mode==1)return -13;*(os_handle_t*)d=3;position=0;if(b&OS_FILE_OPEN_TRUNCATE)stored_size=0;return 0;}
 uint64_t *count=(uint64_t*)d;
 if(number==OS_SYS_FILE_WRITE){if(mode==2){*count=0;return 0;}memcpy(stored+position,(void*)b,c);position+=c;stored_size=position;*count=c;return 0;}
 if(number==OS_SYS_FILE_READ){if(mode==4)return -5;*count=stored_size-position<c?stored_size-position:c;memcpy((void*)b,stored+position,*count);position+=*count;return 0;}
 assert(0);return -1;
}
''' + '\n'.join(extract(signature) for signature in (
    'static bool text_equals(', 'static void set_status(',
    'static bool load_document(', 'static bool remove_temporary_document(', 'static bool save_document(',
    'static void request_action(', 'static void new_document(',
    'static void reload_document(', 'static void complete_message(')) + r'''
static void reset(void){
 strcpy(g_text,"unsaved content");g_text_length=strlen(g_text);g_cursor=4;g_dirty=true;
 g_pending=NOTEPAD_PENDING_NONE;g_message.active=false;mode=0;position=stored_size=0;
 g_named_document=true;strcpy(g_file_path,"/tmp/dialog-test");picker_calls=0;
}
static void ask(enum notepad_pending action){request_action(action);assert(g_message.active&&g_pending==action);g_message.active=false;}
int main(void){
 reset();g_named_document=false;ask(NOTEPAD_PENDING_CLOSE);complete_message(OS_MESSAGE_SAVE);
 assert(picker_calls==1&&picker_save&&g_pending==NOTEPAD_PENDING_CLOSE&&g_dirty);
 reset();g_dirty=false;request_action(NOTEPAD_PENDING_OPEN);assert(picker_calls==1&&!picker_save);
 reset();reload_document();assert(g_pending==NOTEPAD_PENDING_RELOAD&&g_message.active&&g_dirty);
 reset();new_document();assert(g_pending==NOTEPAD_PENDING_NEW&&g_message.active&&g_dirty);
 reset();ask(NOTEPAD_PENDING_CLOSE);complete_message(OS_MESSAGE_CANCEL);
 assert(g_dirty&&!strcmp(g_text,"unsaved content")&&!g_pending);
 reset();ask(NOTEPAD_PENDING_CLOSE);
 if(!setjmp(exited)){complete_message(OS_MESSAGE_DISCARD);assert(0);}
 for(unsigned failure=1;failure<=3;++failure){
  reset();mode=failure;ask(NOTEPAD_PENDING_CLOSE);complete_message(OS_MESSAGE_SAVE);
  assert(g_dirty&&g_message.active&&!g_pending&&!strcmp(g_text,"unsaved content"));
  assert(strstr(g_message.message,"SAVE FAILED"));
 }
 reset();ask(NOTEPAD_PENDING_CLOSE);
 if(!setjmp(exited)){complete_message(OS_MESSAGE_SAVE);assert(0);}
 assert(!g_dirty&&stored_size==strlen("unsaved content")&&!memcmp(stored,"unsaved content",stored_size));
 reset();ask(NOTEPAD_PENDING_NEW);complete_message(OS_MESSAGE_DISCARD);
 assert(g_dirty&&!g_text_length&&!g_cursor&&!g_text[0]);
 reset();ask(NOTEPAD_PENDING_RELOAD);mode=4;complete_message(OS_MESSAGE_DISCARD);
 assert(g_dirty&&!strcmp(g_text,"unsaved content")&&g_cursor==4&&g_message.active);
 reset();strcpy(stored,"replacement");stored_size=strlen(stored);
 ask(NOTEPAD_PENDING_RELOAD);complete_message(OS_MESSAGE_DISCARD);
 assert(!g_dirty&&!strcmp(g_text,"replacement")&&g_message.active&&!strcmp(g_message.message,"RELOADED"));
 reset();memset(stored,'x',sizeof(stored));stored_size=8192;
 assert(!load_document()&&g_dirty&&!strcmp(g_text,"unsaved content")&&g_message.active);
 reset();set_status("UNDO");set_status("REDO");set_status("NOTHING TO UNDO");
 assert(!g_message.active);set_status("SAVED");assert(g_message.active);
 puts("Editor dialogs: cancel/discard/save close, open/write/fsync failures, new/reload confirmation, read failures preserve unsaved data, oversized files rejected, routine edits silent; no footer");
}
'''
with tempfile.TemporaryDirectory(prefix='system-notepad-dialog-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root), '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
