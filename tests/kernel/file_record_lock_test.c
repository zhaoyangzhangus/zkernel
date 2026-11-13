#include <assert.h>
#include <stdio.h>
#include <kernel/file_lock.h>
#include <uapi/linux_abi.h>
static size_t pins;
static size_t gets, puts_count;
void object_get(void *p) { assert(p); ++pins; ++gets; }
void object_put(void *p) { assert(p && pins); --pins; ++puts_count; }
uint32_t wake_all(wait_queue_t *q) { assert(q); return 0; }
static kstatus_t set(vnode_t *n, uint64_t pid, int64_t first, int64_t last, int type) {
    file_lock_range_t r = {pid, first, last, type};
    return file_lock_set(n, &r);
}
static file_lock_range_t query(vnode_t *n, uint64_t pid, int64_t first, int64_t last, int type) {
    file_lock_range_t r = {pid, first, last, type}, found;
    assert(file_lock_query(n, &r, &found) == K_OK);
    return found;
}
int main(void) {
    vnode_t a = {0}, b = {0};
    assert(set(&a, 1, 0, INT64_MAX, LINUX_F_WRLCK) == K_OK);
    unsigned sequence = atomic_load(&a.record_lock_sequence);
    size_t saved_gets = gets, saved_puts = puts_count;
    for (int i = 0; i < 1000; ++i) {
        assert(set(&a, 1, 0, INT64_MAX, LINUX_F_WRLCK) == K_OK);
        assert(set(&a, 1, 10, 19, LINUX_F_WRLCK) == K_OK);
        assert(set(&a, 2, 0, INT64_MAX, LINUX_F_UNLCK) == K_OK);
    }
    assert(atomic_load(&a.record_lock_sequence) == sequence);
    assert(gets == saved_gets && puts_count == saved_puts);
    assert(query(&a, 1, 0, 8, LINUX_F_WRLCK).type == LINUX_F_UNLCK);
    assert(query(&a, 2, 4, 8, LINUX_F_RDLCK).owner == 1);
    assert(set(&a, 2, 0, 0, LINUX_F_RDLCK) == K_EAGAIN);
    assert(set(&b, 2, 0, INT64_MAX, LINUX_F_WRLCK) == K_OK);
    assert(set(&a, 1, 10, 19, LINUX_F_UNLCK) == K_OK); /* split */
    assert(query(&a, 2, 10, 19, LINUX_F_WRLCK).type == LINUX_F_UNLCK);
    assert(query(&a, 2, 9, 9, LINUX_F_WRLCK).last == 9);
    assert(query(&a, 2, 20, 20, LINUX_F_WRLCK).first == 20);
    assert(set(&a, 1, 10, 19, LINUX_F_RDLCK) == K_OK);
    assert(set(&a, 2, 10, 19, LINUX_F_RDLCK) == K_OK); /* shared */
    assert(set(&a, 1, 10, 19, LINUX_F_WRLCK) == K_EAGAIN); /* upgrade atomic */
    assert(query(&a, 3, 10, 19, LINUX_F_RDLCK).type == LINUX_F_UNLCK);
    file_lock_release_file(&a, 2);
    assert(set(&a, 1, 10, 19, LINUX_F_WRLCK) == K_OK); /* merge all */
    file_lock_range_t r = query(&a, 3, 0, INT64_MAX, LINUX_F_WRLCK);
    assert(r.first == 0 && r.last == INT64_MAX && pins == 2);
    file_lock_release_owner(1);
    assert(query(&a, 3, 0, INT64_MAX, LINUX_F_WRLCK).type == LINUX_F_UNLCK);
    file_lock_release_owner(2);
    assert(pins == 0);
    /* 资源耗尽属于真实错误，并且必须保留所有已有区间。 */
    for (int i = 0; i < 512; ++i)
        assert(set(&a, 1, 2*i, 2*i, LINUX_F_WRLCK) == K_OK);
    assert(pins == 512);
    assert(set(&a, 1, 2000, 2001, LINUX_F_WRLCK) == K_ENOLCK);
    assert(set(&a, 1, 0, 0, LINUX_F_RDLCK) == K_OK); /* reuse own record */
    assert(query(&a, 2, 0, 0, LINUX_F_RDLCK).type == LINUX_F_UNLCK);
    assert(query(&a, 2, 2, 2, LINUX_F_RDLCK).type == LINUX_F_WRLCK);
    file_lock_release_owner(1);
    assert(!pins);
    assert(set(&a, 1, 0, INT64_MAX, LINUX_F_WRLCK) == K_OK);
    for (int i = 0; i < 511; ++i)
        assert(set(&b, 2, 2*i, 2*i, LINUX_F_WRLCK) == K_OK);
    sequence = atomic_load(&a.record_lock_sequence);
    saved_gets = gets; saved_puts = puts_count;
    assert(set(&a, 1, 5, 9, LINUX_F_WRLCK) == K_OK); /* full pool, unchanged */
    assert(set(&a, 3, 5, 9, LINUX_F_UNLCK) == K_OK);
    assert(atomic_load(&a.record_lock_sequence) == sequence);
    assert(gets == saved_gets && puts_count == saved_puts);
    assert(set(&a, 1, 5, 9, LINUX_F_UNLCK) == K_ENOLCK);
    assert(query(&a, 3, 5, 9, LINUX_F_RDLCK).type == LINUX_F_WRLCK);
    file_lock_release_owner(1);
    file_lock_release_owner(2);
    assert(!pins);
    assert(set(&a, 1, 0, 100, LINUX_F_WRLCK) == K_OK);
    assert(set(&b, 2, 0, 100, LINUX_F_WRLCK) == K_OK);
    file_lock_range_t waiting = {1, 0, 100, LINUX_F_WRLCK};
    uint32_t first_ticket, second_ticket;
    assert(file_lock_begin_wait(&b, &waiting, &first_ticket) == K_OK);
    waiting.owner = 2;
    assert(file_lock_begin_wait(&a, &waiting, &second_ticket) == K_EDEADLK);
    file_lock_end_wait(first_ticket);
    assert(file_lock_begin_wait(&a, &waiting, &second_ticket) == K_OK);
    file_lock_end_wait(second_ticket);
    file_lock_release_owner(1);
    file_lock_release_owner(2);
    assert(!pins);
    puts("Record locks: shared/exclusive, owner isolation, split/merge, EOF, cleanup, atomic capacity failures passed");
}
