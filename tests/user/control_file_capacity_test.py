"""界面按真实节点预留，与旧加载器对照语法行为、内存和失败释放。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
loader = (root / 'user/runtime/control_file.c').read_text()
reservation = '''if(!ui_node_capacity(&parser))goto fail;
    uint32_t capacity=parser.capacity;'''
assert reservation in loader
resource_reservation = '(size_t)parser.resource_capacity*sizeof(*candidate.resources)'
assert resource_reservation in loader
text_reservation = 'candidate.nodes=malloc(text_offset+parser.text_capacity);'
assert text_reservation in loader
node_overreservation = '''if(!ui_node_capacity(&parser))goto fail;
    uint32_t capacity=(uint32_t)(size/8U+1U);
    if(capacity>OS_CONTROL_TREE_MAX_NODES)capacity=OS_CONTROL_TREE_MAX_NODES;
    parser.capacity=capacity;'''
start = loader.index('    /* 文档只分配一次：')
end = loader.index('    if(!ui_node(', start)
allocation = loader[start:end]
# 独立保留多次分配的旧实现，对照输出与所有权，而非只修改分配计数。
old_allocation = '''    candidate.nodes=malloc((size_t)capacity*(sizeof(*candidate.nodes)+sizeof(*candidate.identifiers)));
    candidate.identifiers=candidate.nodes?(uint16_t *)(candidate.nodes+capacity):0;
    candidate.text=malloc(size<OS_CONTROL_TREE_MAX_TEXT?size:OS_CONTROL_TREE_MAX_TEXT);
    if(!candidate.nodes||!candidate.text){ui_fail(&parser,"out of memory");goto fail;}
'''
lazy_resource = '''        if(!d->resources){
            d->resources=malloc((size_t)p->resource_capacity*sizeof(*d->resources));
            if(!d->resources)return ui_fail(p,"out of memory");
        }
'''
previous = loader.replace(allocation, old_allocation).replace(reservation, node_overreservation)
previous = previous.replace('        ui_file_resource_t *r=', lazy_resource+'        ui_file_resource_t *r=')
previous = previous.replace('free(d->nodes);*d=', 'free(d->nodes);free(d->text);free(d->resources);*d=')
previous = previous.replace('p->document->request.text_size>=p->text_capacity',
    'p->document->request.text_size==OS_CONTROL_TREE_MAX_TEXT')
wrappers = r'''
#include <stdlib.h>
#include <stddef.h>
extern void *capacity_malloc(size_t);
extern void capacity_free(void *);
#define malloc capacity_malloc
#define free capacity_free
'''
rename = ''.join('#define ui_file_' + name + ' reference_' + name + '\n' for name in
                 ('identifier', 'find', 'destroy', 'resource_path', 'wallpaper_path', 'set_wallpaper', 'parse', 'load'))
checks = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "control_file.h"
bool reference_parse(const char *,size_t,ui_file_document_t *,ui_file_error_t *);
void reference_destroy(ui_file_document_t *);
static unsigned calls,live,fail_at;
static size_t bytes[64];static void *pointers[64];
void *capacity_malloc(size_t size){
 assert(calls<64);unsigned slot=calls++;bytes[slot]=size;
 if(calls==fail_at)return 0;
 void *p=malloc(size);assert(p);pointers[slot]=p;++live;return p;
}
void capacity_free(void *p){
 if(!p)return;
 for(unsigned i=0;i<calls;++i)if(pointers[i]==p){pointers[i]=0;assert(live);--live;free(p);return;}
 assert(0);
}
static void reset(void){
 assert(!live);calls=fail_at=0;memset(bytes,0,sizeof(bytes));memset(pointers,0,sizeof(pointers));
}
static unsigned comparisons;
static void compare(const char *source,size_t size,const char *label){
 reset();ui_file_document_t a={0},b={0};ui_file_error_t ae={0},be={0};
 bool accepted=ui_file_parse(source,size,&a,&ae);unsigned candidate_calls=calls;
 bool old=reference_parse(source,size,&b,&be);assert(accepted==old);
 if(accepted){
  assert(a.request.count==b.request.count&&a.request.text_size==b.request.text_size);
  assert(!memcmp(a.nodes,b.nodes,(size_t)a.request.count*sizeof(*a.nodes)));
  assert(!memcmp(a.text,b.text,a.request.text_size));
  assert(!memcmp(a.identifiers,b.identifiers,(size_t)a.request.count*sizeof(*a.identifiers)));
  assert(a.resource_count==b.resource_count);
  if(a.resource_count)assert(!memcmp(a.resources,b.resources,(size_t)a.resource_count*sizeof(*a.resources)));
  assert(candidate_calls==1);
  size_t expected=(size_t)a.request.count*(sizeof(*a.nodes)+sizeof(*a.identifiers));
  assert(a.identifiers==(uint16_t *)(a.nodes+a.request.count));
  size_t alignment=_Alignof(ui_file_resource_t);
  size_t resource_offset=(expected+alignment-1)&~(alignment-1);
  size_t text_offset=resource_offset+(size_t)a.resource_count*sizeof(*a.resources);
  assert((char *)a.text==(char *)a.nodes+text_offset);
  assert(!a.resource_count ? !a.resources :
   (char *)a.resources==(char *)a.nodes+resource_offset);
  assert(!((uintptr_t)a.resources%alignment));
  assert(bytes[0]>=text_offset);
  size_t text_bytes=bytes[0]-text_offset;
  assert(text_bytes>0 && text_bytes>=a.request.text_size && text_bytes<=OS_CONTROL_TREE_MAX_TEXT);
  if(label&&!strcmp(label,"long-comment"))assert(text_bytes==4);
  if(label&&!strcmp(label,"max-nodes"))assert(text_bytes==1);
  if(label)printf("UI text %s: arena=%zu file=%zu decoded=%u\n",
   label,text_bytes,size,a.request.text_size);
  if(label&&a.resource_count)printf("UI resources %s: arena=%zu previous=%zu\n",
   label,(size_t)a.resource_count*sizeof(*a.resources),(size_t)a.request.count*sizeof(*a.resources));
  size_t old_bytes=0;for(unsigned i=candidate_calls;i<calls;++i)old_bytes+=bytes[i];
  if(label)printf("UI capacity %s: nodes=%u total_arena=%zu previous=%zu allocations=1/%u\n",
   label,a.request.count,bytes[0],old_bytes,calls-candidate_calls);
 }else{
  assert(!a.nodes&&!a.identifiers&&!a.text&&!a.resources&&ae.message&&ae.offset<=size);
  assert(!b.nodes&&!b.identifiers&&!b.text&&!b.resources&&be.message&&be.offset<=size);
 }
 ui_file_destroy(&a);reference_destroy(&b);assert(!live);++comparisons;
}
static char *read_file(const char *path,size_t *size){
 FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);
 rewind(f);char *data=malloc((size_t)n+1);assert(data&&fread(data,1,(size_t)n,f)==(size_t)n);
 fclose(f);data[n]=0;*size=(size_t)n;return data;
}
static void append(char *buffer,size_t *used,const char *format,...){
 assert(buffer&&*used<=UI_FILE_MAX_BYTES);size_t remaining=UI_FILE_MAX_BYTES+1-*used;
 va_list args;va_start(args,format);int n=vsnprintf(buffer+*used,remaining,format,args);va_end(args);
 assert(n>=0&&(size_t)n<remaining);*used+=(size_t)n;
}
int main(int argc,char **argv){
 assert(argc==3);size_t ui_size,desktop_size;
 char *ui=read_file(argv[1],&ui_size),*desktop=read_file(argv[2],&desktop_size);
 compare(ui,ui_size,"controls");compare(desktop,desktop_size,"desktop");
 const char *quoted="<!-- <button id='fake'/> \" </container> -->"
  "<container <!-- <text id='x'/> --> id='root'>"
  "<text id='label' text='a > b \" c &lt;d'/><!-- <button id='no'/> -->"
  "</container <!-- <button id='tail'/> -->>";
 compare(quoted,strlen(quoted),"quoted-comments");
 const char *bindings="<container id='root'><button id='program' text='icon program app-directory'/>"
  "<!-- program='fake' icon='fake' --><button id='bound' icon<!-- ignored -->='icon.png' "
  "program='demo' app-directory='/usr/apps/demo'/></container>";
 compare(bindings,strlen(bindings),"resource-tokens");
 char *large=malloc(UI_FILE_MAX_BYTES+1);assert(large);size_t length=0;
 append(large,&length,"<!-- ");
 for(unsigned i=0;i<2000;++i)append(large,&length,"<button id='fake'/> \" </container> ");
 append(large,&length,"--><container id='root'><text id='label' text='real'/></container>");
 compare(large,length,"long-comment");
 length=0;append(large,&length,"<container id='root'>");
 for(unsigned i=1;i<OS_CONTROL_TREE_MAX_NODES;++i)
  append(large,&length,"<button id='n%u'/>",i);
 size_t closing=length;append(large,&length,"</container>");
 compare(large,length,"max-nodes");
 length=closing;append(large,&length,"<button id='extra'/></container>");
 compare(large,length,0);reset();ui_file_document_t d={0};ui_file_error_t e={0};
 assert(!ui_file_parse(large,length,&d,&e)&&!calls&&!live);
 /* 大树仅有一个应用绑定时，只应分配一条资源记录。 */
 length=0;append(large,&length,"<container id='root'>");
 for(unsigned i=1;i<OS_CONTROL_TREE_MAX_NODES-1U;++i)
  append(large,&length,"<button id='n%u'/>",i);
 append(large,&length,"<button id='app' app-directory='/usr/apps/demo' program='demo' icon='icon.png'/></container>");
 compare(large,length,"sparse-resources");
 /* 原始转义文本可超过文字上限，只要解码结果仍在上限以内。 */
 length=0;append(large,&length,"<container id='root'><text id='label' text='");
 for(unsigned i=0;i<20000;++i)append(large,&length,"&amp;");
 append(large,&length,"'/></container>");compare(large,length,"large-entities");
 for(unsigned excess=0;excess<2;++excess){
  length=0;append(large,&length,"<container id='root'><text id='label' text='");
  for(unsigned i=0;i<OS_CONTROL_TREE_MAX_TEXT+excess;++i)append(large,&length,"x");
  append(large,&length,"'/></container>");
  compare(large,length,excess?0:"text-limit");
  reset();bool ok=ui_file_parse(large,length,&d,&e);assert(ok==!excess);
  ui_file_destroy(&d);assert(!live);
 }
 unsigned state=0x3428ac91;char damaged[2048];
 const char *samples[]={ui,desktop,quoted};size_t sizes[]={ui_size,desktop_size,strlen(quoted)};
 for(unsigned n=0;n<9000;++n){
  unsigned which=n%3;size_t size=sizes[which];assert(size<sizeof(damaged));
  memcpy(damaged,samples[which],size);state=state*1664525U+1013904223U;
  size_t at=state%size;damaged[at]=(char)(state>>24);
  compare(damaged,n&1?size:at,0);
 }
 for(size_t n=0;n<strlen(quoted);++n)compare(quoted,n,0);
 reset();fail_at=1;
 assert(!ui_file_parse(desktop,desktop_size,&d,&e)&&!live&&!d.nodes&&!d.identifiers&&!d.resources);
 printf("UI capacity: %u old/new parse comparisons, quote/comment boundaries, exact arenas and rollback passed\n",comparisons);
 free(large);free(ui);free(desktop);
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='control-file-capacity-') as directory:
    path = Path(directory)
    (path / 'current.c').write_text(wrappers + loader)
    (path / 'previous.c').write_text(wrappers + rename + previous)
    (path / 'test.c').write_text(checks)
    command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'user/runtime'), '-I' + str(root / 'include'),
               str(path / 'test.c'), str(path / 'current.c'), str(path / 'previous.c'),
               '-o', str(path / 'test')]
    arguments = [str(path / 'test'), str(root / 'user/ui/controls.ui'), str(root / 'user/ui/desktop.ui')]
    subprocess.run(command, check=True)
    subprocess.run(arguments, check=True)
    # 回退字节数猜测仍可能画对界面，但必须被精确分配预算拒绝。
    (path / 'current.c').write_text(wrappers + loader.replace(reservation, node_overreservation))
    subprocess.run(command, check=True)
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    assert result.returncode != 0 and b'a.identifiers==' in result.stderr, result.stderr
    print('Negative control rejected: file-byte-based node overreservation')
    # 即使界面结果相同，也拒绝为每个普通控件分配应用资源记录。
    oversized = loader.replace(resource_reservation, '(size_t)parser.capacity*sizeof(*candidate.resources)')
    (path / 'current.c').write_text(wrappers + oversized)
    subprocess.run(command, check=True)
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    assert result.returncode != 0 and b'(char *)a.text==' in result.stderr, result.stderr
    print('Negative control rejected: per-node application resource overreservation')
    oversized = loader.replace(text_reservation,
        'candidate.nodes=malloc(text_offset+(size<OS_CONTROL_TREE_MAX_TEXT?size:OS_CONTROL_TREE_MAX_TEXT));')
    (path / 'current.c').write_text(wrappers + oversized)
    subprocess.run(command, check=True)
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    assert result.returncode != 0 and b'text_bytes==4' in result.stderr, result.stderr
    print('Negative control rejected: file-byte-based text overreservation')
    (path / 'current.c').write_text(wrappers + previous)
    subprocess.run(command, check=True)
    result = subprocess.run(arguments, capture_output=True, timeout=30)
    assert result.returncode != 0 and b'candidate_calls==1' in result.stderr, result.stderr
    print('Negative control rejected: multiple document allocations')
