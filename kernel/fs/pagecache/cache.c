#include <kernel/block_cache.h>
#include <kernel/string.h>
#ifdef OS_KERNEL_BUILD
#include <arch/x86_64/paging.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#endif

#ifndef OS_BLOCK_CACHE_READ_AHEAD_BLOCKS
/* 接纳数据范围时，为常用 FAT/目录扇区预留空间。每次未命中都占满缓存会破坏 LRU，并在原本连续的文件
 * 读取之间反复重新加载元数据。 */
#define OS_BLOCK_CACHE_READ_AHEAD_BLOCKS (OS_BLOCK_CACHE_ENTRY_COUNT - 1U)
#endif

static atomic_flag g_block_cache_diag_lock = ATOMIC_FLAG_INIT;

static VOID block_cache_diag_putc(char value) {
    for (UINT32 attempt = 0U; attempt < 100000U; ++attempt) {
        UINT8 ready;
        __asm__ volatile ("inb %w1, %0" : "=a"(ready) : "d"(0x3FDU));
        if ((ready & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((UINT8)value), "d"(0x3F8U));
}

static VOID block_cache_diag_text(const char *text) {
    while (*text != '\0') block_cache_diag_putc(*text++);
}

static VOID block_cache_diag_hex(UINT64 value) {
    static const char digits[] = "0123456789abcdef";
    block_cache_diag_text("0x");
    for (INT32 shift = 60; shift >= 0; shift -= 4) {
        block_cache_diag_putc(digits[(value >> (UINT32)shift) & 0xFU]);
    }
}

static VOID block_cache_diag(UINT32 stage, UINT64 lba,
                             const OS_BLOCK_CACHE *cache) {
    UINT64 current_pid = 0U;
    UINT64 current_tid = 0U;
    UINT64 owner_tid = 0U;
    (void)cache;
#ifdef OS_KERNEL_BUILD
    thread_t *current = sched_current_thread();
    if (current != 0) {
        current_tid = current->tid;
        if (current->process != 0) current_pid = current->process->pid;
    }
    if (cache != 0 && cache->WaitLock.owner != 0) {
        owner_tid = cache->WaitLock.owner->tid;
    }
#endif
    while (atomic_flag_test_and_set_explicit(&g_block_cache_diag_lock,
                                              memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    block_cache_diag_text("OS_BLOCK_CACHE_READ_FAIL stage=");
    block_cache_diag_hex(stage);
    block_cache_diag_text(" lba=");
    block_cache_diag_hex(lba);
    block_cache_diag_text(" pid=");
    block_cache_diag_hex(current_pid);
    block_cache_diag_text(" tid=");
    block_cache_diag_hex(current_tid);
    block_cache_diag_text(" owner_tid=");
    block_cache_diag_hex(owner_tid);
    block_cache_diag_text("\r\n");
    atomic_flag_clear_explicit(&g_block_cache_diag_lock, memory_order_release);
}

static VOID copy_bytes(UINT8 *destination, const UINT8 *source, UINT32 size) {
    memmove(destination, source, size);
}

static BOOLEAN cache_lock(OS_BLOCK_CACHE *cache, UINT64 lba) {
#ifdef OS_KERNEL_BUILD
    thread_t *current = sched_current_thread();
    if (current != 0) {
        if (current->sched_class == SCHED_CLASS_IDLE) {
            kstatus_t status;
            UINT64 flags = 0U;
            bool delivering = false;
            bool already_waiting = false;
            do {
                status = kmutex_try_lock(&cache->WaitLock);
                if (status == K_EAGAIN) {
                    if (!delivering) {
                        /* BSP 引导续接路径不能在互斥锁上休眠。锁所有者可能需要页表锁，而另一 CPU 正等待 BSP
                         * 确认 TLB。自旋等待期间允许接收 IPI，但不允许定时器切走这个尚未保存上下文的 idle
                         * 续接路径。遵循页表/vmalloc 的竞争处理协议。 */
                        __asm__ volatile ("pushfq; popq %0"
                                          : "=r"(flags) : : "memory");
                        sched_preempt_disable();
                        already_waiting = x86_tlb_shootdown_active();
                        if (!already_waiting) x86_tlb_wait_begin();
                        __asm__ volatile ("sti" : : : "memory");
                        delivering = true;
                    }
                    __asm__ volatile ("pause");
                }
            } while (status == K_EAGAIN);
            if (delivering) {
                if ((flags & (1ULL << 9)) == 0U)
                    __asm__ volatile ("cli" : : : "memory");
                if (!already_waiting) x86_tlb_wait_end();
                sched_preempt_enable();
            }
            if (status != K_OK) block_cache_diag((UINT32)status, lba, cache);
            return status == K_OK;
        }
        kstatus_t status = kmutex_lock(&cache->WaitLock, UINT64_MAX);
        if (status != K_OK) block_cache_diag((UINT32)status, lba, cache);
        return status == K_OK;
    }
#else
    (void)cache;
    (void)lba;
#endif

    /* 调度器尚无当前线程前，启动 I/O 仅在 BSP 上执行。 */
    while (__atomic_exchange_n(&cache->Lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile ("pause");
    }
    return 1;
}

static VOID cache_unlock(OS_BLOCK_CACHE *cache) {
#ifdef OS_KERNEL_BUILD
    thread_t *current = sched_current_thread();
    if (current != 0) {
        kstatus_t status = kmutex_unlock(&cache->WaitLock);
        if (status != K_OK) block_cache_diag((UINT32)status, 0U, cache);
        return;
    }
#endif
    __atomic_store_n(&cache->Lock, 0U, __ATOMIC_RELEASE);
}

static OS_BLOCK_CACHE_ENTRY *find_entry_locked(OS_BLOCK_CACHE *cache,
                                                   UINT64 lba) {
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Lba == lba) return entry;
    }
    return 0;
}

static BOOLEAN flush_entry_locked(OS_BLOCK_CACHE *cache,
                                  OS_BLOCK_CACHE_ENTRY *entry) {
    if (!entry->Valid || !entry->Dirty) return 1;
    UINT8 staging[OS_BLOCK_CACHE_MAX_SIZE];
    OS_BLOCK_CACHE_ENTRY *group[OS_BLOCK_CACHE_ENTRY_COUNT];
    UINT32 bytes = cache->Device->BlockSize;
    UINT32 count = 1U;
    if (bytes == 0U || bytes > sizeof(staging) ||
        entry->Lba >= cache->Device->BlockCount) return 0;
    group[0] = entry;
    /* 淘汰和显式写回共用连续脏块合并；单块直接使用缓存，批量暂存不超过 4 KiB。
     * 保持缓存锁直到设备完成，失败时整组仍为脏，覆盖设备可能已经部分写入的情况。 */
    while (count < OS_BLOCK_CACHE_ENTRY_COUNT &&
           count < sizeof(staging) / bytes &&
           count < cache->Device->BlockCount - entry->Lba) {
        OS_BLOCK_CACHE_ENTRY *next = find_entry_locked(cache, entry->Lba + count);
        if (next == 0 || !next->Dirty) break;
        group[count++] = next;
    }
    const UINT8 *data = entry->Data;
    if (count > 1U) {
        for (UINT32 i = 0U; i < count; ++i)
            copy_bytes(staging + (size_t)i * bytes, group[i]->Data, bytes);
        data = staging;
    }
    if (!block_write(cache->Device, entry->Lba, count, data)) return 0;
    for (UINT32 i = 0U; i < count; ++i) group[i]->Dirty = 0;
    return 1;
}

/* 零掩码表示普通写入替换项；预取路径会排除预留槽位。两条路径都优先选择第一个无效槽位，否则选择 LRU
 * 中最旧的槽位。 */
static OS_BLOCK_CACHE_ENTRY *select_entry_locked(
    OS_BLOCK_CACHE *cache, UINT32 reserved_mask) {
    _Static_assert(OS_BLOCK_CACHE_ENTRY_COUNT <= 32U,
                   "prefetch reservation mask must cover every cache slot");
    OS_BLOCK_CACHE_ENTRY *selected = 0;
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if ((reserved_mask & (1U << i)) != 0U) continue;
        if (!entry->Valid) return entry;
        if (selected == 0 || entry->LastUse < selected->LastUse) {
            selected = entry;
        }
    }
    return selected;
}

/* 写入既要查找命中项，也要选择替换项。遇到无效槽位时不要立即停止：后续匹配块应优先于淘汰操作。 */
static OS_BLOCK_CACHE_ENTRY *find_write_entry_locked(
    OS_BLOCK_CACHE *cache, UINT64 lba, BOOLEAN *hit) {
    OS_BLOCK_CACHE_ENTRY *selected = 0;
    *hit = 0;
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Lba == lba) {
            *hit = 1;
            return entry;
        }
        if (selected == 0 || (selected->Valid &&
            (!entry->Valid || entry->LastUse < selected->LastUse))) {
            selected = entry;
        }
    }
    return selected;
}

