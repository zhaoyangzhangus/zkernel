"""Actual TCP slot queries: independent count oracle and bounded visits."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
helpers = source[source.index('bool socket_tcp_slots_available_locked('):
                 source.index('int32_t socket_find_tcp6_locked(')]
assert helpers.count('index < g_registry_capacity') == 2
helpers = helpers.replace('index < g_registry_capacity',
                          'index < g_registry_capacity && (++visits, 1)')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
static socket_tcp_connection_t g_tcp_connections[SOCKET_BINDING_COUNT];
static socket_tcp6_connection_t g_tcp6_connections[SOCKET_BINDING_COUNT];
static unsigned visits;
static uint32_t g_registry_capacity = SOCKET_BINDING_COUNT;
''' + helpers + r'''
static unsigned cases;
static void check(uint32_t required) {
    unsigned free_slots=0, expected_visits=0;
    for (unsigned i=0;i<SOCKET_BINDING_COUNT;++i) {
        assert(g_tcp_connections[i].socket==g_tcp6_connections[i].socket);
        if (!g_tcp_connections[i].socket) ++free_slots;
    }
    if (required && required<=SOCKET_BINDING_COUNT) {
        unsigned found=0;
        for (unsigned i=0;i<SOCKET_BINDING_COUNT;++i) {
            ++expected_visits;
            if (!g_tcp_connections[i].socket && ++found==required) break;
        }
    }
    visits=0;
    assert(socket_tcp_slots_available_locked(required)==(free_slots>=required));
    assert(visits==expected_visits);
    visits=0;
    assert(socket_tcp6_slots_available_locked(required)==(free_slots>=required));
    assert(visits==expected_visits);
    ++cases;
}
int main(void) {
    socket_t occupied={0}; uint32_t seed=17;
    for (unsigned pattern=0;pattern<10004;++pattern) {
        for (unsigned i=0;i<SOCKET_BINDING_COUNT;++i) {
            seed=seed*1664525U+1013904223U;
            bool used=pattern==0 ? false : pattern==1 ? true :
                      pattern==2 ? i!=0 : pattern==3 ? i!=63 : (seed>>31)!=0;
            g_tcp_connections[i].socket=used ? &occupied : 0;
            g_tcp6_connections[i].socket=g_tcp_connections[i].socket;
        }
        for (unsigned required=0;required<=65;++required) check(required);
        check(UINT32_MAX);
    }
    memset(g_tcp_connections,0,sizeof(g_tcp_connections));
    memset(g_tcp6_connections,0,sizeof(g_tcp6_connections));
    check(1); assert(visits==1);
    check(2); assert(visits==2);
    printf("TCP slots: %u IPv4/IPv6 count-oracle cases; empty-table requests visit only 1/2 slots passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-socket-slots-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
