"""Real TCP lookup hints: first-match oracle, lifetime and scan bounds."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


functions = '\n'.join(function(name) for name in (
    'bool socket_address6_equal(', 'bool socket_address6_zero(',
    'static bool socket_tcp_matches(', 'static bool socket_tcp6_matches(',
    'int32_t socket_find_tcp_locked(', 'int32_t socket_find_tcp6_locked(',
    'bool socket_register_tcp_locked(', 'static uint32_t socket_unregister_tcp_locked(',
))
for name in ('socket_tcp_matches', 'socket_tcp6_matches'):
    start = functions.index('static bool ' + name + '(')
    opening = functions.index('{', start)
    functions = functions[:opening + 1] + '\n    ++probes;' + functions[opening + 1:]

program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
static socket_tcp_connection_t tables4[2][512];
static socket_tcp6_connection_t tables6[2][512];
static socket_tcp_connection_t *g_tcp_connections = tables4[0];
static socket_tcp6_connection_t *g_tcp6_connections = tables6[0];
static uint32_t g_registry_capacity = 512;
static uint32_t g_tcp_lookup_hint = UINT32_MAX, g_tcp6_lookup_hint = UINT32_MAX;
static unsigned probes;
static socket_t sockets[514];
void socket_zero(void *p, size_t n) { memset(p, 0, n); }
void socket_copy(void *d, const void *s, size_t n) { memmove(d, s, n); }
''' + functions + r'''
static unsigned cases;
static void check(unsigned family, unsigned peer, unsigned local, unsigned port) {
    int expected = -1;
    for (unsigned i = 0; i < g_registry_capacity; ++i) {
        bool match;
        if (family == 4) {
            const socket_tcp_connection_t *c = &g_tcp_connections[i];
            match = c->socket && c->peer_port == 443 && c->local_port == port &&
                c->peer_address == peer && (!c->local_address || c->local_address == local);
        } else {
            const socket_tcp6_connection_t *c = &g_tcp6_connections[i];
            match = c->socket && c->peer_port == 443 && c->local_port == port &&
                c->peer_address[0] == peer &&
                (!c->local_address[0] || c->local_address[0] == local);
        }
        if (match) { expected = (int)i; break; }
    }
    int actual;
    if (family == 4) actual = socket_find_tcp_locked(peer, 443, local, port);
    else {
        uint8_t src[16] = {peer}, dst[16] = {local};
        actual = socket_find_tcp6_locked(src, 443, dst, port);
    }
    assert(actual == expected); ++cases;
}
int main(void) {
    for (unsigned family = 4; family <= 6; family += 2) {
        g_registry_capacity = 512;
        g_tcp_connections = tables4[0]; g_tcp6_connections = tables6[0];
        memset(tables4, 0, sizeof(tables4)); memset(tables6, 0, sizeof(tables6));
        g_tcp_lookup_hint = g_tcp6_lookup_hint = UINT32_MAX;
        for (unsigned i = 0; i < 512; ++i) {
            if (family == 4) g_tcp_connections[i] = (socket_tcp_connection_t){
                .socket = &sockets[i], .peer_address = 2, .local_address = 1,
                .peer_port = 443, .local_port = 4000+i};
            else {
                g_tcp6_connections[i] = (socket_tcp6_connection_t){
                    .socket = &sockets[i], .peer_port = 443, .local_port = 4000+i};
                g_tcp6_connections[i].peer_address[0] = 2;
                g_tcp6_connections[i].local_address[0] = 1;
            }
        }
        probes = 0; check(family, 2, 1, 4511); assert(probes == 512);
        for (unsigned i = 0; i < 10000; ++i) {
            probes = 0; check(family, 2, 1, 4511); assert(probes == 1);
        }
        /* Registry growth relocates storage, but hints retain only indices. */
        memcpy(tables4[1], tables4[0], sizeof(tables4[0]));
        memcpy(tables6[1], tables6[0], sizeof(tables6[0]));
        g_tcp_connections = tables4[1]; g_tcp6_connections = tables6[1];
        memset(tables4[0], 0xa5, sizeof(tables4[0]));
        memset(tables6[0], 0xa5, sizeof(tables6[0]));
        probes = 0; check(family, 2, 1, 4511); assert(probes == 1);
        /* A newly registered earlier wildcard must supersede the hot entry. */
        assert(socket_unregister_tcp_locked(&sockets[0]) == 1);
        check(family, 2, 1, 4511);
        sockets[512].family = family == 4 ? OS_AF_INET4 : OS_AF_INET6;
        sockets[512].type = OS_SOCK_STREAM;
        socket_private_t state = {.bound = true, .connected = true,
            .peer_port = 443, .local_port = 4511, .peer_address = 2};
        state.peer_address6[0] = 2;
        assert(socket_register_tcp_locked(&sockets[512], &state));
        check(family, 2, 1, 4511); check(family, 2, 9, 4511);
        assert(socket_unregister_tcp_locked(&sockets[512]) == 1);
        state.local_address = 3; state.local_address6[0] = 3;
        sockets[513].family = sockets[512].family;
        sockets[513].type = OS_SOCK_STREAM;
        assert(socket_register_tcp_locked(&sockets[513], &state));
        check(family, 2, 1, 4511); check(family, 2, 3, 4511);
        check(family, 9, 3, 4511); check(family, 2, 9, 4511);
        uint32_t seed = 19;
        for (unsigned i = 0; i < 20000; ++i) {
            seed = seed * 1664525U + 1013904223U;
            check(family, 2 + (i % 5 == 0), 1 + (i % 7 == 0), 4000 + (seed % 520));
        }
        /* A stale, now out-of-range hint never accesses the retired slot. */
        check(family, 2, 1, 4511); g_registry_capacity = 64;
        check(family, 2, 1, 4511); check(family, 2, 1, 4001);
    }
    printf("TCP lookup: %u oracle cases, hot last slot 512->1 probes, relocated tables, wildcard precedence, unregister/reuse and stale bounds PASS\n", cases);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-lookup-hint-') as directory:
    path = Path(directory)
    variants = {
        'current': program,
        'stale-hint': program.replace(
            'g_tcp_lookup_hint = g_tcp6_lookup_hint = UINT32_MAX;', ''),
        'no-hint': program.replace('g_tcp_lookup_hint < g_registry_capacity &&', 'false &&')
                          .replace('g_tcp6_lookup_hint < g_registry_capacity &&', 'false &&'),
    }
    for name, text in variants.items():
        src = path / (name + '.c'); exe = path / name
        src.write_text(text)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(root/'include'), '-I'+str(root/'kernel/net'),
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(src), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        assert (result.returncode == 0) == (name == 'current'), (name, result.stderr)
        if name == 'current': print(result.stdout, end='')
print('Negative controls rejected: missing mutation invalidation; full scan on every lookup')