static BOOLEAN load_entry_locked(OS_BLOCK_CACHE *cache, UINT64 lba,
                                 OS_BLOCK_CACHE_ENTRY **result) {
    OS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0 || result == 0 || !cache->Initialized ||
        cache->Device == 0 || cache->Device->BlockSize > OS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) {
        block_cache_diag(2U, lba, cache);
        return 0;
    }

    entry = find_entry_locked(cache, lba);
    if (entry == 0) {
        OS_BLOCK_CACHE_ENTRY *reserved[OS_BLOCK_CACHE_READ_AHEAD_BLOCKS];
        UINT32 reserved_mask = 0U;
        UINT8 staging[OS_BLOCK_CACHE_READ_AHEAD_BLOCKS * 512U];
        UINT32 prefetch_limit = sizeof(staging) /
                                cache->Device->BlockSize;
        UINT32 reserved_count = 0U;

        if (prefetch_limit == 0U) prefetch_limit = 1U;
        if (prefetch_limit > OS_BLOCK_CACHE_READ_AHEAD_BLOCKS) {
            prefetch_limit = OS_BLOCK_CACHE_READ_AHEAD_BLOCKS;
        }
        /* 限制推测性接纳数量，使最近请求的扇区能在元数据/数据交错读取时保留。较旧的需求条目仍可作为
         * 普通 LRU 替换项；这不是永久固定。 */
        UINT32 hot_count = 0U;
        for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
            OS_BLOCK_CACHE_ENTRY *candidate = &cache->Entries[i];
            if (candidate->Valid && candidate->LastUse != 0U &&
                /* 多级目录与 FAT 会反复交错访问；保留近期需求数据，并为预读留下空间。
                 * 连续消费的冷预读数据不会升级，因此不会把整个扫描范围都当作热点。 */
                cache->Clock - candidate->LastUse <
                    OS_BLOCK_CACHE_ENTRY_COUNT * 5U / 8U) {
                ++hot_count;
                reserved_mask |= 1U << i;
            }
        }
        UINT32 admission_limit = OS_BLOCK_CACHE_ENTRY_COUNT - hot_count;
        if (admission_limit == 0U) admission_limit = 1U;
        if (prefetch_limit > admission_limit) prefetch_limit = admission_limit;

        /* FAT 读取通常跨越一到两个 4 KiB 页面。先预留缓存槽位，再发出一个连续读取请求，并将结果拆分到
         * 预留槽位中。暂存缓冲区有意与首个缓存项分离：NVMe 后端可能使用复合双页 DMA 缓冲区读取 16 个扇区。 */
        while (reserved_count < prefetch_limit &&
               reserved_count < cache->Device->BlockCount - lba) {
            uint64_t next_lba = lba + reserved_count;
            if (find_entry_locked(cache, next_lba) != 0) break;
            entry = select_entry_locked(cache, reserved_mask);
            if (entry == 0 || !flush_entry_locked(cache, entry)) {
                block_cache_diag(3U, lba, cache);
                return 0;
            }
            entry->Valid = 0;
            entry->Dirty = 0;
            reserved_mask |= 1U << (UINT32)(entry - cache->Entries);
            reserved[reserved_count++] = entry;
        }
        if (reserved_count == 0U ||
            !block_read(cache->Device, lba, reserved_count,
                               staging)) {
            block_cache_diag(reserved_count == 0U ? 4U : 5U, lba, cache);
            for (UINT32 i = 0; i < reserved_count; ++i) {
                reserved[i]->Valid = 0;
                reserved[i]->Dirty = 0;
            }
            return 0;
        }
        for (UINT32 i = 0; i < reserved_count; ++i) {
            entry = reserved[i];
            copy_bytes(entry->Data,
                       staging + i * cache->Device->BlockSize,
                       cache->Device->BlockSize);
            entry->Valid = 1;
            entry->Dirty = 0;
            entry->Lba = lba + i;
            /* 推测读取的扇区在实际请求前都视为冷数据。若将整个范围标为需求读取，会仅因另一文件触发预读，
             * 就驱逐常用 FAT/目录扇区。 */
            entry->LastUse = 0U;
        }
        entry = reserved[0];
    }

    /* 连续消费预读扇区不应把一次目录扫描的每个扇区都升级为热点，
     * 否则较深目录会挤出每次路径查询都需要的父目录。非连续命中仍正常提升。 */
    if (entry->LastUse != 0U || !cache->LastReadValid ||
        cache->LastReadLba >= lba || lba - cache->LastReadLba != 1U) {
        entry->LastUse = ++cache->Clock;
    }
    cache->LastReadLba = lba;
    cache->LastReadValid = 1;
    *result = entry;
    return 1;
}

