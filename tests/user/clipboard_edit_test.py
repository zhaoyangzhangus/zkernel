"""执行记事本真实复制/剪切/粘贴代码，检查容量、撤销事务和失败不删选区。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'user/desktop/notepad/main.c').read_text()
def extract(signature):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "user/runtime/clipboard.h"
#define NOTEPAD_TEXT_CAPACITY 8192U
static char g_text[8192],clipboard[16384];
static size_t g_selection_anchor=SIZE_MAX,g_cursor,g_text_length,clip_size;
static bool g_dirty,fail_copy,change_size,empty_on_read;static unsigned edits;
static int64_t notepad_syscall_one(uint64_t op,uint64_t address){
 os_clipboard_text_t *r=(void *)(uintptr_t)address;
 if(op==OS_SYS_CLIPBOARD_SET){if(fail_copy)return -12;memcpy(clipboard,(void *)(uintptr_t)r->data,r->size);clip_size=r->size;return 0;}
 assert(op==OS_SYS_CLIPBOARD_GET);
 if(!r->size){r->size=clip_size;if(change_size){clip_size=5;memcpy(clipboard,"grown",5);change_size=false;}return 0;}
 if(empty_on_read){clip_size=0;empty_on_read=false;}
 size_t capacity=r->size;r->size=clip_size;
 if(capacity<clip_size)return -28;
 memcpy((void *)(uintptr_t)r->data,clipboard,clip_size);return 0;
}
static void begin_edit(void){++edits;}
''' + '\n'.join(extract(x) for x in ['static bool has_selection(', 'static void erase_selection(', 'static void copy_selection(', 'static void paste_selection('])+r'''
static void document(const char *text,size_t first,size_t last){strcpy(g_text,text);g_text_length=strlen(text);g_cursor=last;g_selection_anchor=first;g_dirty=false;edits=0;}
int main(void){
 document("A中文B",1,7);copy_selection(false);assert(clip_size==6&&!memcmp(clipboard,"中文",6)&&!edits&&!g_dirty);
 fail_copy=true;copy_selection(true);assert(!strcmp(g_text,"A中文B")&&!edits);fail_copy=false;
 copy_selection(true);assert(!strcmp(g_text,"AB")&&g_cursor==1&&edits==1&&g_dirty);
 paste_selection();assert(!strcmp(g_text,"A中文B")&&g_cursor==7&&edits==2);
 document("hello",1,4);clip_size=3;memcpy(clipboard,"XYZ",3);paste_selection();assert(!strcmp(g_text,"hXYZo")&&edits==1);
 document("unchanged",0,9);clip_size=sizeof(g_text)+1;paste_selection();assert(!strcmp(g_text,"unchanged")&&!edits);
 document("keep",0,4);clip_size=3;memcpy(clipboard,"a\0b",3);paste_selection();assert(!strcmp(g_text,"keep")&&!edits);
 clip_size=3;empty_on_read=true;paste_selection();assert(!strcmp(g_text,"keep")&&!edits);
 document("",SIZE_MAX,0);clip_size=1;clipboard[0]='x';change_size=true;paste_selection();assert(!strcmp(g_text,"grown")&&edits==1);
 puts("Clipboard editing: UTF-8 cross-document copy/cut/paste, single undo transaction, failure/overflow/NUL retention and query races PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c=Path(tmp)/'test.c';c.write_text(code);exe=Path(tmp)/'test'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),'-I'+str(root),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
