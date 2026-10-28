"""Exercise actual Unix I/O mutex and pipe timeout expressions in all six paths.

The mutex/pipe fixtures model contention, not the full ancillary protocol.
Their calls and timeout helper are extracted unchanged from production code.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/syscall/linux.c').read_text()

def body(name):
    match = re.search(r'static [^\n]+ ' + name + r'\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

def call(text, prefix):
    start = text.index(prefix)
    end = text.index('(', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '(') - (text[end] == ')')
        end += 1
    return text[start:end]

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define LINUX_O_NONBLOCK 0x800U
#define LINUX_MSG_DONTWAIT 0x40U
#define K_OK 0
#define K_ETIMEDOUT -110
#define K_EBUSY -16
#define K_EAGAIN -11
typedef int kstatus_t;
typedef struct { uint32_t flags; } linux_fd_entry_t;
typedef struct { int read_mutex, write_mutex; } channel_t;
typedef struct { void *read_end, *write_end; } socket_t;
static bool held;
static unsigned pipe_calls;
static uint64_t mutex_timeout, pipe_timeout;
static int kmutex_lock(int *mutex, uint64_t timeout) {
    assert(mutex); mutex_timeout = timeout;
    return held ? (timeout == 0 ? K_ETIMEDOUT : K_EBUSY) : K_OK;
}
static int pipe_read(void *end, void *data, uint64_t size,
                     uint64_t timeout, uint64_t *bytes) {
    (void)data; assert(end); ++pipe_calls; pipe_timeout = timeout;
    *bytes = size; return K_OK;
}
static int pipe_write(void *end, const void *data, uint64_t size,
                      uint64_t timeout, uint64_t *bytes) {
    return pipe_read(end, (void*)data, size, timeout, bytes);
}
static unsigned linux_unix_channel_pipe_limit(void *context,unsigned n) {
    (void)context;return n;
}
static int pipe_read_limited(void *end,void *data,uint64_t size,uint64_t timeout,
                            uint64_t *bytes,unsigned (*limit)(void*,unsigned),
                            void *context) {
    assert(limit==linux_unix_channel_pipe_limit && context);
    return pipe_read(end,data,size,timeout,bytes);
}
static int64_t linux_copy_status(kstatus_t status) { return status; }
''' + body('linux_socket_timeout') + '\n' + body('linux_pipe_status') + '\n'
names = ('linux_sendmsg', 'linux_recvmsg', 'linux_unix_sendto',
         'linux_unix_recvfrom', 'linux_read', 'linux_write')
for index, name in enumerate(names):
    text = body(name)
    mutex_call = call(text, 'kmutex_lock(&channel->')
    suffix = text[text.index(mutex_call) + len(mutex_call):]
    pipe_call = call(suffix, 'pipe_write(' if index in (0, 2, 5) else
                     'pipe_read_limited(' if index == 1 else 'pipe_read(')
    entry_decl = ('linux_fd_entry_t *entry = &value;' if index in (2, 3)
                  else 'linux_fd_entry_t entry = value;')
    program += f'''
static int path{index}(uint32_t flags, uint32_t message_flags) {{
    (void)message_flags;
    linux_fd_entry_t value = {{flags}};
    {entry_decl}
    channel_t state = {{0}}; channel_t *channel = &state;
    socket_t sock = {{&state, &state}}; socket_t *socket = &sock;
    void *read_end = &state, *write_end = &state;
    unsigned char payload[8] = {{0}}, temporary[8] = {{0}};
    uint64_t bytes = 0;
    unsigned request = 1, request_length = 1, payload_length = 1;
    (void)socket; (void)read_end; (void)write_end; (void)payload;
    (void)temporary; (void)request; (void)request_length; (void)payload_length;
    int result = {mutex_call};
    if (result == K_OK) result = {pipe_call};
    return (int)linux_pipe_status(result);
}}
'''
program += 'int main(void) {\nassert(linux_socket_timeout(0, LINUX_MSG_DONTWAIT) == 0);\n'
for index, name in enumerate(names):
    for flags, messages in ((0, 0), (0x800, 0), (0, 0x40), (0x800, 0x40)):
        nonblocking = bool(flags or (messages and index < 4))
        expected_timeout = '0' if nonblocking else 'UINT64_MAX'
        expected_held = 'K_EAGAIN' if nonblocking else 'K_EBUSY'
        program += f'''
    held = true; pipe_calls = 0;
    assert(path{index}({flags}, {messages}) == {expected_held});
    assert(mutex_timeout == {expected_timeout} && pipe_calls == 0);
    held = false; pipe_calls = 0;
    assert(path{index}({flags}, {messages}) == K_OK);
    assert(mutex_timeout == {expected_timeout});
    assert(pipe_timeout == {expected_timeout} && pipe_calls == 1);
'''
program += '}\n'
with tempfile.TemporaryDirectory(prefix='liteos-unix-nonblock-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Unix I/O: six production lock/pipe paths honor O_NONBLOCK/MSG_DONTWAIT; contention returns EAGAIN')
