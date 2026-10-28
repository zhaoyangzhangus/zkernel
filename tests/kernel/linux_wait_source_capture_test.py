"""Execute complete production poll/epoll source capture and release helpers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
poll = source[source.index('typedef struct linux_poll_wait_set {'):
              source.index('static bool linux_poll_wait_ready(')]
epoll = source[source.index('static bool linux_epoll_watch_has_wait_source('):
               source.index('static bool linux_epoll_wait_ready(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LINUX_POLL_WAIT_SOURCES 4U
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
#define K_ENOSPC -28
typedef int kstatus_t;
enum {LINUX_FD_NVIDIA,LINUX_FD_PIPE,LINUX_FD_SOCKET,LINUX_FD_UNIX_SOCKET,
      LINUX_FD_WINDOW,LINUX_FD_EVENTFD,LINUX_FD_EPOLL,LINUX_FD_FILE};
typedef struct {unsigned magic,refs,gets,puts;} object_t;
typedef struct {bool held;} spinlock_t;
typedef struct {object_t header;spinlock_t lock;void *read_end,*write_end;} linux_unix_socket_t;
typedef struct {unsigned non_object_magic;void *wait_object;} frontend_t;
typedef struct {unsigned kind;void *private_data;} linux_fd_entry_t;
typedef struct {int16_t events,revents;} linux_pollfd_t;
typedef struct {bool used;unsigned kind,events;void *private_data;bool disabled;} linux_epoll_watch_t;
typedef struct {spinlock_t lock;uint64_t control_sequence;unsigned watch_limit;linux_epoll_watch_t watches[8];} linux_epoll_object_t;
static unsigned held;
static void spinlock_lock(spinlock_t *l){assert(!l->held);l->held=true;++held;}
static void spinlock_unlock(spinlock_t *l){assert(l->held&&held);l->held=false;--held;}
static bool object_try_get(void *p){object_t *o=p;assert(o&&o->magic==5070);if(!o->refs)return false;++o->refs;++o->gets;return true;}
static void object_put(void *p){object_t *o=p;assert(!held&&o->magic==5070&&o->refs>1);--o->refs;++o->puts;}
static bool linux_poll_read_requested(int16_t bits){return bits&1;}
static bool linux_poll_write_requested(int16_t bits){return bits&4;}
static void *nv_frontend_file_wait_object(void *p){frontend_t *f=p;assert(f->non_object_magic==9999);return f->wait_object;}
''' + poll + epoll + r'''
int main(void) {
 object_t source={.magic=5070,.refs=1},read={.magic=5070,.refs=1},write={.magic=5070,.refs=1};
 frontend_t a={9999,&source},b={9999,&source};
 linux_pollfd_t fds[8];linux_fd_entry_t entries[8];
 for(unsigned i=0;i<8;++i){fds[i]=(linux_pollfd_t){.events=1};entries[i]=(linux_fd_entry_t){LINUX_FD_NVIDIA,i%2?(void*)&a:(void*)&b};}
 void *poll_objects[4];uint8_t poll_owned[4];
 linux_poll_wait_set_t set={.objects=poll_objects,.owned=poll_owned,.capacity=4};
 assert(!linux_poll_capture_wait_set(fds,entries,8,&set));
 assert(set.count==1&&set.objects[0]==&source&&!set.owned[0]&&!source.gets);
 linux_poll_wait_set_release(&set);assert(!set.count&&!source.puts);
 linux_unix_socket_t socket={.header={.magic=5070,.refs=1},.read_end=&read,.write_end=&write};
 entries[0]=(linux_fd_entry_t){LINUX_FD_PIPE,&read};
 entries[1]=entries[2]=(linux_fd_entry_t){LINUX_FD_UNIX_SOCKET,&socket};
 assert(!linux_poll_capture_wait_set(fds,entries,3,&set));
 assert(set.count==3&&set.objects[0]==&read&&set.owned[0]);
 assert(set.objects[1]==&socket&&!set.owned[1]&&set.objects[2]==&write&&set.owned[2]);
 assert(read.refs==2&&write.refs==2&&read.gets==1&&write.gets==1);
 linux_poll_wait_set_release(&set);assert(read.refs==1&&write.refs==1&&!held);
 /* Failure after upgrading an already-borrowed endpoint unwinds once. */
 write.refs=0;
 assert(linux_poll_capture_wait_set(fds,entries,3,&set)==K_ENOENT&&!set.count);
 assert(read.refs==1&&read.gets==read.puts&&!socket.lock.held);
 write.refs=1;
 object_t unique[5];
 for(unsigned i=0;i<5;++i){unique[i]=(object_t){.magic=5070,.refs=1};entries[i]=(linux_fd_entry_t){LINUX_FD_PIPE,&unique[i]};}
 assert(linux_poll_capture_wait_set(fds,entries,5,&set)==K_ENOSPC&&!set.count);
 for(unsigned i=0;i<5;++i)assert(unique[i].refs==1&&!unique[i].puts);
 set.count=0;
 for(unsigned i=0;i<4;++i)assert(!linux_poll_wait_set_add(&set,&unique[i],false));
 assert(!linux_poll_wait_set_add(&set,&unique[0],true)&&set.count==4&&set.owned[0]&&unique[0].refs==2);
 assert(!linux_poll_wait_set_add(&set,&unique[0],true)&&unique[0].gets==1);
 linux_poll_wait_set_release(&set);assert(unique[0].refs==1);
 /* epoll must pin the actual wait object, not the opaque NVIDIA frontend. */
 linux_epoll_object_t ep={.control_sequence=123,.watch_limit=3};
 ep.watches[0]=(linux_epoll_watch_t){true,LINUX_FD_NVIDIA,1,&a,false};
 ep.watches[1]=(linux_epoll_watch_t){true,LINUX_FD_NVIDIA,1,&b,false};
 ep.watches[2]=(linux_epoll_watch_t){false,LINUX_FD_NVIDIA,1,&a,false};
 void *objects[8];size_t count=0;uint64_t sequence=0;
 assert(!linux_epoll_capture_wait_set(&ep,objects,8,&count,&sequence));
 assert(sequence==123&&count==2&&objects[0]==&ep&&objects[1]==&source&&source.refs==2&&source.gets==1);
 linux_epoll_release_wait_set(objects,count);assert(source.refs==1&&source.puts==1);
 /* Zero masks must subscribe for ERR/HUP; disarmed ONESHOT must not. */
 ep.watches[0].events=0;ep.watches[1].disabled=true;
 assert(!linux_epoll_capture_wait_set(&ep,objects,8,&count,&sequence)&&count==2);
 assert(objects[1]==&source&&source.refs==2);
 linux_epoll_release_wait_set(objects,count);
 ep.watches[0].disabled=true;
 assert(!linux_epoll_capture_wait_set(&ep,objects,8,&count,&sequence)&&count==1);
 linux_epoll_release_wait_set(objects,count);assert(source.refs==1);
 ep.watches[0].events=1;ep.watches[0].disabled=false;ep.watches[1].disabled=false;
 ep.watches[2]=(linux_epoll_watch_t){true,LINUX_FD_UNIX_SOCKET,1,&socket,false};
 assert(linux_epoll_capture_wait_set(&ep,objects,3,&count,&sequence)==K_ENOSPC);
 assert(source.refs==1&&socket.header.refs==1&&read.refs==1&&write.refs==1&&!held);
 write.refs=0;
 assert(linux_epoll_capture_wait_set(&ep,objects,8,&count,&sequence)==K_ENOENT);
 assert(source.refs==1&&socket.header.refs==1&&read.refs==1&&!held);
 write.refs=1;
 assert(!linux_epoll_capture_wait_set(&ep,objects,8,&count,&sequence)&&count==5);
 linux_epoll_release_wait_set(objects,count);
 assert(source.refs==1&&socket.header.refs==1&&read.refs==1&&write.refs==1);
 assert(source.gets==source.puts&&socket.header.gets==socket.header.puts&&read.gets==read.puts&&write.gets==write.puts);
 assert(linux_poll_wait_set_add(NULL,&source,false)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-wait-capture-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Wait-source capture: complete poll/epoll helpers, NVIDIA alias dedup, borrowed-to-owned promotion, Unix endpoint pins and failure unwind passed')
