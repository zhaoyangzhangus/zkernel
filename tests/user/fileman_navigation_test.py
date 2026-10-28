"""以真实临时目录验证生产文件管理器，而非仅检查按钮文案。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
app = (root / 'user/desktop/fileman/main.c').read_text()
app = app.replace('static uint32_t fm4_measure_range(const char *text,size_t length) {',
    'static size_t measure_bytes;\nstatic uint32_t fm4_measure_range(const char *text,size_t length) {\n    measure_bytes+=length;')
start = app.index('static int64_t fileman_syscall_one(')
end = app.index('static int64_t fileman_syscall_three(', start)
app = app[:start] + 'static int64_t fileman_syscall_one(uint64_t, uint64_t);\n' + app[end:]
def allowed_flags(path, marker):
    source = (root / path).read_text()
    start = source.index(marker) + len(marker)
    return source[start:source.index(')) != 0U', start)]
syscall_flags = allowed_flags('kernel/syscall/graphics.c', '(arguments.flags & ~(')
server_flags = allowed_flags('kernel/graphics/window.c', '(flags & ~(')
libc = (root / 'user/libc/stdlib.c').read_text()
sort = libc[libc.index('static void swap_bytes('):libc.index('void *bsearch(')]
sort = sort.replace('void qsort(', 'static void fileman_snapshot_qsort(')
sort = sort.replace('    unsigned char *bytes = (unsigned char *)base;',
    '    assert(size==sizeof(void *));\n    unsigned char *bytes = (unsigned char *)base;')
program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <uapi/control_validation.h>
#include <kernel/control_tree.h>
static atomic_bool copy_gate,copy_entered,copy_release;
static ssize_t fileman_test_read(int fd,void *data,size_t size){
 if(atomic_load(&copy_gate)){
  atomic_store(&copy_entered,true);
  while(!atomic_load(&copy_release))usleep(1000);
 }
 return read(fd,data,size);
}
#define read fileman_test_read
/* 自有排序 */
#define qsort fileman_snapshot_qsort
static int fail_allocation=-1;
static size_t last_malloc_size;
static void *fileman_test_malloc(size_t n){
 last_malloc_size=n;
 if(fail_allocation==0)return NULL;
 if(fail_allocation>0)--fail_allocation;
 return malloc(n);
}
static void *fileman_test_realloc(void *p,size_t n){
 if(fail_allocation==0)return NULL;
 if(fail_allocation>0)--fail_allocation;
 return realloc(p,n);
}
#define malloc fileman_test_malloc
#define realloc fileman_test_realloc
#define main fileman_application_main
''' + app + r'''
#undef main
#undef malloc
#undef realloc
#undef read
static unsigned enumerate_calls;
static unsigned batch_calls;
static int malformed_batch;
static unsigned remove_calls;
static int remove_failure;
static char removed_path[256];
static int fail_enumeration=-1;
static int fail_batch_status;
static bool fail_tree_submit;
static const os_control_node_t *submitted_nodes;
static const char *submitted_text;
static unsigned window_updates,text_draws;
static int64_t fileman_syscall_one(uint64_t op, uint64_t arg) {
 if(op==OS_SYS_FILE_ENUMERATE_BATCH){
  os_file_enumerate_batch_t *r=(void*)(uintptr_t)arg;++batch_calls;
  if(fail_batch_status)return fail_batch_status;
  if(malformed_batch){r->count=malformed_batch==1?0:r->capacity+1;return 0;}
  r->count=0;
  while(r->count<r->capacity){
   os_file_enumerate_t one={.path=r->path,.index=r->index+r->count};
   int64_t status=fileman_syscall_one(OS_SYS_FILE_ENUMERATE,(uint64_t)(uintptr_t)&one);
   if(status==-2&&r->count)return 0;
   if(status)return status;
   ((os_file_info_t*)(uintptr_t)r->entries)[r->count++]=one.info;
  }
  return 0;
 }
 if(op==OS_SYS_FILE_REMOVE){
  os_file_path_op_t *r=(void*)(uintptr_t)arg;
  const char *path=(void*)(uintptr_t)r->path;
  ++remove_calls;copy_text(removed_path,sizeof removed_path,path);
  if(remove_failure)return remove_failure;
  return unlink(path)?-errno:0;
 }
 if(op==OS_SYS_FILE_STAT) {
  os_file_stat_t *r=(void*)(uintptr_t)arg;struct stat s;
  if(!strcmp((const char*)(uintptr_t)r->path,"/__fileman_large__")){
   r->info.type=OS_FILE_TYPE_DIRECTORY;return 0;
  }
  if(stat((const char*)(uintptr_t)r->path,&s))return -errno;
  r->info.type=S_ISDIR(s.st_mode)?OS_FILE_TYPE_DIRECTORY:OS_FILE_TYPE_REGULAR;
  r->info.size=s.st_size;return 0;
 }
 if(op==OS_SYS_FILE_ENUMERATE) {
  os_file_enumerate_t *r=(void*)(uintptr_t)arg;
  ++enumerate_calls;
  if((int)r->index==fail_enumeration)return -EIO;
  if(!strcmp((const char*)(uintptr_t)r->path,"/__fileman_large__")) {
   if(r->index==1200)return -2;
   snprintf(r->info.name,sizeof r->info.name,"item-%04u",r->index);
   r->info.type=OS_FILE_TYPE_REGULAR;r->info.size=r->index;return 0;
  }
  DIR *d=opendir((const char*)(uintptr_t)r->path);if(!d)return -errno;
  struct dirent *e;unsigned i=0;
  while((e=readdir(d))) {
   if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
   if(i++!=r->index)continue;
   char path[512];snprintf(path,sizeof path,"%s/%s",(char*)(uintptr_t)r->path,e->d_name);
   struct stat s;assert(!stat(path,&s));
   r->info.type=S_ISDIR(s.st_mode)?OS_FILE_TYPE_DIRECTORY:OS_FILE_TYPE_REGULAR;
   r->info.size=s.st_size;copy_text(r->info.name,sizeof r->info.name,e->d_name);
   closedir(d);return 0;
  }
  closedir(d);return -2;
 }
 if(op==OS_SYS_WINDOW_CONTROL_TREE){
  os_window_control_tree_t *r=(void *)(uintptr_t)arg;
  const os_control_node_t *nodes=(void *)(uintptr_t)r->nodes;
  assert(r->count>=FM4_FRAME_BASE_COUNT && nodes[0].flags&OS_CONTROL_WINDOW_FRAME);
  for(unsigned i=0;i<r->count;++i)assert(nodes[i].kind!=OS_CONTROL_SURFACE);
  assert(os_control_tree_validate(nodes,r->count,r->text_size));
  unsigned base=nodes[0].identifier==ui_message_target(&g_delete_message,0)?1U:0U;
  assert(nodes[base+8].kind==OS_CONTROL_CONTAINER&&nodes[base+8].y==(int32_t)user_caption_height());
  assert(nodes[base+9].y==0&&nodes[base+9].width==(int32_t)FM4_SIDEBAR_WIDTH);
  assert(nodes[base+9].background==OS_THEME_TOOLBAR);
  bool title=false;
  for(unsigned i=0;i<r->count;++i)if(nodes[i].identifier==35U) {
   title=true;assert(nodes[i].parent==base+9&&nodes[i].height==(int32_t)ui_text_size());
   assert(nodes[i].y==(int32_t)((user_caption_height()-ui_text_size())/2U));
   assert(nodes[i].flags&OS_CONTROL_POINTER_PASSTHROUGH);
  }
  assert(title);
  if(fail_tree_submit){fail_tree_submit=false;return -ENOMEM;}
  submitted_nodes=nodes;submitted_text=(const void *)(uintptr_t)r->text;
  static uint64_t revision; r->revision=++revision;return 0;
 }
 if(op==OS_SYS_WINDOW_UPDATE){++window_updates;return 0;}
 assert(!"unexpected native operation");return -1;
}
bool ui_text_init(uint32_t s){(void)s;return true;}
bool ui_text_adjust(int32_t d){(void)d;return true;}
static uint32_t test_text_size=16;
uint32_t ui_text_size(void){return test_text_size;}
uint32_t ui_text_advance(void){return 8;}
uint32_t ui_text_line_height(void){return test_text_size*3/2;}
static void valid_utf8(const char *s,size_t n){
 for(size_t i=0;i<n;){
  unsigned c=(unsigned char)s[i++],tail=0;
  if(c<128)continue;
  if(c>=0xc2&&c<=0xdf)tail=1;
  else if(c>=0xe0&&c<=0xef)tail=2;
  else if(c>=0xf0&&c<=0xf4)tail=3;
  else assert(!"invalid UTF-8 leading byte");
  while(tail--){assert(i<n);assert(((unsigned char)s[i++]&0xc0)==0x80);}
 }
}
uint32_t ui_text_measure(const char *s){valid_utf8(s,strlen(s));return fm4_measure(s);}
uint32_t ui_text_measure_range(const char *s,size_t n){valid_utf8(s,n);return fm4_measure_range(s,n);}
void ui_text_draw(uint32_t*p,uint32_t s,uint32_t w,uint32_t h,int32_t x,int32_t y,const char*t,uint32_t c){
 ++text_draws;
 (void)p;(void)s;(void)w;(void)h;(void)x;(void)y;(void)t;(void)c;
}
void ui_text_draw_rect(uint32_t*p,uint32_t s,uint32_t w,uint32_t h,int32_t x,int32_t y,
 uint32_t l,uint32_t top,uint32_t r,uint32_t b,const char*t,uint32_t c){
 (void)l;(void)top;(void)r;(void)b;ui_text_draw(p,s,w,h,x,y,t,c);
}
static void key(unsigned code);
static uint32_t test_focus;
static unsigned overlay_index(void){
 for(unsigned i=0;i<g_frame_tree.count;++i)
  if(g_frame->nodes[i].identifier==63U||g_frame->nodes[i].identifier==ui_message_target(&g_delete_message,6))return i;
 return g_frame_tree.count;
}
#define g_frame_overlay_index overlay_index()
static os_window_event_t control_event(unsigned index,unsigned action){
 const os_control_node_t *n=g_frame->nodes+index;
 return (os_window_event_t){.type=OS_WINDOW_EVENT_CONTROL,.control={.target=n->identifier,
  .node_index=index,.node_kind=n->kind,.tree_revision=g_frame_tree.revision,.user_data=n->user_data,.action=action}};
}
static void copy_wait(void){
 for(unsigned i=0;g_copy_active&&i<5000;++i){fm4_copy_poll();usleep(1000);}
 assert(!g_copy_active&&!g_copy_error);
}
static void select_name(const char *name){
 for(unsigned i=0;i<g_entry_count;++i)if(!strcmp(g_entries[i]->info.name,name)){
  g_selected=i;g_selection_valid=true;return;
 }
 assert(!"missing test entry");
}
/* 测试端执行真实公共焦点算法，再派发与服务端相同的语义事件。 */
static unsigned dialog_button(unsigned choice) {
 for(unsigned i=0;i<g_frame_tree.count;++i)
  if(g_frame->nodes[i].identifier==ui_message_target(&g_delete_message,choice))return i;
 assert(!"missing dialog button");return 0;
}
static void key(unsigned code){
 if(g_delete_message.active && code!=0xe0 && code!=0xe1){
  Rect viewport=control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,
   g_window.width,g_window.height,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
  if((test_focus&~255U)!=ui_message_target(&g_delete_message,0))
   test_focus=ui_message_target(&g_delete_message,3);
  if(code==0x2b||code==0x4f||code==0x50){
   unsigned next;
   assert(control_tree_focus_next_query(g_frame->nodes,g_frame_tree.count,viewport,test_focus,
    code==0x50 || (code==0x2b&&g_shift),&next,0,0,0));
   assert(next!=OS_CONTROL_NO_PARENT);test_focus=g_frame->nodes[next].identifier;return;
  }
  if(code==0x28||code==0x58||code==0x2c){
   unsigned index=control_tree_find(g_frame->nodes,g_frame_tree.count,viewport,test_focus,0);
   assert(index!=OS_CONTROL_NO_PARENT);os_window_event_t e=control_event(index,OS_CONTROL_CLICK);
   handle_event(&e);return;
  }
  os_window_event_t e=control_event(0,OS_CONTROL_KEY);
  e.control.code=code;e.control.value=OS_INPUT_VALUE_PRESS;handle_event(&e);return;
 }
 os_window_event_t e={.type=OS_WINDOW_EVENT_INPUT,
  .input={.type=OS_INPUT_EVENT_KEY,.value=OS_INPUT_VALUE_PRESS,.code=code}};handle_key(&e);
}
static void dialog_layout_test(void){
 const unsigned widths[]={1,2,32,120,320,640,960,1920};
 const unsigned heights[]={1,2,60,200,640};
 ui_message_show(&g_delete_message,"删除确认","测试项目",OS_MESSAGE_DELETE_CANCEL);
 for(unsigned size=12;size<=48;size+=4){
  test_text_size=size;
  for(unsigned w=0;w<sizeof widths/sizeof *widths;++w)
   for(unsigned h=0;h<sizeof heights/sizeof *heights;++h){
    g_window.width=widths[w];g_window.height=heights[h];
    fm4_frame_buffer_t *prepared=g_candidate;
    memset(prepared->nodes,0xa5,sizeof prepared->nodes);
    assert(fm4_publish_frame());
    assert(g_frame_tree.count<FM4_FRAME_NODE_COUNT);
    const unsigned char *unused=(const void *)(prepared->nodes+FM4_FRAME_NODE_COUNT-1);
    for(unsigned byte=0;byte<sizeof(*g_candidate->nodes);++byte)assert(unused[byte]==0xa5);
    /* 更换候选缓冲垃圾字节后，同一界面仍须逐字节一致，不能重复提交或泄露残留字段。 */
    uint64_t revision=g_frame_tree.revision;
    memset(g_candidate->nodes,0x5a,sizeof g_candidate->nodes);
    assert(fm4_publish_frame()&&g_frame_tree.revision==revision);
    Rect viewport=control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,
     g_window.width,g_window.height,OS_WINDOW_STANDARD_CAPTION_CONTROLS|OS_WINDOW_CAPTION_HEIGHT(user_caption_height()));
    for(unsigned choice=2;choice<=3;++choice){
     unsigned index=dialog_button(choice);control_tree_box_t box;
     assert(control_tree_find(g_frame->nodes,g_frame_tree.count,viewport,
      g_frame->nodes[index].identifier,&box)==index);
     Rect clipped={box.bounds.x0>box.clip.x0?box.bounds.x0:box.clip.x0,
      box.bounds.y0>box.clip.y0?box.bounds.y0:box.clip.y0,
      box.bounds.x1<box.clip.x1?box.bounds.x1:box.clip.x1,
      box.bounds.y1<box.clip.y1?box.bounds.y1:box.clip.y1};
     if(rect_is_empty(clipped))continue;
     int x=clipped.x0+(clipped.x1-clipped.x0)/2,y=clipped.y0+(clipped.y1-clipped.y0)/2;
     unsigned hit=control_tree_locate(g_frame->nodes,g_frame_tree.count,viewport,x,y,0);
     if(hit!=index)fprintf(stderr,"dialog font=%u size=%ux%u choice=%u rect=%d,%d,%d,%d hit=%u expected=%u\n",
      size,g_window.width,g_window.height,choice,clipped.x0,clipped.y0,clipped.x1,clipped.y1,hit,index);
     assert(hit==index);
    }
   }
 }
 test_text_size=16;g_window.width=960;g_window.height=640;g_delete_message.active=false;
}
static void candidate_reuse_test(void){
 unsigned entries=g_entry_count;
 size_t avoided=0;
 for(unsigned mode=0;mode<6;++mode){
  g_list_view=mode&1U;g_context_visible=mode==2U||mode==3U;
  g_context_keyboard=mode==3U;g_context_hover=FM4_CONTROL_CONTEXT_OPEN;
  g_entry_count=mode>=4U?0:entries;
  fm4_status(mode&1U?"测试状态文字":"READY");
  fm4_frame_buffer_t *previous=g_frame,*candidate=g_candidate;
  uint64_t before=g_frame_tree.revision;
  memset(g_candidate->nodes,0xa5,sizeof g_candidate->nodes);
  assert(fm4_publish_frame());uint64_t revision=g_frame_tree.revision;
  if(revision!=before){
   assert(g_frame==candidate&&g_candidate==previous);
   avoided+=(size_t)g_frame_tree.count*sizeof(*g_frame->nodes)+g_frame_tree.text_size;
  }else assert(g_frame==previous&&g_candidate==candidate);
  assert((const void *)(uintptr_t)g_frame_tree.nodes==submitted_nodes);
  assert((const void *)(uintptr_t)g_frame_tree.text==submitted_text);
  previous=g_frame;candidate=g_candidate;
  memset(g_candidate->nodes,0x5a,sizeof g_candidate->nodes);
  assert(fm4_publish_frame()&&g_frame_tree.revision==revision);
  assert(g_frame==previous&&g_candidate==candidate);
  const unsigned char *unused=(const void *)(g_candidate->nodes+FM4_FRAME_NODE_COUNT-1);
  for(unsigned byte=0;byte<sizeof(*g_candidate->nodes);++byte)assert(unused[byte]==0x5a);
 }
 g_entry_count=entries;g_list_view=false;g_context_visible=g_context_keyboard=false;fm4_status("READY");
 printf("Fileman candidates: dialog/grid/list/menu/empty/status snapshots independent of old bytes; %zu-byte full clear removed per frame\n",sizeof g_candidate->nodes);
 printf("Fileman publication: buffer identities alternate on success only; %zu bytes of node/text copying removed in six state changes\n",avoided);
}
int main(int argc,char **argv) {
 /* 标志校验 */
 assert(argc==2);char path[512];
 g_window.identifier=7;g_window.width=960;g_window.height=640;
 g_window.pixels=mmap(NULL,960*640*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(g_window.pixels!=MAP_FAILED);
 copy_text(g_path,sizeof g_path,argv[1]);assert(load_directory());
 dialog_layout_test();
 fm4_load_places();
 candidate_reuse_test();
 /* 图标、标签间距和命中卡片一起缩放，大图标不能越过字体缓存上限。 */
 for(unsigned size=16;size<=96;size+=16) {
  test_text_size=size;g_window.width=4096;g_window.height=2160;g_list_view=false;
  assert(fm4_publish_frame());unsigned icons=0;
  for(unsigned i=0;i<g_frame_tree.count;++i) {
   const os_control_node_t *n=g_frame->nodes+i;
   if((n->flags&OS_CONTROL_SYMBOL) &&
      (n->value==OS_CONTROL_SYMBOL_FOLDER || n->value==OS_CONTROL_SYMBOL_FILE)) {
    assert(n->icon_size==64U*size/16U && n->font_size==size);
    assert(n->y+n->height<=(int32_t)FM4_GRID_LABEL_TOP);
    assert(n->width<=g_frame->nodes[n->parent].width);++icons;
   }
  }
  assert(icons);
  assert(g_frame->nodes[11].width==(int32_t)size && g_frame->nodes[11].font_size==size);
 }
 test_text_size=16;g_window.width=960;g_window.height=640;
 fm4_load_volumes();
 for(unsigned i=0;i<g_volume_count;++i)assert(strcmp(g_volumes[i].path,"/"));
 g_volume_count=0;g_place_count=1;
 copy_text(g_places[0].path,sizeof g_places[0].path,g_path);
 copy_text(g_places[0].label,sizeof g_places[0].label,"主文件夹");
 render();
 /* 侧栏通过公共树命中，图标不截断导航点击，悬停不重画应用 Surface。 */
 assert(g_frame->nodes[10].kind==OS_CONTROL_LIST_ITEM&&(g_frame->nodes[10].flags&OS_CONTROL_CHECKED));
 assert(g_frame->nodes[11].value==OS_CONTROL_SYMBOL_HOME);
 control_tree_box_t sidebar_box;
 unsigned hit=control_tree_locate(g_frame->nodes,g_frame_tree.count,
  control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,960,640,OS_WINDOW_STANDARD_CAPTION_CONTROLS),
  20,20,&sidebar_box);
 assert(hit==10U);
 uint64_t sidebar_revision=g_frame_tree.revision;
 uint32_t sidebar_pixel=g_window.pixels[20*960+20];
 os_window_event_t sidebar_move=control_event(10,OS_CONTROL_ENTER);handle_event(&sidebar_move);
 assert(g_frame_tree.revision==sidebar_revision&&g_window.pixels[20*960+20]==sidebar_pixel);
 copy_text(g_places[0].path,sizeof g_places[0].path,"/__inactive_place__");render();
 assert(!(g_frame->nodes[10].flags&OS_CONTROL_CHECKED));
 copy_text(g_places[0].path,sizeof g_places[0].path,g_path);
 render();assert(g_frame->nodes[10].flags&OS_CONTROL_CHECKED);
 /* 有卷时分隔线可见，卷图标穿透，导航动作仍执行真实目录读取。 */
 copy_text(g_volumes[0].path,sizeof g_volumes[0].path,g_path);
 copy_text(g_volumes[0].label,sizeof g_volumes[0].label,"测试卷");g_volume_count=1;render();
 assert(g_frame->nodes[12].user_data==FM4_CONTROL_VOLUME_BASE);
 assert(g_frame->nodes[13].value==OS_CONTROL_SYMBOL_DRIVE);
 hit=control_tree_locate(g_frame->nodes,g_frame_tree.count,
  control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,960,640,OS_WINDOW_STANDARD_CAPTION_CONTROLS),
  25,FM4_VOLUME_FIRST_Y+16,&sidebar_box);
 assert(hit==12U);
 os_window_event_t volume_click=control_event(12,OS_CONTROL_CLICK);handle_event(&volume_click);
 assert(!strcmp(g_path,g_volumes[0].path));
 g_volume_count=0;render();
 assert(g_frame->nodes[FM4_FRAME_BASE_COUNT-4U].flags&OS_CONTROL_HIDDEN);
 /* 发布失败不能覆盖用于快速事件绑定的已发布树或版本。 */
 os_control_node_t published_nodes[FM4_FRAME_NODE_COUNT];
 char published_text[FM4_FRAME_TEXT_CAPACITY];
 memcpy(published_nodes,g_frame->nodes,sizeof published_nodes);
 memcpy(published_text,g_frame->text,sizeof published_text);
 fm4_frame_buffer_t *published=g_frame,*candidate=g_candidate;
 sidebar_revision=g_frame_tree.revision;g_list_view=true;fail_tree_submit=true;
 assert(!fm4_publish_frame());
 assert(g_frame==published&&g_candidate==candidate&&!g_frame_input_ready);
 assert(!memcmp(published_text,g_frame->text,sizeof published_text));
 assert(g_frame_tree.revision==sidebar_revision&&!memcmp(published_nodes,g_frame->nodes,sizeof published_nodes));
 g_list_view=false;assert(fm4_publish_frame()&&g_frame_tree.revision==sidebar_revision);
 assert(g_frame==published&&g_candidate==candidate&&g_frame_input_ready);
 /* 将整个文件 Surface 设为只读，证明纯控件变化没有偷偷绘制或提交文件像素。 */
 select_name("Alpha.txt");render();
 assert(g_window.pixels[639*960+959]==0);
 unsigned updates_before=window_updates,draws_before=text_draws;
 assert(!mprotect(g_window.pixels,960*640*4,PROT_READ));
 /* 文件卡片事件携带真实索引；错误坐标不能影响命中，悬停不重新发布。 */
 unsigned card=0;
 for(unsigned i=0;i<g_frame_tree.count;++i)
  if(g_frame->nodes[i].identifier==FM4_ITEM_ID_BASE+g_selected)card=i;
 assert(card&&g_frame->nodes[card].kind==OS_CONTROL_LIST_ITEM);
 os_window_event_t hover=control_event(card,OS_CONTROL_ENTER);
 uint64_t hover_revision=g_frame_tree.revision;handle_event(&hover);
 assert(g_frame_tree.revision==hover_revision);
 g_last_click_item=FM4_INDEX_NONE;
 os_window_event_t file_click=control_event(card,OS_CONTROL_CLICK);
 file_click.pointer_x=file_click.pointer_y=-1000;file_click.control.timestamp=12345;
 handle_event(&file_click);assert(g_selection_valid&&!strcmp(g_entries[g_selected]->info.name,"Alpha.txt"));
 os_window_event_t root_key=control_event(0,OS_CONTROL_KEY);
 root_key.control.code=0xE0;root_key.control.value=OS_INPUT_VALUE_PRESS;
 handle_event(&root_key);assert(g_ctrl);
 root_key.control.value=OS_INPUT_VALUE_RELEASE;
 if(root_key.control.tree_revision>1)--root_key.control.tree_revision;
 handle_event(&root_key);assert(!g_ctrl);
 /* 聚焦卡片在字号/布局刷新后仍须接收已排队的修饰键释放。 */
 os_window_event_t modifier_key=control_event(card,OS_CONTROL_KEY);
 modifier_key.control.code=0xE0;modifier_key.control.value=OS_INPUT_VALUE_PRESS;
 handle_event(&modifier_key);assert(g_ctrl);
 g_list_view=true;render();
 modifier_key.control.value=OS_INPUT_VALUE_RELEASE;
 handle_event(&modifier_key);assert(!g_ctrl);
 /* 新目录视图未发布成功时，旧点击只触发重试，不能变成一次双击打开。 */
 g_list_view=false;fail_tree_submit=true;assert(!fm4_publish_frame()&&!g_frame_input_ready);
 unsigned selected_before=g_selected;uint64_t last_before=g_last_click_time;
 handle_event(&file_click);
 assert(g_frame_input_ready&&g_selected==selected_before&&g_last_click_time==last_before);
 for(unsigned repeat=0;repeat<3;++repeat){
  os_window_event_t button=control_event(7,OS_CONTROL_CLICK);handle_event(&button);
  assert(g_context_visible);key(0x4a);key(0x51);key(0x29);assert(!g_context_visible);
 }
 uint32_t selected_x,selected_y;assert(fm4_item_rect(g_selected,&selected_x,&selected_y));
 os_window_event_t right_click={.type=OS_WINDOW_EVENT_INPUT,
  .pointer_x=selected_x+4,.pointer_y=selected_y+4,
  .input={.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_RIGHT,.value=OS_INPUT_VALUE_PRESS}};
 handle_event(&right_click);assert(g_context_visible);key(0x29);
 key(0x4c);assert(g_delete_message.active);key(0x4f);key(0x50);
 os_window_event_t cancel=control_event(dialog_button(3),OS_CONTROL_CLICK);handle_event(&cancel);
 assert(!g_delete_message.active);
 g_copy_active=true;atomic_store(&g_copy_context.bytes,123*1024);fm4_copy_poll();g_copy_active=false;
 assert(strstr(g_status,"123 KiB"));
 assert(!(g_frame->nodes[g_frame_overlay_index-5U].flags&OS_CONTROL_HIDDEN));
 const os_control_node_t *status=g_frame->nodes+g_frame_overlay_index-4U;
 assert(status->text_length==strlen(g_status)&&!memcmp(g_frame->text+status->text_offset,g_status,status->text_length));
 unsigned status_hit=control_tree_locate(g_frame->nodes,g_frame_tree.count,
  control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,960,640,OS_WINDOW_STANDARD_CAPTION_CONTROLS),
  FM4_SIDEBAR_WIDTH+32,620,&sidebar_box);
 assert(status_hit==OS_CONTROL_NO_PARENT || g_frame->nodes[status_hit].identifier>=FM4_ITEM_ID_BASE);
 fm4_status("READY");render();
 assert(g_frame->nodes[g_frame_overlay_index-5U].flags&OS_CONTROL_HIDDEN);
 os_window_event_t location_only=control_event(3,OS_CONTROL_CLICK);handle_event(&location_only);
 key(0x04);g_ctrl=true;key(0x04);g_ctrl=false;key(0x28);
 assert(g_edit_mode!=FM4_EDIT_LOCATION&&!strcmp(g_path,argv[1]));
 fm4_status("READY");render();
 assert(window_updates==updates_before&&text_draws==draws_before);
 assert(!mprotect(g_window.pixels,960*640*4,PROT_READ|PROT_WRITE));
 puts("Fileman control-only changes: read-only Surface, zero pixel submissions and zero application text draws PASS");
 fm4_history_reset();assert(g_entry_count==3);
 assert(!strcmp(g_entries[0]->info.name,"zdir"));
 assert(!strcmp(g_entries[1]->info.name,"Alpha.txt"));
 g_sort_descending=true;fm4_sort();assert(!strcmp(g_entries[1]->info.name,"beta.txt"));
 copy_text(g_query,sizeof g_query,"ALP");assert(load_directory());assert(g_entry_count==1);
 assert(fm4_navigate(argv[1],true));assert(!g_query[0]&&g_entry_count==3);
 unsigned before=enumerate_calls;
 g_ctrl=true;key(0x09);g_ctrl=false;key(0x05);assert(!strcmp(g_query,"b")&&g_entry_count==1);
 key(0x29);assert(!g_query[0]&&g_entry_count==3);
 /* 搜索后单击文件转移键盘操作权，保留筛选结果，Delete 必须进入删除确认。 */
 g_ctrl=true;key(0x09);g_ctrl=false;key(0x05);
 assert(g_edit_mode==FM4_EDIT_SEARCH&&g_entry_count==1&&!strcmp(g_query,"b"));
 g_last_click_item=FM4_INDEX_NONE;
 unsigned result_node=0;
 while(result_node<g_frame_tree.count&&g_frame->nodes[result_node].identifier!=FM4_ITEM_ID_BASE)++result_node;
 assert(result_node<g_frame_tree.count);
 os_window_event_t search_result=control_event(result_node,OS_CONTROL_CLICK);
 handle_event(&search_result);
 assert(g_edit_mode==FM4_EDIT_NONE&&!g_edit_select_all&&g_selection_valid);
 assert(!strcmp(g_query,"b")&&g_entry_count==1);
 assert(g_frame->nodes[3].text_length==1&&g_frame->text[g_frame->nodes[3].text_offset]=='b');
 assert(g_frame->nodes[4].background==OS_THEME_ACCENT&&g_frame->nodes[4].foreground==OS_THEME_ON_ACCENT&&
  !(g_frame->nodes[4].flags&OS_CONTROL_BACKGROUND_ALPHA));
 key(0x4c);assert(g_delete_message.active&&strstr(g_delete_path,"/beta.txt"));
 key(0x29);assert(!g_delete_message.active&&!remove_calls);
 os_window_event_t edit_query=control_event(3,OS_CONTROL_CLICK);handle_event(&edit_query);
 assert(g_edit_mode==FM4_EDIT_SEARCH&&g_edit_select_all);
 key(0x28);assert(g_edit_mode==FM4_EDIT_NONE&&g_entry_count==1);
 assert(g_frame->nodes[3].text_length==1&&g_frame->text[g_frame->nodes[3].text_offset]=='b');
 key(0x29);assert(!g_query[0]&&g_entry_count==3);
 assert(g_frame->nodes[4].flags&OS_CONTROL_BACKGROUND_ALPHA);
 assert(g_frame->nodes[3].text_length==strlen(g_path)&&!memcmp(g_frame->text,g_path,strlen(g_path)));
 /* 搜索提交后，搜索按钮仍能一次清除筛选；Ctrl+L 始终编辑真实目录。 */
 g_ctrl=true;key(0x09);g_ctrl=false;key(0x05);key(0x28);
 os_window_event_t cancel_search=control_event(4,OS_CONTROL_CLICK);handle_event(&cancel_search);
 assert(g_edit_mode==FM4_EDIT_NONE&&!g_query[0]&&g_entry_count==3);
 g_ctrl=true;key(0x09);g_ctrl=false;key(0x05);key(0x28);
 g_ctrl=true;key(0x0f);g_ctrl=false;
 assert(g_edit_mode==FM4_EDIT_LOCATION&&!strcmp(g_location,g_path));
 assert(g_frame->nodes[3].text_length==strlen(g_path)&&!memcmp(g_frame->text,g_path,strlen(g_path)));
 key(0x29);assert(g_edit_mode==FM4_EDIT_NONE&&!g_query[0]&&g_entry_count==3);
 assert(enumerate_calls==before);
 /* 独立保留原逐次缩短规则，逐字节核对新累计算法和容量边界。 */
 const char *fit_samples[]={"中文文件夹与图片名称很长.txt","abcdefghijklmnopqrstuvwxyz.txt",
  "a中b😀c文d.png","短","", "a"};
 unsigned fit_cases=0;
 for(unsigned f=8;f<=96;f+=4) {
  uint32_t saved_font=test_text_size;test_text_size=f;
  for(unsigned sample=0;sample<sizeof(fit_samples)/sizeof(fit_samples[0]);++sample)
   for(unsigned capacity=1;capacity<=64;++capacity)
    for(unsigned w=0;w<=640;w+=5) {
     char name[128],expected[128];strcpy(name,fit_samples[sample]);strcpy(expected,name);
     uint32_t length=(uint32_t)strlen(expected);
     if(length>=capacity)length=capacity-1U;
     if(fm4_measure(expected)>w) {
      if(!length)expected[0]=0;
      uint32_t suffix=fm4_measure("…");
      while(length) {
       do{--length;}while(length&&((unsigned char)expected[length]&0xc0U)==0x80U);
       expected[length]=0;
       if(length+3U<capacity&&suffix<=w&&fm4_measure(expected)<=w-suffix) {
        strcat(expected,"…");break;
       }
      }
     }
     measure_bytes=0;fm4_fit_text(name,capacity,w);
     assert(measure_bytes<=strlen(fit_samples[sample]));
     if(strcmp(name,expected))fprintf(stderr,"FIT f=%u sample=%u cap=%u w=%u got=%s expected=%s\n",f,sample,capacity,w,name,expected);
     assert(!strcmp(name,expected));valid_utf8(name,strlen(name));++fit_cases;
    }
  test_text_size=saved_font;
 }
 printf("Fileman text fitting: %u original-rule byte oracles, UTF-8/font/capacity boundaries, one full measure PASS\n",fit_cases);

 g_list_view=true;assert(fm4_columns()==1);
 for(unsigned i=0;i<3;++i){unsigned x,y;assert(fm4_item_rect(i,&x,&y));assert(fm4_item_at(x+2,y+2)==i);}
 g_list_view=false;
 fm4_begin_name(1);copy_text(g_location,sizeof g_location,"created");assert(fm4_finish_name());
 select_name("created");fm4_begin_name(2);copy_text(g_location,sizeof g_location,"renamed");assert(fm4_finish_name());
 copy_text(g_location,sizeof g_location,"../escape");assert(!fm4_finish_name());
 copy_text(g_location,sizeof g_location,"Alpha.txt");assert(!fm4_finish_name());
 g_name_action=0;g_edit_mode=FM4_EDIT_NONE;select_name("Alpha.txt");fm4_copy_selection(false);
 snprintf(path,sizeof path,"%s/renamed",argv[1]);assert(fm4_navigate(path,true));assert(fm4_paste());copy_wait();
 assert(!fm4_paste());assert(g_entry_count==1);
 assert(fm4_history_go(-1));select_name("beta.txt");fm4_copy_selection(true);
 assert(fm4_navigate(path,true));assert(fm4_paste()&&!g_clipboard[0]);
 assert(fm4_history_go(-1));assert(g_entry_count==3);
 fm4_show_context(958,638);assert(g_context_x+FM4_CONTEXT_WIDTH<=960);
 render();
 for(unsigned i=0;i<FM4_CONTEXT_ROWS;++i){
  control_tree_box_t box;
  uint32_t hit=control_tree_locate(g_frame->nodes,g_frame_tree.count,
   control_tree_window_viewport(g_frame->nodes,g_frame_tree.count,960,640,OS_WINDOW_STANDARD_CAPTION_CONTROLS),
   g_context_x+10,g_context_y+i*FM4_CONTEXT_ROW_HEIGHT+4,&box);
  assert(hit==g_frame_overlay_index+1U+i&&g_frame->nodes[hit].user_data==FM4_CONTROL_CONTEXT_OPEN+i);
 }
 render();
 /* 鼠标条目由公共控件命中和绘制，应用处理关联事件但不重提交或改写 Surface。 */
 fm4_show_context(400,100);
 render();
 for(unsigned i=0;i<FM4_CONTEXT_ROWS;++i){
  uint64_t revision=g_frame_tree.revision;
  uint32_t pixel=g_window.pixels[(100+i*FM4_CONTEXT_ROW_HEIGHT+FM4_CONTEXT_ROW_HEIGHT/2)*960+420];
  const os_control_node_t *item=g_frame->nodes+g_frame_overlay_index+1U+i;
  os_window_event_t e={.type=OS_WINDOW_EVENT_CONTROL,.control={.target=item->identifier,.node_index=g_frame_overlay_index+1U+i,
   .node_kind=item->kind,.tree_revision=revision,.user_data=item->user_data,.action=OS_CONTROL_ENTER}};
  handle_event(&e);
  assert(g_context_hover==FM4_CONTROL_CONTEXT_OPEN+i);
  assert(g_frame_tree.revision==revision&&g_window.pixels[(100+i*FM4_CONTEXT_ROW_HEIGHT+FM4_CONTEXT_ROW_HEIGHT/2)*960+420]==pixel);
 }
 const os_control_node_t *last=g_frame->nodes+g_frame_overlay_index+FM4_CONTEXT_ROWS;
 os_window_event_t outside={.type=OS_WINDOW_EVENT_CONTROL,.control={.target=last->identifier,
  .node_index=g_frame_overlay_index+FM4_CONTEXT_ROWS,.node_kind=last->kind,.tree_revision=g_frame_tree.revision,
  .user_data=last->user_data,.action=OS_CONTROL_LEAVE}};
 handle_event(&outside);assert(g_context_hover==FM4_CONTROL_NONE);
 /* 菜单键盘导航循环选行，不得更改下层选择或穿透删除键。 */
 select_name("Alpha.txt");unsigned selected_before_menu=g_selected;
 key(0x4c);assert(g_context_visible&&!g_delete_message.active);
 key(0x52);assert(g_context_hover==FM4_CONTROL_CONTEXT_NEW_FOLDER);
 key(0x51);assert(g_context_hover==FM4_CONTROL_CONTEXT_OPEN);
 key(0x51);assert(g_context_hover==FM4_CONTROL_CONTEXT_DELETE);
 key(0x4d);assert(g_context_hover==FM4_CONTROL_CONTEXT_NEW_FOLDER);
 key(0x4a);assert(g_context_hover==FM4_CONTROL_CONTEXT_OPEN);
 assert(g_context_keyboard&&(g_frame->nodes[g_frame_overlay_index+1U].flags&OS_CONTROL_CHECKED));
 for(unsigned i=0;i<FM4_CONTEXT_ROWS;++i)assert(g_frame->nodes[g_frame_overlay_index+1U+i].flags&OS_CONTROL_NO_HOVER);
 os_window_event_t move=control_event(g_frame_overlay_index+3U,OS_CONTROL_MOVE);handle_event(&move);
 assert(!g_context_keyboard&&g_context_hover==FM4_CONTROL_CONTEXT_PROPERTIES);
 for(unsigned i=0;i<FM4_CONTEXT_ROWS;++i)assert(!(g_frame->nodes[g_frame_overlay_index+1U+i].flags&(OS_CONTROL_CHECKED|OS_CONTROL_NO_HOVER)));
 key(0x4a);assert(g_context_keyboard&&(g_frame->nodes[g_frame_overlay_index+1U].flags&OS_CONTROL_CHECKED));
 assert(g_selection_valid&&g_selected==selected_before_menu);
 key(0x29);assert(!g_context_visible&&g_context_hover==FM4_CONTROL_NONE&&g_selection_valid);
 fm4_show_context(400,100);key(0x4a);key(0x51);key(0x51);key(0x28);
 assert(!g_context_visible&&strstr(g_status,"文件  ")&&g_selected==selected_before_menu);
 fm4_show_context(400,100);key(0x4d);key(0x28);
 assert(!g_context_visible&&g_name_action==1&&g_edit_mode==FM4_EDIT_LOCATION);
 key(0x29);assert(g_edit_mode!=FM4_EDIT_LOCATION&&!g_name_action);
 /* 真正的控件点击执行菜单动作，过期/篡改元数据不能执行；关闭后不再保留菜单节点。 */
 select_name("Alpha.txt");selected_before_menu=g_selected;
 fm4_show_context(400,100);render();
 os_window_event_t menu_click=control_event(g_frame_overlay_index+3U,OS_CONTROL_CLICK),forged=menu_click;
 forged.control.user_data=FM4_CONTROL_CONTEXT_DELETE;
 handle_event(&forged);assert(g_context_visible&&!g_delete_message.active);
 --forged.control.tree_revision;handle_event(&forged);assert(g_context_visible);
 handle_event(&menu_click);assert(!g_context_visible&&g_frame_tree.count==g_frame_overlay_index);
 assert(strstr(g_status,"文件  ")&&g_selected==selected_before_menu);
 handle_event(&menu_click);assert(!g_context_visible);
 os_window_event_t menu_button=control_event(7,OS_CONTROL_CLICK);handle_event(&menu_button);
 assert(g_context_visible&&g_frame_tree.count==g_frame_overlay_index+FM4_MENU_NODE_COUNT);
 menu_button=control_event(7,OS_CONTROL_CLICK);handle_event(&menu_button);
 assert(!g_context_visible&&g_frame_tree.count==g_frame_overlay_index);
 menu_button=control_event(7,OS_CONTROL_CLICK);handle_event(&menu_button);
 os_window_event_t dismiss={.type=OS_WINDOW_EVENT_INPUT,.pointer_x=10,.pointer_y=10,
  .input={.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_LEFT,.value=OS_INPUT_VALUE_PRESS}};
 handle_event(&dismiss);assert(!g_context_visible&&g_selected==selected_before_menu);
 /* 删除确认锁定路径，默认取消，并隔离键盘、右键、滚轮及框外左键。 */
 snprintf(path,sizeof path,"%s/modal-victim",argv[1]);
 int victim=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(victim>=0);assert(!close(victim));
 assert(load_directory());select_name("modal-victim");key(0x4c);
 assert(g_delete_message.active);
 ui_message_document_t modal_document;
 assert(ui_message_build(&g_delete_message,g_window.identifier,g_window.width,g_window.height,&modal_document));
 assert(g_frame_tree.count==g_frame_overlay_index+modal_document.request.count-2U);
 assert((g_frame->nodes[1].flags&OS_CONTROL_INERT)==OS_CONTROL_INERT);
 assert(g_frame->nodes[g_frame_overlay_index].background==0);
 assert(!(g_frame->nodes[10].flags&OS_CONTROL_DISABLED));
 for(unsigned i=0;i<g_frame_tree.count;++i)
  if(g_frame->nodes[i].identifier>=FM4_ITEM_ID_BASE &&
     g_frame->nodes[i].identifier<FM4_ITEM_ID_BASE+g_entry_count)
   assert(!(g_frame->nodes[i].flags&OS_CONTROL_DISABLED));
 os_window_event_t blocked_control=control_event(3,OS_CONTROL_CLICK);handle_event(&blocked_control);
 blocked_control=control_event(10,OS_CONTROL_CLICK);handle_event(&blocked_control);
 assert(g_delete_message.active&&g_edit_mode==FM4_EDIT_NONE);
 unsigned modal_selection=g_selected,modal_scroll=g_first_entry;
 key(0x51);key(0x4d);g_ctrl=true;key(0x0f);key(0x09);g_ctrl=false;
 os_window_event_t blocked={.type=OS_WINDOW_EVENT_INPUT,.pointer_x=720,.pointer_y=110,
  .input={.type=OS_INPUT_EVENT_BUTTON,.code=OS_INPUT_BUTTON_RIGHT,.value=OS_INPUT_VALUE_PRESS}};
 handle_event(&blocked);assert(g_delete_message.active&&!g_context_visible);
 blocked.input.code=OS_INPUT_BUTTON_LEFT;handle_event(&blocked);
 blocked.input.type=OS_INPUT_EVENT_RELATIVE;blocked.input.code=OS_INPUT_REL_WHEEL;blocked.input.value=-1;
 handle_event(&blocked);
 assert(g_selected==modal_selection&&g_first_entry==modal_scroll&&g_edit_mode==FM4_EDIT_NONE);
 key(0x28);assert(!g_delete_message.active&&!remove_calls&&access(path,F_OK)==0);
 remove_selected();select_name("Alpha.txt");assert(load_directory());
 assert(g_delete_message.active&&!strcmp(g_delete_path,path));
 render();key(0x4f);key(0x28);
 assert(!g_delete_message.active&&remove_calls==1&&!strcmp(removed_path,path)&&!g_delete_path[0]);
 assert(access(path,F_OK)!=0&&errno==ENOENT);select_name("Alpha.txt");
 /* 删除错误保留列表及选择，显示具体原因；关闭提示不得重发删除。 */
 const int delete_errors[]={ENOENT,EBUSY,EACCES,EPERM,EROFS,ENOTEMPTY,EIO,ENOMEM,EINVAL};
 const char *delete_reasons[]={"不存在","使用中","权限","权限","只读","不为空","读写失败","内存不足","删除失败"};
 for(unsigned i=0;i<sizeof delete_errors/sizeof *delete_errors;++i){
  unsigned attempts=remove_calls,selection=g_selected,entries=g_entry_count;
  remove_selected();render();
  uint32_t old_generation=g_delete_message.generation;
  remove_failure=-delete_errors[i];
  fm4_complete_delete(OS_MESSAGE_ACCEPT);
  assert(remove_calls==attempts+1&&g_selected==selection&&g_entry_count==entries&&g_selection_valid);
  assert(g_delete_message.active&&g_delete_message.buttons==OS_MESSAGE_OK);
  assert(g_delete_message.generation!=old_generation&&strstr(g_delete_message.message,delete_reasons[i]));
  assert(!strcmp(g_delete_message.title,"无法删除")&&!g_delete_path[0]);
  os_window_event_t ok=control_event(dialog_button(2),OS_CONTROL_CLICK);
  handle_event(&ok);
  assert(!g_delete_message.active&&remove_calls==attempts+1);
  remove_failure=0;
 }
 remove_calls=1;
 key(0x4c);key(0x2b);assert(test_focus==ui_message_target(&g_delete_message,2));
 g_shift=true;key(0x2b);g_shift=false;assert(test_focus==ui_message_target(&g_delete_message,3));
 key(0x29);assert(!g_delete_message.active&&remove_calls==1);
 /* 多字号下按钮焦点只更新公共树，不改写文件 Surface；局部清理与完整重绘一致。 */
 uint32_t *dialog_pixels=malloc(960*640*4);assert(dialog_pixels);
 const unsigned render_sizes[]={16,24,32,48};
 remove_selected();
 for(unsigned i=0;i<sizeof render_sizes/sizeof *render_sizes;++i){
  test_text_size=render_sizes[i];test_focus=ui_message_target(&g_delete_message,3);
  render();
  uint64_t focus_revision=g_frame_tree.revision;
  memcpy(dialog_pixels,g_window.pixels,960*640*4);key(0x4f);
  assert(g_frame_tree.revision==focus_revision&&!memcmp(dialog_pixels,g_window.pixels,960*640*4));
  assert(test_focus==ui_message_target(&g_delete_message,2));
  render();
  memcpy(dialog_pixels,g_window.pixels,960*640*4);render();
  assert(!memcmp(dialog_pixels,g_window.pixels,960*640*4));
 }
 free(dialog_pixels);test_text_size=16;render();
 os_window_event_t cancel_click=control_event(dialog_button(3),OS_CONTROL_CLICK);
 os_window_event_t stale_click=cancel_click;--stale_click.control.tree_revision;
 handle_event(&stale_click);assert(g_delete_message.active);
 handle_event(&cancel_click);assert(!g_delete_message.active&&remove_calls==1);
 render();
 assert(FM4_HEADER_HEIGHT==0);
 uint64_t stable_revision=g_frame_tree.revision;render();assert(g_frame_tree.revision==stable_revision);
 const os_control_node_t *location=g_frame->nodes+3;
 os_window_event_t location_click={.type=OS_WINDOW_EVENT_CONTROL,.control={
  .target=location->identifier,.node_index=3,.node_kind=location->kind,
  .tree_revision=g_frame_tree.revision,.user_data=location->user_data,.action=OS_CONTROL_CLICK}};
 handle_event(&location_click);assert(g_edit_mode==FM4_EDIT_LOCATION&&g_edit_select_all);
 key(0x04);assert(!strcmp(g_location,"a"));
 handle_event(&location_click);assert(!strcmp(g_location,"a"));
 key(0x29);assert(g_edit_mode!=FM4_EDIT_LOCATION);
 /* 大目录、失败回滚以及筛选后的父目录导航均使用生产路径。 */
 unsigned batches_before=batch_calls;
 assert(fm4_navigate("/__fileman_large__",true));
 assert(batch_calls-batches_before==39);
 assert(g_directory_count==1200&&g_entry_count==1200);
 assert(last_malloc_size==2400*sizeof(*g_entries));
 /* 大目录由公共滚动条传递绝对位置，不再维护应用私有滑块。 */
 render();
 unsigned scroll_index=g_frame_overlay_index-6U;
 assert(g_frame->nodes[scroll_index].identifier==40);
 assert(g_frame->nodes[scroll_index].kind==OS_CONTROL_SCROLLBAR);
 assert(!(g_frame->nodes[scroll_index].flags&OS_CONTROL_HIDDEN));
 assert(g_frame->nodes[scroll_index].page_size>0&&g_frame->nodes[scroll_index].page_size<10000);
 os_window_event_t scroll_event=control_event(scroll_index,OS_CONTROL_CHANGE);
 scroll_event.control.value=10000;handle_event(&scroll_event);
 assert(g_first_entry==fm4_max_scroll_row()*fm4_columns());
 scroll_event=control_event(scroll_index,OS_CONTROL_CHANGE);
 scroll_event.control.value=0;handle_event(&scroll_event);assert(!g_first_entry);
 uint64_t scroll_revision=g_frame_tree.revision;
 scroll_event=control_event(scroll_index,OS_CONTROL_CHANGE);
 scroll_event.control.value=-1;handle_event(&scroll_event);assert(!g_first_entry);
 scroll_event.control.value=10001;handle_event(&scroll_event);assert(!g_first_entry);
 scroll_event.control.value=0;handle_event(&scroll_event);
 assert(g_frame_tree.revision==scroll_revision);
 scroll_event=control_event(scroll_index,OS_CONTROL_WHEEL);
 scroll_event.control.value=-1;handle_event(&scroll_event);assert(g_first_entry>0);
 scroll_event=control_event(scroll_index,OS_CONTROL_CHANGE);
 scroll_event.control.value=0;handle_event(&scroll_event);assert(!g_first_entry);
 assert(sizeof(*g_entries)==sizeof(fileman_entry_t *));
 printf("Fileman indexes: %zu bytes for full order plus filtered view; sorting exchanges %zu rather than %zu bytes\n",
        last_malloc_size,sizeof(*g_directory_order),sizeof(fileman_entry_t));
 assert(g_directory_order==g_entries+g_directory_count);
 fileman_entry_t *unchanged=malloc(1200*sizeof(*unchanged));assert(unchanged);
 memcpy(unchanged,g_directory_entries,1200*sizeof(*unchanged));
 for(unsigned direction=0;direction<2;++direction) {
  g_sort_descending=direction;fm4_sort();
  bool seen[1200]={0};
  for(unsigned i=0;i<g_entry_count;++i) {
   assert(g_entries[i]==g_directory_order[i]);
   assert(g_entries[i]>=g_directory_entries&&g_entries[i]<g_directory_entries+1200);
   size_t slot=(size_t)(g_entries[i]-g_directory_entries);assert(!seen[slot]);seen[slot]=true;
   char expected[32];snprintf(expected,sizeof(expected),"item-%04u",direction?1199-i:i);
   assert(!strcmp(g_entries[i]->info.name,expected));
  }
  assert(!memcmp(unchanged,g_directory_entries,1200*sizeof(*unchanged)));
 }
 free(unchanged);
 puts("Fileman sort: 1200 immutable backing records, pointer-only swaps, ascending/descending permutations PASS");
 copy_text(g_query,sizeof g_query,"item-1199");fm4_apply_filter();
 assert(g_entry_count==1);
 g_selection_valid=true;g_selected=0;g_first_entry=0;
 fileman_entry_t **old=g_entries,**old_order=g_directory_order,*raw=g_directory_entries;
 for(malformed_batch=1;malformed_batch<=2;++malformed_batch){
  assert(!load_directory());assert(g_entries==old&&g_directory_order==old_order&&g_directory_entries==raw&&g_entry_count==1);
 }
 malformed_batch=0;
 /* 内核拒绝不完整的大目录快照时，当前视图、选择和滚动位置必须保留。 */
 fail_batch_status=-ENOMEM;assert(!load_directory());fail_batch_status=0;
 assert(g_entries==old&&g_directory_order==old_order&&g_directory_entries==raw&&g_entry_count==1&&g_selection_valid);
 assert(g_selected==0&&g_first_entry==0);
 before=enumerate_calls;
 assert(!fm4_navigate("/__missing_fileman_directory__",true));
 assert(!strcmp(g_path,"/__fileman_large__")&&!strcmp(g_query,"item-1199"));
 assert(g_entries==old&&g_directory_order==old_order&&g_directory_entries==raw&&g_entry_count==1&&g_selection_valid);
 assert(enumerate_calls==before);
 fail_enumeration=17;assert(!load_directory());fail_enumeration=-1;
 assert(g_entries==old&&g_directory_order==old_order&&g_directory_entries==raw&&g_entry_count==1&&g_selection_valid);
 /* 七次容量增长和最终视图分配的每个失败点都必须保留旧列表。 */
 for(int failure=0;failure<8;++failure){
  fail_allocation=failure;assert(!load_directory());fail_allocation=-1;
  assert(g_entries==old&&g_directory_order==old_order&&g_directory_entries==raw&&g_directory_count==1200&&g_entry_count==1);
 }
 copy_text(g_query,sizeof g_query,"no-match");fm4_apply_filter();assert(g_entry_count==0);
 assert(fm4_navigate(argv[1],true));snprintf(path,sizeof path,"%s/renamed",argv[1]);
 assert(fm4_navigate(path,true));copy_text(g_query,sizeof g_query,"Alpha");fm4_apply_filter();
 fm4_parent();assert(!strcmp(g_path,argv[1])&&!g_query[0]&&g_entry_count==3);
 select_name("Alpha.txt");fm4_copy_selection(false);
 snprintf(path,sizeof path,"%s/async",argv[1]);assert(!mkdir(path,0755));
 assert(fm4_navigate(path,true));atomic_store(&copy_gate,true);
 assert(fm4_paste());
 for(unsigned i=0;!atomic_load(&copy_entered)&&i<5000;++i)usleep(1000);
 assert(atomic_load(&copy_entered)&&g_copy_active&&!atomic_load(&g_copy_finished));
 assert(fm4_navigate(argv[1],true));
 g_ctrl=true;key(0x09);g_ctrl=false;key(0x04);assert(!strcmp(g_query,"a"));
 fm4_show_context(400,100);key(0x29);
 assert(!g_context_visible&&!atomic_load(&g_copy_context.cancel));
 select_name("Alpha.txt");remove_selected();render();key(0x29);
 assert(!g_delete_message.active&&!atomic_load(&g_copy_context.cancel));
 key(0x29);assert(atomic_load(&g_copy_context.cancel));
 atomic_store(&copy_release,true);
 for(unsigned i=0;g_copy_active&&i<5000;++i){fm4_copy_poll();usleep(1000);}
 assert(!g_copy_active&&g_copy_error==ECANCELED&&!strcmp(g_path,argv[1]));
 assert(atomic_load(&g_copy_context.bytes)==0);
 free(g_entries);free(g_directory_entries);
 assert(!munmap(g_window.pixels,960*640*4));puts("Fileman: real operations, 1200-entry snapshot, eight allocation failures, I/O rollback, no-I/O filtering, UTF-8 labels and native heap sort PASS");
}
'''
required = 'OS_WINDOW_VISIBLE|OS_WINDOW_RESIZABLE|OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS'
checks = f'unsigned required={required};\n'
for mask in (syscall_flags, server_flags):
    checks += f'assert((required & ~({mask}))==0U);assert((0x80000000U & ~({mask}))!=0U);\n'
program = program.replace('/* 标志校验 */', checks)
program = program.replace('/* 自有排序 */', sort)
with tempfile.TemporaryDirectory(prefix='fileman-navigation-') as directory:
    path = Path(directory)
    source = path / 'test.c'
    source.write_text(program)
    binary = path / 'test'
    fixture = path / 'files'
    fixture.mkdir()
    (fixture / 'Alpha.txt').write_text('copy-content')
    (fixture / 'beta.txt').write_text('move-content')
    (fixture / 'zdir').mkdir()
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root / 'user/desktop/fileman'),
                    '-pthread', str(source),str(root/'kernel/graphics/control_tree.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(fixture)], check=True)
    assert (fixture / 'renamed/Alpha.txt').read_text() == 'copy-content'
    assert (fixture / 'renamed/beta.txt').read_text() == 'move-content'
    assert not (fixture / 'beta.txt').exists()
    # 负向注入：任何重新写回窗口像素的代码必须被只读映射捕获。
    source.write_text(program.replace('static void render(void) {',
        'static void render(void) { if(g_window.pixels) g_window.pixels[0] ^= 1U;',1))
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-I' + str(root / 'user/desktop/fileman'),
                    '-pthread', str(source),str(root/'kernel/graphics/control_tree.c'), '-o', str(binary)], check=True)
    negative_fixture = path / 'negative-files'
    negative_fixture.mkdir()
    (negative_fixture / 'Alpha.txt').write_text('copy-content')
    (negative_fixture / 'beta.txt').write_text('move-content')
    (negative_fixture / 'zdir').mkdir()
    result = subprocess.run([str(binary), str(negative_fixture)], capture_output=True, text=True)
    assert result.returncode != 0 and 'WRITE memory access' in result.stderr, result.stderr
    print('Fileman negative control: application pixel writes rejected by read-only mapping PASS')