static BOOLEAN writeback_cache_locked(OS_BLOCK_CACHE *cache) {
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        if (!flush_entry_locked(cache, &cache->Entries[i])) return 0;
    }
    return 1;
}

static BOOLEAN flush_cache_locked(OS_BLOCK_CACHE *cache) {
    return writeback_cache_locked(cache) && block_flush(cache->Device);
}

BOOLEAN block_cache_init(OS_BLOCK_CACHE *cache,
                                const OS_BLOCK_DEVICE *device) {
    if (cache == 0 || device == 0 || !device->Registered || device->BlockSize == 0U ||
        device->BlockSize > OS_BLOCK_CACHE_MAX_SIZE || cache->Initialized) return 0;

    cache->Device = device;
    cache->Clock = 0;
    cache->LastReadLba = 0;
    cache->LastReadValid = 0;
    cache->Lock = 0U;
#ifdef OS_KERNEL_BUILD
    kmutex_init(&cache->WaitLock, false);
#endif
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
        cache->Entries[i].Lba = 0;
        cache->Entries[i].LastUse = 0;
    }
    __atomic_store_n(&cache->Initialized, 1, __ATOMIC_RELEASE);
    return 1;
}

BOOLEAN block_cache_read(OS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer) {
    OS_BLOCK_CACHE_ENTRY *entry;
    BOOLEAN success = 0;

    if (cache == 0 || buffer == 0) return 0;
    if (!cache_lock(cache, lba)) return 0;
    if (load_entry_locked(cache, lba, &entry)) {
        copy_bytes((UINT8 *)buffer, entry->Data, cache->Device->BlockSize);
        success = 1;
    }
    cache_unlock(cache);
    return success;
}

