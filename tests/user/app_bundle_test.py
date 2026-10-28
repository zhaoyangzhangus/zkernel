"""执行真实桌面图标加载和终端应用路径解析，核对应用包不依赖旧目录。"""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
io = (root/'kernel/fs/vfs/file_io.c').read_text()
file_loader = io[io.index('kstatus_t vfs_load_file_kernel('):io.index('/* 整次写入')]
assets = (root/'kernel/graphics/assets.c').read_text()
loader = assets[assets.index('static bool desktop_load_ui(void)'):assets.index('static bool desktop_wallpaper_request_ready(')]
shell = (root/'user/desktop/gshell/main.c').read_text()
resolver = shell[shell.index('static bool make_program_path('):shell.index('static bool append_text_chunk(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "user/runtime/control_file.h"
#include <kernel/control_tree.h>
#define SHELL_LINE_CAPACITY 256
enum { DESKTOP_APP_NONE, DESKTOP_APP_FILES, DESKTOP_APP_TERMINAL,
 DESKTOP_APP_NOTES, DESKTOP_APP_SETTINGS, DESKTOP_APP_TASKMGR, DESKTOP_APP_CHROME, DESKTOP_APP_IMAGEVIEW };
typedef struct {void *storage;unsigned loaded;} desktop_asset_image_t;
static desktop_asset_image_t g_desktop_wallpaper_asset,g_desktop_app_assets[8];
typedef struct {char path[256],icon[256],title[32];uint32_t node_index;} desktop_app_binding_t;
static desktop_app_binding_t g_desktop_programs[8];
static ui_file_document_t g_desktop_document;
static struct {uint32_t display_width,display_height;struct {const os_control_node_t *controls;uint32_t control_count;} scene;} g_window_server={.display_width=1920,.display_height=1080};
typedef int kstatus_t;
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EIO -5
#define VFS_OPEN_READ 1
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
static vnode_t vnode;
static file_t fixture_file={&vnode};
static void vfs_vnode_lock(vnode_t *v){assert(v==&vnode);}
static void vfs_vnode_unlock(vnode_t *v){assert(v==&vnode);}
static char *ui_source;
static unsigned bad_read;
static int vfs_open_kernel(const char *p,unsigned flags,unsigned mode,file_t **f){
 assert(!strcmp(p,"/usr/apps/desktop/desktop.ui")&&flags==1&&!mode);*f=&fixture_file;return 0;
}
static void vfs_close(file_t *f){assert(f==&fixture_file);}
static int vfs_read_kernel(file_t *f,void *dst,size_t n,uint64_t *read){
 assert(f==&fixture_file&&n==vnode.size);*read=n-bad_read;memcpy(dst,ui_source,*read);return 0;
}
static void *kmalloc(size_t n,unsigned flags){assert(!flags);return malloc(n);}
static void kfree(void *p){free(p);}
static unsigned loads,fail_at;
static const char *expected[]={"/usr/apps/desktop/wall.png","/usr/apps/fileman/icon.png",
 "/usr/apps/gshell/icon.png","/usr/apps/notepad/icon.png","/usr/apps/settings/icon.png",
 "/usr/apps/taskmgr/icon.png","/usr/apps/chrome/icon.png","/usr/apps/imageview/icon.png"};
static bool desktop_load_png_asset(const char *path,desktop_asset_image_t *asset){
 assert(g_desktop_document.nodes&&g_window_server.scene.controls==g_desktop_document.nodes);
 assert(loads<8&&!strcmp(path,expected[loads]));
 ++loads;if(loads==fail_at)return false;
 assert(!asset->storage);asset->storage=malloc(16);assert(asset->storage);
 asset->loaded=loads;return true;
}
static const char *existing;
static bool file_exists(const char *s){
 char absolute[256];
 if(*s=='/')return !strcmp(s,existing);
 snprintf(absolute,sizeof(absolute),"/tmp/%s",s);
 return !strcmp(absolute,existing);
}
static void copy_text(char *dst,const char *s){strcpy(dst,s);}
static void append_text(char *dst,unsigned capacity,const char *s){
 unsigned n=strlen(dst);snprintf(dst+n,capacity-n,"%s",s);
}
static void append_character(char *dst,unsigned capacity,char c){
 unsigned n=strlen(dst);if(n+1<capacity){dst[n]=c;dst[n+1]=0;}
}
static char lower_ascii(char c){return (char)tolower((unsigned char)c);}
static bool text_equals_ignore_case(const char *a,const char *b){
 while(*a&&*b&&lower_ascii(*a)==lower_ascii(*b)){++a;++b;}return *a==*b;
}
''' + file_loader + loader + resolver + r'''
int main(int argc,char **argv){
 assert(argc==2);FILE *input=fopen(argv[1],"rb");assert(input);
 assert(!fseek(input,0,SEEK_END));long size=ftell(input);assert(size>0);
 rewind(input);ui_source=calloc((size_t)size+1,1);assert(ui_source);
 assert(fread(ui_source,1,(size_t)size,input)==(size_t)size);fclose(input);vnode.size=(uint64_t)size;
 for(fail_at=0;fail_at<=8;++fail_at){loads=0;desktop_discard_assets();
  assert(desktop_load_assets()==(fail_at==0));assert(loads==(fail_at?fail_at:8));
  if(!fail_at) {
   assert(g_desktop_wallpaper_asset.loaded==1);
   for(unsigned app=1;app<=7;++app)assert(g_desktop_app_assets[app].loaded==app+1);
  } else {
   assert(!g_desktop_wallpaper_asset.storage&&!g_desktop_document.nodes&&!g_window_server.scene.controls&&!g_window_server.scene.control_count);
   for(unsigned app=1;app<=7;++app)assert(!g_desktop_app_assets[app].storage&&!g_desktop_programs[app].path[0]);
  }
 }
 loads=fail_at=0;assert(desktop_load_assets());
 /* 已有完整文档时拒绝重复加载，但不能释放仍在使用的资源。 */
 unsigned old_loads=loads;void *old_wallpaper=g_desktop_wallpaper_asset.storage;
 const os_control_node_t *old_nodes=g_window_server.scene.controls;
 assert(!desktop_load_assets()&&loads==old_loads&&g_desktop_wallpaper_asset.storage==old_wallpaper);
 assert(g_window_server.scene.controls==old_nodes&&g_desktop_document.nodes==old_nodes);
 assert(!strcmp(g_desktop_programs[1].path,"/usr/apps/fileman/fileman"));
 assert(!strcmp(g_desktop_programs[6].path,"/usr/apps/chrome/chrome-start"));
 char *directory=strstr(ui_source,"/usr/apps/fileman");assert(directory);
 assert(g_window_server.scene.controls==g_desktop_document.nodes&&g_window_server.scene.control_count==9);
 assert(!desktop_load_ui());ui_file_destroy(&g_desktop_document);
 memcpy(directory,"/usr/apps/customx",17);assert(desktop_load_ui());
 assert(!strcmp(g_desktop_programs[1].path,"/usr/apps/customx/fileman"));
 assert(!strcmp(g_desktop_programs[1].icon,"/usr/apps/customx/icon.png"));
 desktop_app_binding_t before[8];memcpy(before,g_desktop_programs,sizeof(before));
 ui_file_destroy(&g_desktop_document);
 memcpy(directory,"/usr/apps/../../x",17);assert(!desktop_load_ui());
 assert(!memcmp(before,g_desktop_programs,sizeof(before)));
 bad_read=1;assert(!desktop_load_ui());
 assert(!memcmp(before,g_desktop_programs,sizeof(before)));
 /* 冷启动的界面读取失败时不得开始任何图片 I/O。 */
 desktop_discard_assets();old_loads=loads;
 assert(!desktop_load_assets()&&loads==old_loads&&!g_desktop_document.nodes&&!g_desktop_wallpaper_asset.storage);
 desktop_discard_assets();desktop_discard_assets();free(ui_source);
 const char *commands[]={"FILEMAN","GsHell","notepad","settings","taskmgr","chrome","cube","ui-file-demo","imageview",
 "nasm","NDISASM","wget","libc-test","linux-events-test","fat-fsync-probe",
 "taskmgr-cpu-probe","tcp-wake-probe","chrome-glibc-probe","vulkan-probe","vulkan-video"};
 const char *targets[]={"/usr/apps/fileman/fileman","/usr/apps/gshell/gshell",
  "/usr/apps/notepad/notepad","/usr/apps/settings/settings","/usr/apps/taskmgr/taskmgr",
  "/usr/apps/chrome/chrome-start","/usr/apps/cube/cube","/usr/apps/ui-file-demo/ui-file-demo","/usr/apps/imageview/imageview",
  "/usr/apps/nasm/nasm","/usr/apps/ndisasm/ndisasm","/usr/apps/wget/wget",
  "/usr/apps/libc-test/libc-test","/usr/apps/linux-events-test/linux-events-test",
  "/usr/apps/fat-fsync-probe/fat-fsync-probe","/usr/apps/taskmgr-cpu-probe/taskmgr-cpu-probe",
  "/usr/apps/tcp-wake-probe/tcp-wake-probe","/usr/apps/chrome-glibc-probe/chrome-glibc-probe",
  "/usr/apps/vulkan-probe/vulkan-probe","/usr/apps/vulkan-video/vulkan-video"};
 char path[256];
 for(unsigned i=0;i<sizeof(commands)/sizeof(*commands);++i){existing=targets[i];assert(make_program_path(commands[i],path));assert(!strcmp(path,existing));}
 const char *services[]={"audiod","netd","deviced","logd","crashd"};
 char service_path[256];
 for(unsigned i=0;i<sizeof(services)/sizeof(*services);++i){
  snprintf(service_path,sizeof(service_path),"/sbin/%s",services[i]);existing=service_path;
  assert(make_program_path(services[i],path)&&!strcmp(path,existing));
 }
 existing="/tmp/chrome";assert(make_program_path("chrome",path)&&!strcmp(path,"chrome"));
 assert(make_program_path("./chrome",path)&&!strcmp(path,"./chrome"));
 assert(make_program_path("link/../chrome",path)&&!strcmp(path,"link/../chrome"));
 existing="/usr/apps/fileman/fileman";assert(make_program_path(existing,path)&&!strcmp(path,existing));
 existing="";assert(!make_program_path("missing",path));
 assert(!make_program_path(0,path)&&!make_program_path("",path)&&!make_program_path("x",0));
 puts("Application bundles: actual icon loader and command resolver; all icons, partial failure, names, launcher and system services PASS");
}
'''
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with tempfile.TemporaryDirectory(prefix='app-bundle-test-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(code)
    command=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-O2',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),'-I'+str(root/'include'),str(work/'test.c'),
                    str(root/'user/runtime/control_file.c'),str(root/'kernel/graphics/control_tree.c'),
                    '-o',str(work/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(work/'test'),str(root/'user/ui/desktop.ui')],check=True)
    # 清理分支缺失时，即使成功启动路径不变，也必须被失败注入拒绝。
    cleanup='if(!complete)desktop_discard_assets();'
    assert cleanup in code
    (work/'test.c').write_text(code.replace(cleanup,''))
    subprocess.run(command,check=True)
    rejected=subprocess.run([str(work/'test'),str(root/'user/ui/desktop.ui')],
                            capture_output=True,timeout=30)
    assert rejected.returncode and b'g_desktop_document.nodes' in rejected.stderr,rejected.stderr
    print('Negative control rejected: retaining unpublished desktop resources after failure')
