"""验证真实应用身份识别只在进程锁内复制名称。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/launcher.c').read_text()
start = source.index('static bool desktop_process_name_matches(')
match = source[start:source.index('static bool desktop_launch_worker_ready(', start)]
start = source.index('uint32_t desktop_window_app_locked(')
identify = source[start:source.index('bool desktop_restore_minimized_app(', start)]
match = match.replace('    uint32_t index = 0U;', '    assert(!held);\n    uint32_t index = 0U;')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define DESKTOP_APP_NONE 0U
#define DESKTOP_APP_CHROME 6U
#define DESKTOP_APP_WALLPAPER 8U
#define DESKTOP_APP_SLOTS 8U
typedef struct {unsigned info_lock; char name[32];} process_t;
typedef struct {process_t *owner;} window_server_window_t;
static struct {char path[256];} g_desktop_programs[8];
static unsigned held,copies,locks;
static bool available=true,change_on_unlock;
static process_t owner;
static bool desktop_shell_assets_available(void){return available;}
static void spinlock_lock(unsigned *lock){assert(lock==&owner.info_lock&&!held);held=1;++locks;}
static void spinlock_unlock(unsigned *lock){
 assert(lock==&owner.info_lock&&held);held=0;
 if(change_on_unlock)memset(owner.name,'x',sizeof(owner.name));
}
static void *snapshot_copy(void *out,const void *in,size_t bytes){
 assert(held&&in==owner.name&&bytes==sizeof(owner.name));++copies;
 return memcpy(out,in,bytes);
}
#define memcpy snapshot_copy
''' + match + identify + r'''
#undef memcpy
int main(void){
 const char *names[]={"", "files", "shell", "notes", "settings", "tasks", "chrome", "images", "wallpaper"};
 for(unsigned i=1;i<8;++i)snprintf(g_desktop_programs[i].path,256,"/usr/apps/a/%s",i==6?"chrome-start":names[i]);
 window_server_window_t window={&owner},no_owner={0};
 assert(!desktop_window_app_locked(0)&&!desktop_window_app_locked(&no_owner)&&!locks);
 available=false;assert(!desktop_window_app_locked(&window)&&!locks);available=true;
 for(unsigned i=0;i<40000;++i){
  unsigned app=i%9;memset(owner.name,0,sizeof(owner.name));strcpy(owner.name,names[app]);
  change_on_unlock=(i&1)!=0;
  assert(desktop_window_app_locked(&window)==app);
  assert(!held&&copies==i+1&&locks==i+1);
 }
 change_on_unlock=false;
 memset(owner.name,'x',sizeof(owner.name));assert(!desktop_window_app_locked(&window));
 strcpy(owner.name,"chrome-child");assert(!desktop_window_app_locked(&window));
 strcpy(owner.name,"Chrome");assert(!desktop_window_app_locked(&window));
 for(unsigned i=1;i<8;++i)g_desktop_programs[i].path[0]=0;
 strcpy(owner.name,"wallpaper");assert(desktop_window_app_locked(&window)==8);
 assert(!held);
 puts("App identity: 40000 mappings and post-unlock name changes, absent owner/assets, full name and exact matching PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='app-identity-') as directory:
    binary = str(Path(directory) / 'test')
    def compile_run(text):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary], input=text, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True)
    result = compile_run(program)
    assert result.returncode == 0, result.stderr
    print(result.stdout, end='')
    broken = program.replace('desktop_process_name_matches(name,',
                             'desktop_process_name_matches(owner->name,')
    result = compile_run(broken)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('反例被拒绝：解锁后重新读取进程名，破坏同一次识别的快照')