BOOLEAN block_cache_read_range(OS_BLOCK_CACHE *cache, UINT64 lba,
                                      UINT32 count, VOID *buffer) {
    if (cache == 0 || buffer == 0 || count == 0U) return 0;
    if (!cache_lock(cache, lba)) return 0;
    const OS_BLOCK_DEVICE *device = cache->Device;
    if (!cache->Initialized || device == 0 || device->BlockSize == 0U ||
        device->BlockSize > OS_BLOCK_CACHE_MAX_SIZE ||
        count > OS_BLOCK_CACHE_MAX_READ_BYTES / device->BlockSize ||
        lba >= device->BlockCount || count > device->BlockCount - lba) {
        cache_unlock(cache);
        return 0;
    }
    /* 仅排序实际命中的缓存槽位，空间与请求长度无关。未命中区间可直接作为一个设备请求，
     * 不必为了固定的逐扇区索引把连续数据切成 64 KiB。重复 LBA 仍优先采用首个槽位。 */
    UINT8 slots[OS_BLOCK_CACHE_ENTRY_COUNT];
    UINT32 offsets[OS_BLOCK_CACHE_ENTRY_COUNT];
    UINT32 hits = 0U;
    _Static_assert(OS_BLOCK_CACHE_ENTRY_COUNT <= 256U, "range slot index fits byte");
    if (count == 1U) {
        OS_BLOCK_CACHE_ENTRY *single = find_entry_locked(cache, lba);
        if (single != 0) {
            slots[0] = (UINT8)(single - cache->Entries);
            offsets[0] = 0U;
            hits = 1U;
        }
    } else for (UINT32 index = 0U; index < OS_BLOCK_CACHE_ENTRY_COUNT; ++index) {
        OS_BLOCK_CACHE_ENTRY *candidate = &cache->Entries[index];
        if (!candidate->Valid || candidate->Lba < lba) continue;
        UINT64 offset = candidate->Lba - lba;
        if (offset >= count) continue;
        UINT32 at = 0U;
        while (at < hits && offsets[at] < offset) ++at;
        if (at < hits && offsets[at] == offset) continue;
        for (UINT32 move = hits; move > at; --move) {
            offsets[move] = offsets[move - 1U];
            slots[move] = slots[move - 1U];
        }
        offsets[at] = (UINT32)offset;
        slots[at] = (UINT8)index;
        ++hits;
    }
    UINT32 done = 0U;
    for (UINT32 hit = 0U; hit <= hits; ++hit) {
        UINT32 next = hit < hits ? offsets[hit] : count;
        UINT8 *out = (UINT8 *)buffer + (size_t)done * device->BlockSize;
        /* 后端读取期间保持缓存锁：并发脏写不得被较旧的磁盘版本覆盖。批量未命中不会淘汰常用 FAT 元数据，
         * 也不需要预留推测读取槽位。 */
        if (next > done && !block_read(device, lba + done, next - done, out)) {
            cache_unlock(cache);
            return 0;
        }
        done = next;
        if (hit < hits) {
            OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[slots[hit]];
            copy_bytes((UINT8 *)buffer + (size_t)done * device->BlockSize,
                       entry->Data, device->BlockSize);
            entry->LastUse = ++cache->Clock;
            ++done;
        }
    }
    cache_unlock(cache);
    return 1;
}

