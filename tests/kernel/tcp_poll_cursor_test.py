"""Actual TCP polling cursor: lock count, sparse registries and pinned lifetime."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket_transport.c').read_text()
start = source.index('static socket_t *socket_tcp_poll_next(')
end = source.index('\n/* 每次处理一个已固定的 socket', start)
helper = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SOCKET_BINDING_COUNT 64U
typedef struct {unsigned refs,id;bool can_pin;} socket_t;
typedef struct {socket_t *socket;} socket_tcp_connection_t;
typedef struct {socket_t *socket;} socket_tcp6_connection_t;
static socket_tcp_connection_t table4[128];static socket_tcp6_connection_t table6[128];
static socket_t objects[256];static bool missing[2][128];
static uint32_t capacity=64;
static uint32_t pass_limit;
static unsigned guard,locks,locked,visits[2],gets,releases;
static unsigned *socket_binding_lock(void){return &guard;}
uint32_t socket_registry_capacity_locked(void){assert(locked);return capacity;}
static void socket_lock(unsigned *lock){assert(lock==&guard && !locked);locked=1;++locks;}
static void socket_unlock(unsigned *lock){assert(lock==&guard && locked);locked=0;}
static const socket_tcp_connection_t *socket_tcp_connection_at(uint32_t i){
 assert(locked && i<capacity);++visits[0];return missing[0][i]?0:&table4[i];
}
static const socket_tcp6_connection_t *socket_tcp6_connection_at(uint32_t i){
 assert(locked && i<capacity);++visits[1];return missing[1][i]?0:&table6[i];
}
static bool object_try_get(socket_t *s){
 assert(locked && s && s->refs==0);++gets;
 if(!s->can_pin)return false;
 ++s->refs;return true;
}
static void object_put(socket_t *s){assert(!locked && s->refs==1);--s->refs;++releases;}
'''
suffix = r'''
static void reset(void){
 memset(table4,0,sizeof(table4));memset(table6,0,sizeof(table6));memset(missing,0,sizeof(missing));
 memset(objects,0,sizeof(objects));locks=locked=gets=releases=visits[0]=visits[1]=0;capacity=64;pass_limit=0;
 for(unsigned i=0;i<256;++i){objects[i].id=i;objects[i].can_pin=true;}
}
static void check(unsigned family){
 unsigned expected[64],count=0,attempts=0;
 for(unsigned i=0;i<64;++i){
  socket_t *s=family?table6[i].socket:table4[i].socket;
  if(s && !missing[family][i]){++attempts;if(s->can_pin)expected[count++]=s->id;}
 }
 unsigned before=locks,before_gets=gets,before_puts=releases;
 uint32_t cursor=0;unsigned found=0;
 while(cursor<64){
  socket_t *s=socket_tcp_poll_next(&cursor,family);
  if(!s)break;
  assert(found<count && s->id==expected[found++] && s->refs==1 && !locked);
  object_put(s);
 }
 assert(cursor==64 && found==count && !locked);
 assert(gets-before_gets==attempts && releases-before_puts==count);
 unsigned terminal=(count==0 || (expected[count-1]%64)!=63)?1:0;
 assert(locks-before==count+terminal);
 assert(visits[family]==64);
}
int main(void){
 uint32_t seed=43;unsigned cases=0;
 for(unsigned pattern=0;pattern<10004;++pattern){
  reset();
  for(unsigned family=0;family<2;++family)for(unsigned i=0;i<64;++i){
   seed=seed*1664525U+1013904223U;
   bool present=pattern==0?false:pattern==1?true:pattern==2?i==63:pattern==3?i==0:(seed&7)<3;
   socket_t *s=&objects[family*64+i];
   if(pattern>=4){s->can_pin=(seed&31)!=0;missing[family][i]=(seed&63)==1;}
   if(family)table6[i].socket=present?s:0;else table4[i].socket=present?s:0;
  }
  check(0);check(1);cases+=2;
  if(pattern==0)assert(locks==2 && !gets);
 }
 // A held reference survives unregister/close; a lower-slot replacement waits
 // for the next pass, and deleting a later slot cannot leave a stale pointer.
 reset();table4[2].socket=&objects[2];table4[63].socket=&objects[63];
 uint32_t cursor=0;socket_t *p=socket_tcp_poll_next(&cursor,false);
 assert(p==&objects[2] && p->refs==1 && cursor==3);
 table4[2].socket=0;p->can_pin=false;table4[63].socket=0;
 table4[0].socket=&objects[0];object_put(p);
 assert(!socket_tcp_poll_next(&cursor,false) && cursor==64);
 cursor=0;p=socket_tcp_poll_next(&cursor,false);assert(p==&objects[0]);object_put(p);
 assert(!socket_tcp_poll_next(&cursor,false));
 cursor=UINT32_MAX;assert(!socket_tcp_poll_next(&cursor,true) && cursor==UINT32_MAX);
 // Growth cannot keep extending an in-progress timer pass. New high slots
 // wait for the next pass; no pointer into the old backing is retained.
 reset();table4[2].socket=&objects[2];cursor=0;
 p=socket_tcp_poll_next(&cursor,false);assert(p==&objects[2]);object_put(p);
 capacity=128;table4[127].socket=&objects[127];
 assert(!socket_tcp_poll_next(&cursor,false) && cursor==64);
 cursor=0;pass_limit=0;
 p=socket_tcp_poll_next(&cursor,false);assert(p==&objects[2]);object_put(p);
 p=socket_tcp_poll_next(&cursor,false);assert(p==&objects[127] && cursor==128);object_put(p);
 capacity=64;assert(!socket_tcp_poll_next(&cursor,false) && cursor==128);
 capacity=128;table6[100].socket=&objects[228];cursor=0;
 p=socket_tcp_poll_next(&cursor,true);assert(p==&objects[228] && cursor==101);object_put(p);
 assert(!socket_tcp_poll_next(&cursor,true) && cursor==128 && gets==releases);
 // IPv6 uses the horizon captured by IPv4, not a second capacity snapshot.
 reset();table4[63].socket=&objects[63];cursor=0;
 p=socket_tcp_poll_next(&cursor,false);assert(p==&objects[63] && pass_limit==64);object_put(p);
 capacity=128;table6[31].socket=&objects[159];table6[100].socket=&objects[228];cursor=0;
 p=socket_tcp_poll_next(&cursor,true);assert(p==&objects[159]);object_put(p);
 assert(!socket_tcp_poll_next(&cursor,true) && cursor==64 && pass_limit==64);
 pass_limit=0;cursor=32;
 p=socket_tcp_poll_next(&cursor,true);assert(p==&objects[228] && pass_limit==128);object_put(p);
 assert(gets==releases);
 printf("TCP poll cursor: %u sparse/full/final-slot/refusal cases; fixed cross-family pass horizon, next-pass growth, shrink bounds, empty registry 128->2 guard acquisitions and balanced references PASS\n",cases);
}
'''
old_helper = '''static socket_t *socket_tcp_poll_next(uint32_t *cursor, bool ipv6, uint32_t *pass_limit) {
    (void)pass_limit;
    while (*cursor < SOCKET_BINDING_COUNT) {
        socket_lock(socket_binding_lock());
        uint32_t index = (*cursor)++;
        socket_t *target;
        if (ipv6) {
            const socket_tcp6_connection_t *c = socket_tcp6_connection_at(index);
            target = c ? c->socket : 0;
        } else {
            const socket_tcp_connection_t *c = socket_tcp_connection_at(index);
            target = c ? c->socket : 0;
        }
        if (target && !object_try_get(target)) target = 0;
        socket_unlock(socket_binding_lock());
        if (target) return target;
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-poll-cursor-') as directory:
    path = Path(directory)
    unbounded=helper.replace('uint32_t limit = capacity < *pass_limit ? capacity : *pass_limit;',
                             'uint32_t limit = capacity;')
    assert unbounded!=helper
    for name,candidate in [('production',helper),('lock_churn',old_helper),('unbounded',unbounded)]:
        wrapper='static socket_t *next_socket(uint32_t *cursor, bool ipv6) { return socket_tcp_poll_next(cursor, ipv6, &pass_limit); }\n'
        (path/'test.c').write_text(prefix+candidate+wrapper+
                                  suffix.replace('socket_tcp_poll_next(', 'next_socket('))
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=30)
        if name=='lock_churn':
            assert result.returncode != 0, 'Per-empty-slot locking must fail cost bound'
            assert 'locks-before==count+terminal' in result.stderr,result.stderr
            print('Negative control rejected: old empty-slot lock churn')
        elif name=='unbounded':
            assert result.returncode!=0 and 'cursor==64' in result.stderr,result.stderr
            print('Negative control rejected: concurrent growth extends an active polling pass')
        else:
            assert result.returncode == 0,result.stderr
            print(result.stdout,end='')
