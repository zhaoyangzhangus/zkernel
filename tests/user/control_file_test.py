"""执行真实文件加载器：结构/样式验证、命名绑定、截断、损坏输入及失败释放。"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
static unsigned live,allocations,fail_at,interrupt_read;
static void *test_malloc(size_t size){
 if(++allocations==fail_at)return 0;void *p=malloc(size);if(p)++live;return p;
}
static void test_free(void *p){if(p){assert(live);--live;free(p);}}
static ssize_t test_read(int fd,void *buffer,size_t size){
 assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);
 if(interrupt_read){--interrupt_read;errno=EINTR;return -1;}
 return read(fd,buffer,size>17?17:size);
}
#define malloc test_malloc
#define free test_free
#define read test_read
#include "user/runtime/control_file.c"
#undef malloc
#undef free
#undef read
static void reject(const char *s){
 ui_file_document_t d={0};ui_file_error_t e={0};
 assert(!ui_file_parse(s,strlen(s),&d,&e));
 assert(!live&&!d.nodes&&!d.text&&!d.request.count&&e.message&&e.offset<=strlen(s));
}
int main(int argc,char **argv){
 assert(argc==3);ui_file_document_t d={0};ui_file_error_t e={0};
 interrupt_read=1;assert(ui_file_load(argv[1],&d,&e)&&live==1);
 assert(d.request.count==16&&d.request.hdr.size==48&&d.request.hdr.version==1);
 assert(!d.request.revision&&os_control_tree_validate(d.nodes,d.request.count,d.request.text_size));
 os_control_node_t *apply=ui_file_find(&d,"apply"),*root=ui_file_find(&d,"root");
 assert(apply&&root==d.nodes&&apply->kind==OS_CONTROL_BUTTON&&apply->background==OS_THEME_ACCENT);
 assert(root->layout==OS_CONTROL_COLUMN&&root->font_size==20&&root->padding==24);
 assert(apply->text_length==strlen("应用")&&!memcmp(d.text+apply->text_offset,"应用",strlen("应用")));
 assert(!ui_file_find(&d,"missing"));
 unsigned context=42;apply->user_data=(uintptr_t)&context;d.request.revision=99;
 os_control_event_t event={.node_index=(uint32_t)(apply-d.nodes),.node_kind=apply->kind,
  .target=apply->identifier,.tree_revision=99,.user_data=(uintptr_t)&context};
 assert(os_control_event_node(&d.request,d.nodes,&event)==apply);
 ++event.tree_revision;assert(!os_control_event_node(&d.request,d.nodes,&event));
 assert(!ui_file_parse("x",1,&d,&e)&&live==1);ui_file_destroy(&d);assert(!live);
 const char *valid="<container id='root' layout='row'><button id='ok' width='fill' height='32' text='A &amp; B &lt; C &quot;Q&quot; &apos;x&apos;'/></container>";
 assert(ui_file_parse(valid,strlen(valid),&d,&e));
 assert(d.request.count==2&&!memcmp(d.text,"A & B < C \"Q\" 'x'",d.request.text_size));
 ui_file_destroy(&d);assert(!live);
 const char *frame="<container id='r' flags='window-frame passthrough'><button id='menu' text='...'/></container>";
 assert(ui_file_parse(frame,strlen(frame),&d,&e));
 assert(d.nodes[0].flags&OS_CONTROL_WINDOW_FRAME);ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><container id='child' flags='window-frame'/></container>");
 const char *no_hover="<container id='r'><list-item id='row' flags='no-hover' text='项目'/></container>";
 assert(ui_file_parse(no_hover,strlen(no_hover),&d,&e));
 assert(d.nodes[1].flags&OS_CONTROL_NO_HOVER);ui_file_destroy(&d);assert(!live);
 reject("<container id='r' flags='no-hover'/>");
 const char *scroll="<container id='r'><scrollbar id='s' page-size='2500' value='5000'/></container>";
 assert(ui_file_parse(scroll,strlen(scroll),&d,&e));
 assert(d.nodes[1].page_size==2500&&d.nodes[1].value==5000);
 ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><scrollbar id='s' page-size='10001'/></container>");
 reject("<container id='r'><scrollbar id='s' page-size='-1'/></container>");
 reject("<container id='r'><slider id='s' page-size='2500'/></container>");
 reject("<container id='r'><scrollbar id='s' page-size='1' page-size='2'/></container>");
 const char *checker="<container id='r' flags='checkerboard'/>";
 assert(ui_file_parse(checker,strlen(checker),&d,&e));
 assert(d.nodes[0].flags&OS_CONTROL_CHECKERBOARD);ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><button id='b' flags='checkerboard'/></container>");
 const char *popup="<container id='r'><container id='menu' flags='menu-panel'/><container id='dialog' flags='dialog-panel'/></container>";
 assert(ui_file_parse(popup,strlen(popup),&d,&e));
 assert(d.nodes[1].flags&OS_CONTROL_MENU_PANEL);
 assert(d.nodes[2].flags&OS_CONTROL_DIALOG_PANEL);ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><container id='popup' flags='menu-panel dialog-panel'/></container>");
 reject("<container id='r'><button id='b' flags='menu-panel'/></container>");
 const char *symbol="<container id='r'><button id='find' symbol='search' icon-size='192'/></container>";
 assert(ui_file_parse(symbol,strlen(symbol),&d,&e));
 assert(d.nodes[1].icon_size==192U);
 assert(d.nodes[1].flags&OS_CONTROL_SYMBOL);
 assert(d.nodes[1].value==OS_CONTROL_SYMBOL_SEARCH&&!d.nodes[1].text_length&&!d.resource_count);
 ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><button id='x' symbol='search' icon-size='4097'/></container>");
 reject("<container id='r'><button id='x' icon-size='32'/></container>");
 reject("<container id='r' symbol='search'/>");
 const char *navigation="<container id='r'><button id='home' symbol='home'/><button id='drive' symbol='drive'/></container>";
 assert(ui_file_parse(navigation,strlen(navigation),&d,&e));
 assert(d.nodes[1].value==OS_CONTROL_SYMBOL_HOME&&d.nodes[2].value==OS_CONTROL_SYMBOL_DRIVE);
 ui_file_destroy(&d);assert(!live);
 const char *files="<container id='r'><button id='folder' symbol='folder'/><button id='file' symbol='file'/></container>";
 assert(ui_file_parse(files,strlen(files),&d,&e));
 assert(d.nodes[1].value==OS_CONTROL_SYMBOL_FOLDER&&d.nodes[2].value==OS_CONTROL_SYMBOL_FILE);
 ui_file_destroy(&d);assert(!live);
 reject("<container id='r'><button id='x' symbol='missing'/></container>");
 reject("<container id='r'><button id='x' symbol='search' text='label'/></container>");
 reject("<container id='r'><button id='x' symbol='search' value='1'/></container>");
 reject("<container id='r'><button id='x' value='1' symbol='search'/></container>");
 reject("<container id='r'><button id='x' symbol='search' surface='1'/></container>");
 const char *application="<container id='desktop'><button id='app' text='APP' app-directory='/usr/apps/demo' program='demo' icon='icon.png'/></container>";
 assert(ui_file_parse(application,strlen(application),&d,&e)&&live==1&&d.resource_count==1);
 char resource_path[256];
 assert(ui_file_resource_path(&d,d.resources,UI_RESOURCE_PROGRAM,resource_path,sizeof(resource_path)));
 assert(!strcmp(resource_path,"/usr/apps/demo/demo"));
 assert(ui_file_resource_path(&d,d.resources,UI_RESOURCE_ICON,resource_path,sizeof(resource_path)));
 assert(!strcmp(resource_path,"/usr/apps/demo/icon.png"));
 assert(!ui_file_resource_path(&d,d.resources,UI_RESOURCE_ICON,resource_path,4));
 assert(!ui_file_resource_path(&d,d.resources,UI_RESOURCE_DIRECTORY,resource_path,sizeof(resource_path)));
 ui_file_resource_t corrupt=d.resources[0];corrupt.offset[1]=UINT32_MAX;
 assert(!ui_file_resource_path(&d,&corrupt,UI_RESOURCE_PROGRAM,resource_path,sizeof(resource_path)));
 ui_file_destroy(&d);assert(!live);
 allocations=0;fail_at=1;
 assert(!ui_file_parse(application,strlen(application),&d,&e)&&!live&&!d.resources);
 fail_at=0;
 /* 文档必须独立于输入缓冲，内部视图只能随文档整体释放。 */
 char transient[512];strcpy(transient,application);
 assert(ui_file_parse(transient,strlen(transient),&d,&e)&&live==1);
 memset(transient,0,sizeof(transient));
 assert(ui_file_find(&d,"app") && d.resource_count==1);
 assert(ui_file_resource_path(&d,d.resources,UI_RESOURCE_ICON,resource_path,sizeof(resource_path)));
 assert(!strcmp(resource_path,"/usr/apps/demo/icon.png"));
 ui_file_destroy(&d);ui_file_destroy(&d);ui_file_destroy(0);assert(!live);
 const char *bad[]={
  "", "<text id='root'/>","<container/>","<container id='a' id='b'/>",
  "<container id='r'><button id='a'/><text id='a'/></container>",
  "<container id='r'><button id='x'><text id='y'/></button></container>",
  "<container id='r'><button id='x'/></button>","<container id='r' strange='1'/>",
  "<container id='r' padding='-1'/>","<container id='r' width='2147483648'/>",
  "<container id='r' x='-2147483649'/>","<container id='r' height='-2'/>",
  "<container id='r' font-size='97'/>","<container id='r' radius='4097'/>",
  "<container id='r' foreground='#fffffff'/>","<container id='r' text='&bogus;'/>",
  "<container id='r' text='&amp'/>","<container id='r' text='a' text='b'/>",
  "<container id='r' flags='checked'/>","<container id='r' flags='clip clip'/>",
  "<container id='r' flags='mystery'/>","<container id='r' flags='grow'/>",
  "<container id='r' layout='row' flags='justify-center justify-end'/>",
  "<container id='r'><slider id='s' value='10001'/></container>",
  "<container id='r'><surface id='s' surface='2'/></container>",
  "<container id='r'><button id='s' value='10'/></container>",
  "<container id='r'><slider id='s' value='10' surface='1'/></container>",
  "<container id='r' user-data='123'/>","<container id='r'/> trailing",
  "<container id='r'/><!-- incomplete", "<!DOCTYPE x><container id='r'/>",
  "<container id='x&amp;y'/>","<container id='r'><!-- missing"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)reject(bad[i]);
 const char *paths[]={"../escape","/absolute",".","..","a/b","a b",""};
 char invalid_resource[512];
 for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i){
  snprintf(invalid_resource,sizeof(invalid_resource),"<container id='r'><button id='x' app-directory='/usr/apps/demo' program='%s' icon='icon.png'/></container>",paths[i]);
  reject(invalid_resource);
 }
 reject("<container id='r'><button id='x' app-directory='/usr/apps/../demo' program='demo' icon='icon.png'/></container>");
 reject("<container id='r'><button id='x' program='demo' icon='icon.png'/></container>");
 reject("<container id='r' app-directory='/usr/apps/demo' program='demo' icon='icon.png'/>");
 char nested[8192];size_t length=0;
 for(unsigned i=0;i<65;++i)length+=(size_t)sprintf(nested+length,"<container id='n%u'>",i);
 for(unsigned i=0;i<65;++i)length+=(size_t)sprintf(nested+length,"</container>");
 reject(nested);
 char damaged[512];size_t n=strlen(valid);unsigned state=12345,cases=0;
 for(unsigned sample=0;sample<8000;++sample){
  memcpy(damaged,valid,n);state=state*1664525U+1013904223U;
  size_t position=state%n;damaged[position]=(char)(state>>24);
  size_t size=sample&1?n:position;
  bool ok=ui_file_parse(damaged,size,&d,&e);
  if(ok){assert(os_control_tree_validate(d.nodes,d.request.count,d.request.text_size));ui_file_destroy(&d);}
  else assert(!d.nodes&&!d.text&&e.message&&e.offset<=size);
  assert(!live);++cases;
 }
 for(unsigned failure=1;failure<=2;++failure){
  allocations=0;fail_at=failure;
  assert(!ui_file_load(argv[1],&d,&e)&&!live&&!d.nodes&&!d.text&&e.message);
 }
 fail_at=0;assert(!ui_file_load("/no-such-ui-file",&d,&e)&&!live);
 /* 无写入端的命名管道必须立即拒绝，不能卡在打开阶段。 */
 assert(!ui_file_load(argv[2],&d,&e)&&!live&&!d.nodes&&e.message);
 assert(!ui_file_load("/tmp",&d,&e)&&!live&&!d.nodes&&e.message);
 printf("UI file loader: real file/short read/EINTR, named binding, shared validation, malformed/deep input, %u mutations and allocation rollback PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-file-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root), '-I'+str(root/'include'), str(path/'test.c'), '-o', str(path/'test')], check=True)
    os.mkfifo(path/'pipe.ui')
    subprocess.run([str(path/'test'), str(root/'user/ui/controls.ui'), str(path/'pipe.ui')],
                   check=True, timeout=30)
