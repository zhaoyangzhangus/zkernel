"""Actual descriptor table destroy/clone: complete copies and lock-safe release."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
functions = source[source.index('void linux_fd_table_destroy('):
                   source.index('static void linux_fd_release_locks(')]
initialize = source[source.index('void linux_fd_table_init('):
                    source.index('void linux_fd_table_destroy(')]
fd_limit = int(re.search(r'#define OS_LINUX_FD_LIMIT (\d+)U',
                        (root / 'include/kernel/linux_abi.h').read_text())[1])
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OS_LINUX_FD_LIMIT FD_LIMIT_FROM_HEADER
#define K_OK 0
#define K_EINVAL -22
#define LINUX_FD_UNUSED 0
#define LINUX_FD_CONSOLE 2
typedef int kstatus_t;
typedef struct { unsigned refs; } owned_t;
typedef struct { owned_t *file, *private_data, *description;
                 unsigned flags; unsigned char kind; } linux_fd_entry_t;
typedef struct { bool held; } spinlock_t;
typedef struct { spinlock_t lock; linux_fd_entry_t entries[OS_LINUX_FD_LIMIT]; } linux_fd_table_t;
static unsigned held, gets, puts, initializations, lock_calls, stdio_calls;
static unsigned release_calls;
static spinlock_t *first_locked, *second_locked;
static linux_fd_table_t *releasing;
static bool reenter;
void linux_fd_table_destroy(linux_fd_table_t *table);
static void spinlock_lock(spinlock_t *lock) {
    ++lock_calls;
    if (!held) first_locked=lock;
    else {assert(held==1 && (uintptr_t)first_locked < (uintptr_t)lock); second_locked=lock;}
    assert(!lock->held); lock->held = true; ++held;
}
static void spinlock_unlock(spinlock_t *lock) {
    assert(lock==(held==2?second_locked:first_locked));
    assert(lock->held && held); lock->held = false; --held;
}
static void linux_zero(void *p, size_t size) { memset(p, 0, size); }
static void spinlock_init(spinlock_t *lock) {
    ++initializations; lock->held=false;
}
static void linux_fd_table_install_stdio(linux_fd_table_t *table) {
    ++stdio_calls;
    spinlock_lock(&table->lock);
    for (unsigned i=0;i<3;++i) {
        table->entries[i].file=NULL;table->entries[i].private_data=NULL;
        table->entries[i].kind=LINUX_FD_CONSOLE;
        table->entries[i].flags=i?1U:0U;
    }
    spinlock_unlock(&table->lock);
}
static void assert_empty(const linux_fd_table_t *table) {
    for (unsigned i = 0; i < OS_LINUX_FD_LIMIT; ++i) {
        const linux_fd_entry_t *e = &table->entries[i];
        assert(!e->file && !e->private_data && !e->description && !e->kind && !e->flags);
    }
}
static void linux_fd_entry_get(const linux_fd_entry_t *entry) {
    assert(held == 2);
    if (entry->file) { assert(entry->file->refs); ++entry->file->refs; ++gets; }
    if (entry->description) { assert(entry->description->refs); ++entry->description->refs; ++gets; }
}
static void linux_fd_entry_put(linux_fd_entry_t *entry) {
    assert(!held);
    ++release_calls;
    if (releasing) assert_empty(releasing);
    if (reenter) { reenter=false; linux_fd_table_destroy(releasing); }
    if (entry->file) { assert(entry->file->refs); --entry->file->refs; ++puts; }
    if (entry->description) { assert(entry->description->refs); --entry->description->refs; ++puts; }
    *entry = (linux_fd_entry_t){0};
}
'''
checks = r'''
int main(void) {
    (void)linux_zero;
    (void)linux_fd_table_init;
    (void)linux_fd_table_install_stdio;
    linux_fd_table_t fresh;
    memset(&fresh,0xa5,sizeof(fresh));
    linux_fd_table_init(NULL);
    assert(!initializations && !lock_calls && !stdio_calls);
    linux_fd_table_init(&fresh);
    assert(initializations==1 && !lock_calls && !stdio_calls && !fresh.lock.held);
    for (unsigned i=0;i<OS_LINUX_FD_LIMIT;++i) {
        const linux_fd_entry_t *e=&fresh.entries[i];
        assert(!e->file && !e->private_data && !e->description);
        assert(e->kind==(i<3?LINUX_FD_CONSOLE:LINUX_FD_UNUSED));
        assert(e->flags==(i==1 || i==2?1U:0U));
    }
    linux_fd_table_t pair[2]={0};
    assert(linux_fd_table_clone(&pair[0],&pair[1])==K_OK);
    assert(linux_fd_table_clone(&pair[1],&pair[0])==K_OK);
    assert(!held && !gets && !puts && !release_calls);
    for (unsigned mask = 0; mask < 128; ++mask) {
        linux_fd_table_t source = {0}, destination = {0};
        owned_t objects[OS_LINUX_FD_LIMIT], descriptions[OS_LINUX_FD_LIMIT], old[OS_LINUX_FD_LIMIT];
        unsigned populated = 0;
        for (unsigned i = 0; i < OS_LINUX_FD_LIMIT; ++i) {
            objects[i].refs = descriptions[i].refs = 0;
            old[i].refs = 1;
            destination.entries[i] = (linux_fd_entry_t){.file=&old[i], .kind=1};
            if ((i + mask) % 7 < mask % 8) {
                objects[i].refs = descriptions[i].refs = 1; ++populated;
                source.entries[i] = (linux_fd_entry_t){.file=&objects[i],
                    .description=&descriptions[i], .flags=mask+i, .kind=1};
            }
        }
        linux_fd_entry_t saved[OS_LINUX_FD_LIMIT]; memcpy(saved, source.entries, sizeof(saved));
        gets = puts = initializations = release_calls = 0;
        releasing = &destination; reenter=true;
        assert(linux_fd_table_clone(&source, &destination) == K_OK);
        releasing = NULL;
        assert(!held && !initializations && gets == populated*2 && puts == OS_LINUX_FD_LIMIT);
        assert(release_calls == OS_LINUX_FD_LIMIT);
        assert(!reenter);
        for (unsigned i = 0; i < OS_LINUX_FD_LIMIT; ++i) {
            assert(!old[i].refs);
            assert(!memcmp(&saved[i], &source.entries[i], sizeof(saved[i])));
            assert(destination.entries[i].file == saved[i].file);
            assert(destination.entries[i].description == saved[i].description);
            assert(destination.entries[i].private_data == saved[i].private_data);
            assert(destination.entries[i].flags == saved[i].flags);
            assert(destination.entries[i].kind == saved[i].kind);
            assert(objects[i].refs == (saved[i].file ? 2U : 0U));
        }
        unsigned before = gets + puts;
        assert(linux_fd_table_clone(&source, &source) == K_OK);
        assert(linux_fd_table_clone(NULL, &destination) == K_EINVAL);
        assert(linux_fd_table_clone(&source, NULL) == K_EINVAL);
        assert(linux_fd_table_clone(NULL, NULL) == K_OK);
        linux_fd_table_destroy(NULL);
        assert(gets + puts == before);
        releasing = &destination; linux_fd_table_destroy(&destination);
        assert_empty(&destination);
        linux_fd_table_destroy(&destination); /* repeated destruction */
        releasing = &source; linux_fd_table_destroy(&source);
        releasing = NULL; assert_empty(&source);
        assert(puts == OS_LINUX_FD_LIMIT + populated*4 && !held);
        assert(release_calls == OS_LINUX_FD_LIMIT + populated*2);
        for (unsigned i = 0; i < OS_LINUX_FD_LIMIT; ++i) assert(!objects[i].refs && !descriptions[i].refs);
    }
    linux_fd_table_t atypical={0};owned_t description={1};
    atypical.entries[10].description=&description;
    unsigned calls=release_calls;
    releasing=&atypical;linux_fd_table_destroy(&atypical);releasing=NULL;
    assert(!description.refs && release_calls==calls+1);assert_empty(&atypical);
    puts("128 sparse/dense table layouts: copy, self/null cases, repeated/reentrant teardown, sparse-only unlocked releases and unused-kind description ownership passed");
}
'''
# 不要在测试夹具中遮蔽 C 库的 puts()。
code = code.replace('puts', 'release_count').replace('FD_LIMIT_FROM_HEADER', str(fd_limit))
checks = checks.replace('puts', 'release_count').replace('release_count("', 'puts("')
with tempfile.TemporaryDirectory(prefix='liteos-fd-table-') as directory:
    base = Path(directory)
    def run(body):
        path = base / 'test.c'
        path.write_text(code + initialize + body + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-no-pie', str(path),
                        '-o', str(base / 'test')], check=True)
        return subprocess.run([str(base / 'test')], capture_output=True, text=True)
    result = run(functions)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    saved_initialize=initialize
    initialize=initialize.replace('    spinlock_init(&table->lock);',
        '    spinlock_init(&table->lock); linux_fd_table_install_stdio(table);')
    result=run(functions)
    assert result.returncode != 0 and 'lock_calls' in result.stderr,result.stderr
    initialize=saved_initialize
    print('Negative control rejected: initialization locks an unpublished table')
    initialize=initialize.replace('descriptor == 1U || descriptor == 2U ? 1U : 0U',
                                  '0U')
    result=run(functions)
    assert result.returncode != 0 and 'e->flags' in result.stderr,result.stderr
    initialize=saved_initialize
    print('Negative control rejected: initialization loses stdio access flags')
    negative = functions.replace('linux_fd_table_destroy(destination);',
                                 'linux_fd_table_destroy(destination); linux_fd_table_init(destination);')
    result = run(negative)
    assert result.returncode != 0, 'Negative control accepted redundant lock/table initialization'
    print('Negative control rejected: clone reinitializes the destination table')
    negative = functions.replace('table->entries[descriptor] = (linux_fd_entry_t){0};', '')
    result = run(negative)
    assert result.returncode != 0, 'Negative control released entries still published in the table'
    print('Negative control rejected: release callbacks see attached entries')
    negative = functions.replace('descriptor < count;', 'descriptor < OS_LINUX_FD_LIMIT;')
    result = run(negative)
    assert result.returncode != 0, 'Negative control released uninitialized or empty scratch entries'
    print('Negative control rejected: noncompact release loop; sparse and repeated teardown skip empty entries')
    negative = functions.replace('(uintptr_t)source_lock < (uintptr_t)destination_lock',
                                 '(uintptr_t)source_lock > (uintptr_t)destination_lock')
    result = run(negative)
    assert result.returncode != 0 and 'first_locked' in result.stderr,result.stderr
    print('Negative control rejected: reverse table lock ordering; fixed two-lock acquisition and reverse unlock passed')
