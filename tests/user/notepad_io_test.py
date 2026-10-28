"""Run production editor I/O against an independent file/handle model."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'user/desktop/notepad/main.c').read_text()
functions=source[source.index('static bool load_document('):source.index('static void show_file_picker(')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <uapi/all.h>
#define NOTEPAD_TEXT_CAPACITY 8192U
#define NOTEPAD_PATH_CAPACITY 256U
enum { NORMAL, SHORT_IO, IO_FAIL, ZERO_WRITE, OVERREPORT, FLUSH_FAIL, CLOSE_FAIL,
       RENAME_FAIL, OPEN_FAIL, COLLISION, BUSY, REMOVE_FAIL, CASES };
static char g_text[8192],g_load_text[8192],g_file_path[256],status_text[128];
static size_t g_text_length,g_cursor;
static uint32_t g_scroll_line;
static bool g_dirty,g_follow_cursor;
static unsigned mode,opens,reads,writes,closes,flushes,renames,removes,marks,history;
static bool handle_live,temporary_exists;
static size_t original_size,temporary_size,position;
static char original[8193],temporary[8193],temporary_path[256];
static void set_status(const char *s){strcpy(status_text,s);}
static void mark_saved(void){++marks;g_dirty=false;}
static void history_reset(void){++history;}
static int64_t notepad_syscall_four(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){
    if(n==OS_SYS_FILE_OPEN){
        ++opens;assert(!handle_live && !c);
        if(mode==OPEN_FAIL)return -13;
        if(b==OS_FILE_OPEN_READ){assert(!strcmp((void*)a,g_file_path));*(os_handle_t*)d=41;}
        else {
            assert(b==(OS_FILE_OPEN_WRITE|OS_FILE_OPEN_CREATE|OS_FILE_OPEN_EXCLUSIVE));
            assert(strcmp((void*)a,g_file_path));assert(!strncmp((void*)a,"/dir/.np-save-",14));
            assert(strlen((void*)a)<256);
            if(mode==BUSY || (mode==COLLISION && opens<=2))return -17;
            assert(!temporary_exists);strcpy(temporary_path,(void*)a);
            temporary_exists=true;temporary_size=0;*(os_handle_t*)d=42;
        }
        handle_live=true;position=0;return 0;
    }
    assert(handle_live);
    uint64_t *bytes=(void*)d;
    if(n==OS_SYS_FILE_READ){
        assert(a==41);++reads;
        if(mode==IO_FAIL && reads==2)return -5;
        if(mode==OVERREPORT){*bytes=c+1;return 0;}
        size_t amount=original_size-position<c?original_size-position:c;
        if((mode==SHORT_IO || mode==IO_FAIL) && amount>7)amount=7;
        memcpy((void*)b,original+position,amount);position+=amount;*bytes=amount;return 0;
    }
    assert(n==OS_SYS_FILE_WRITE && a==42 && temporary_exists);++writes;
    if((mode==IO_FAIL || mode==REMOVE_FAIL) && writes==2)return -5;
    if(mode==ZERO_WRITE){*bytes=0;return 0;}
    if(mode==OVERREPORT){*bytes=c+1;return 0;}
    if((mode==SHORT_IO || mode==IO_FAIL || mode==REMOVE_FAIL) && c>7)c=7;
    assert(position+c<=sizeof(temporary));memcpy(temporary+position,(void*)b,c);
    position+=c;temporary_size=position;*bytes=c;return 0;
}
static int64_t notepad_syscall_one(uint64_t n,uint64_t a){
    if(n==OS_SYS_HANDLE_CLOSE){assert(handle_live&&(a==41||a==42));handle_live=false;++closes;return mode==CLOSE_FAIL?-5:0;}
    if(n==OS_SYS_FILE_FSYNC){assert(handle_live&&a==42);++flushes;return mode==FLUSH_FAIL?-5:0;}
    if(n==OS_SYS_FILE_RENAME){
        os_file_rename_t *p=(void*)a;assert(p->hdr.size==sizeof(*p)&&p->hdr.version==OS_SYSCALL_ABI_VERSION);
        assert(!handle_live && temporary_exists && flushes==1 && closes==1);
        assert(!strcmp((void*)p->old_path,temporary_path)&&!strcmp((void*)p->new_path,g_file_path));
        ++renames;if(mode==RENAME_FAIL)return -5;
        memcpy(original,temporary,temporary_size);original_size=temporary_size;temporary_exists=false;return 0;
    }
    assert(n==OS_SYS_FILE_REMOVE);os_file_path_op_t *p=(void*)a;
    assert(p->hdr.size==sizeof(*p)&&p->hdr.version==OS_SYSCALL_ABI_VERSION);
    assert(!handle_live && temporary_exists && !strcmp((void*)p->path,temporary_path));++removes;
    if(mode==REMOVE_FAIL)return -5;
    temporary_exists=false;return 0;
}
''' + functions + r'''
static void reset(unsigned test){
    mode=test;opens=reads=writes=closes=flushes=renames=removes=marks=history=0;
    handle_live=temporary_exists=false;position=temporary_size=0;
    strcpy(g_file_path,"/dir/doc.txt");strcpy(original,"ORIGINAL");original_size=8;
    memset(g_text,'x',700);g_text[700]=0;g_text_length=700;g_cursor=6;
    g_dirty=true;g_scroll_line=9;g_follow_cursor=false;status_text[0]=0;
}
int main(void){
    reset(NORMAL);strcpy(g_file_path,"/dir/.np-save-00000001.tmp");
    assert(save_document()&&opens==1&&!temporary_exists&&original_size==700);
    for(unsigned test=0;test<CASES;++test){
        reset(test);bool ok=save_document();bool success=test==NORMAL||test==SHORT_IO||test==COLLISION;
        assert(ok==success && !handle_live);
        if(success){
            assert(!g_dirty && marks==1 && original_size==700 && !memcmp(original,g_text,700));
            assert(renames==1 && !removes && !temporary_exists);
            if(test==NORMAL)assert(writes==1);
            if(test==COLLISION)assert(opens==3);
        }else{
            assert(g_dirty && !marks && original_size==8 && !memcmp(original,"ORIGINAL",8));
            assert(temporary_exists==(test==REMOVE_FAIL));
            assert(strstr(status_text,"SAVE FAILED"));
            if(test==REMOVE_FAIL)assert(strstr(status_text,"CLEANUP FAILED"));
            if(test==BUSY)assert(opens==32 && !removes);
            if(test==OPEN_FAIL)assert(!removes);
        }
    }
    reset(NORMAL);g_text_length=0;g_text[0]=0;assert(save_document()&&!original_size&&!writes&&marks==1);
    reset(NORMAL);memset(g_file_path,'a',255);g_file_path[0]='/';g_file_path[240]='/';g_file_path[255]=0;
    assert(!save_document()&&!opens&&!strcmp(original,"ORIGINAL"));
    reset(NORMAL);strcpy(g_file_path,"relative.txt");assert(!save_document()&&!opens);
    const unsigned load_modes[]={NORMAL,SHORT_IO,IO_FAIL,OVERREPORT,OPEN_FAIL};
    for(unsigned i=0;i<5;++i){
        reset(load_modes[i]);memset(original,'r',1000);original_size=1000;
        bool ok=load_document();assert(ok==(i<2)&&!handle_live);
        if(ok){assert(g_text_length==1000&&g_cursor==1000&&!g_dirty&&history==1&&marks==1&&g_follow_cursor&&!g_scroll_line);
            assert(!memcmp(g_text,original,1000)&&!g_text[1000]);if(mode==NORMAL)assert(reads==2);}
        else assert(g_text_length==700&&g_cursor==6&&g_dirty&&!history&&!marks&&g_scroll_line==9&&!g_follow_cursor);
    }
    reset(SHORT_IO);memset(original,'q',8191);original_size=8191;assert(load_document()&&g_text_length==8191);
    reset(SHORT_IO);memset(original,'q',8192);original_size=8192;
    assert(!load_document()&&g_dirty&&g_text_length==700&&!marks&&!history&&!handle_live);
    reset(NORMAL);original_size=0;assert(load_document()&&!g_text_length&&!g_text[0]&&!g_dirty);
}
'''
with tempfile.TemporaryDirectory(prefix='notepad-io-') as directory:
    path=Path(directory);(path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(ROOT/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Editor I/O: full short-read loops, capacity limits, exclusive same-directory staging, failed saves retain original and dirty state passed')