BOOLEAN block_cache_write(OS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer) {
    OS_BLOCK_CACHE_ENTRY *entry;

    if (cache == 0 || buffer == 0) return 0;
    if (!cache_lock(cache, lba)) return 0;

    if (!cache->Initialized || cache->Device == 0 ||
        cache->Device->BlockSize > OS_BLOCK_CACHE_MAX_SIZE ||
        lba >= cache->Device->BlockCount) {
        cache_unlock(cache);
        return 0;
    }

    BOOLEAN hit;
    entry = find_write_entry_locked(cache, lba, &hit);
    if (!hit) {
        if (entry == 0 || !flush_entry_locked(cache, entry)) {
            cache_unlock(cache);
            return 0;
        }
        entry->Valid = 1;
        entry->Dirty = 0;
        entry->Lba = lba;
    }

    ++cache->Clock;
    entry->LastUse = cache->Clock;
    copy_bytes(entry->Data, (const UINT8 *)buffer, cache->Device->BlockSize);
    entry->Dirty = 1;

    cache_unlock(cache);
    return 1;
}

/* 在缓存锁内协调连续写入：先保存覆盖范围中的旧脏数据，再丢弃覆盖范围缓存。
 * 设备即使只写了一部分就报错，后续读取也会看到实际设备内容，其他脏块不会丢失。 */
BOOLEAN block_cache_write_range(OS_BLOCK_CACHE *cache, UINT64 lba,
                                UINT32 count, const VOID *buffer) {
    if (cache == 0 || buffer == 0 || count == 0U) return 0;
    if (!cache_lock(cache, lba)) return 0;
    const OS_BLOCK_DEVICE *device = cache->Device;
    if (!cache->Initialized || device == 0 || device->BlockSize == 0U ||
        device->BlockSize > OS_BLOCK_CACHE_MAX_SIZE ||
        count > OS_BLOCK_CACHE_MAX_WRITE_BYTES / device->BlockSize ||
        lba >= device->BlockCount || count > device->BlockCount - lba) {
        cache_unlock(cache);
        return 0;
    }
    for (UINT32 i = 0U; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Dirty && entry->Lba >= lba &&
            entry->Lba - lba < count && !flush_entry_locked(cache, entry)) {
            cache_unlock(cache);
            return 0;
        }
    }
    for (UINT32 i = 0U; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        OS_BLOCK_CACHE_ENTRY *entry = &cache->Entries[i];
        if (entry->Valid && entry->Lba >= lba && entry->Lba - lba < count) {
            entry->Valid = 0;
            entry->Dirty = 0;
        }
    }
    BOOLEAN success = block_write(device, lba, count, buffer);
    cache_unlock(cache);
    return success;
}

BOOLEAN block_cache_flush(OS_BLOCK_CACHE *cache) {
    BOOLEAN success;
    if (cache == 0) return 0;

    if (!cache_lock(cache, 0U)) return 0;
    if (!cache->Initialized || cache->Device == 0) {
        cache_unlock(cache);
        return 0;
    }

    success = flush_cache_locked(cache);
    cache_unlock(cache);
    return success;
}

BOOLEAN block_cache_writeback(OS_BLOCK_CACHE *cache) {
    BOOLEAN success;
    if (cache == 0) return 0;

    if (!cache_lock(cache, 0U)) return 0;
    if (!cache->Initialized || cache->Device == 0) {
        cache_unlock(cache);
        return 0;
    }

    success = writeback_cache_locked(cache);
    cache_unlock(cache);
    return success;
}

BOOLEAN block_cache_destroy(OS_BLOCK_CACHE *cache) {
    if (cache == 0) return 0;

    if (!cache_lock(cache, 0U)) return 0;
    if (!cache->Initialized || cache->Device == 0 || !flush_cache_locked(cache)) {
        cache_unlock(cache);
        return 0;
    }

    cache->Initialized = 0;
    cache->Device = 0;
    cache->Clock = 0;
    for (UINT32 i = 0; i < OS_BLOCK_CACHE_ENTRY_COUNT; ++i) {
        cache->Entries[i].Valid = 0;
        cache->Entries[i].Dirty = 0;
        cache->Entries[i].Lba = 0;
        cache->Entries[i].LastUse = 0;
    }

    cache_unlock(cache);
    return 1;
}

BOOLEAN block_cache_invalidate(OS_BLOCK_CACHE *cache, UINT64 lba) {
    OS_BLOCK_CACHE_ENTRY *entry;
    if (cache == 0) return 0;

    if (!cache_lock(cache, lba)) return 0;
    if (!cache->Initialized) {
        cache_unlock(cache);
        return 0;
    }

    entry = find_entry_locked(cache, lba);
    if (entry == 0) {
        cache_unlock(cache);
        return 1;
    }
    if (!flush_entry_locked(cache, entry)) {
        cache_unlock(cache);
        return 0;
    }

    entry->Valid = 0;
    entry->Dirty = 0;
    entry->Lba = 0;
    entry->LastUse = 0;

    cache_unlock(cache);
    return 1;
}
