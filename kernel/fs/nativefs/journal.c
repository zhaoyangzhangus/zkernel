#include <kernel/journal.h>
#include <kernel/crc32.h>
#include <kernel/string.h>
#include <kernel/block.h>
#include <kernel/kmem.h>
#include <kernel/io.h>
#include <kernel/deferred.h>
#include <kernel/nvme_core.h>

#define JOURNAL_MAGIC 0x4C4A4E4CU
#define JOURNAL_KIND_BEGIN  1U
#define JOURNAL_KIND_DATA   2U
#define JOURNAL_KIND_COMMIT 3U
#define JOURNAL_DATA_LIMIT (JOURNAL_BLOCK_SIZE - 32U)

typedef struct __attribute__((packed)) journal_disk_header {
    uint32_t magic;
    uint64_t sequence;
    uint32_t kind;
    uint64_t target;
    uint32_t length;
    uint32_t checksum;
} journal_disk_header_t;

_Static_assert(sizeof(journal_disk_header_t) == 32U, "journal header size");

static kstatus_t journal_read_slot(const journal_t *journal, uint32_t slot,
                                   uint8_t block[JOURNAL_BLOCK_SIZE]);
static bool journal_decode(const uint8_t block[JOURNAL_BLOCK_SIZE],
                           journal_disk_header_t *header);

static void journal_lock(journal_t *journal) {
    spinlock_lock(&journal->lock);
}

static void journal_unlock(journal_t *journal) {
    spinlock_unlock(&journal->lock);
}

static void bytes_zero(void *memory, size_t length) {
    memset(memory, 0, length);
}

static void bytes_copy(void *destination, const void *source, size_t length) {
    memmove(destination, source, length);
}

static uint32_t journal_crc(const uint8_t *block) {
    return crc32_ieee_zeroed(block, JOURNAL_BLOCK_SIZE,
                            __builtin_offsetof(journal_disk_header_t, checksum));
}

static bool sequence_newer(uint64_t left, uint64_t right) {
    return left != right && (int64_t)(left - right) > 0;
}

static bool journal_find_commit(const journal_t *journal, bool after,
                                uint64_t floor, journal_disk_header_t *result,
                                uint32_t *result_slot) {
    bool found = false;
    if (journal == 0 || result == 0 || result_slot == 0) return false;
    for (uint32_t slot = 0; slot < journal->block_count; ++slot) {
        uint8_t block[JOURNAL_BLOCK_SIZE];
        journal_disk_header_t header;
        if (journal_read_slot(journal, slot, block) != K_OK ||
            !journal_decode(block, &header) || header.kind != JOURNAL_KIND_COMMIT ||
            (after && !sequence_newer(header.sequence, floor))) continue;
        /* 日志窗口内序号不会跨越有符号整数的一半，故可用序号比较排序。 */
        if (!found || sequence_newer(result->sequence, header.sequence)) {
            *result = header;
            *result_slot = slot;
            found = true;
        }
    }
    return found;
}

static kstatus_t journal_read_slot(const journal_t *journal, uint32_t slot,
                                   uint8_t block[JOURNAL_BLOCK_SIZE]) {
    if (journal == 0 || slot >= journal->block_count ||
        journal->storage.read == 0) return K_EINVAL;
    return journal->storage.read(journal->storage.context,
                                 journal->start_block + slot,
                                 block, JOURNAL_BLOCK_SIZE);
}

static bool journal_decode(const uint8_t block[JOURNAL_BLOCK_SIZE],
                           journal_disk_header_t *header) {
    bytes_copy(header, block, sizeof(*header));
    if (header->magic != JOURNAL_MAGIC ||
        (header->kind != JOURNAL_KIND_BEGIN && header->kind != JOURNAL_KIND_DATA &&
         header->kind != JOURNAL_KIND_COMMIT) ||
        header->length > JOURNAL_DATA_LIMIT ||
        (header->kind != JOURNAL_KIND_DATA && header->length != 0U)) return false;
    return journal_crc(block) == header->checksum;
}

static kstatus_t journal_write_slot(journal_t *journal, uint32_t slot,
                                    uint64_t sequence, uint32_t kind,
                                    uint64_t target, const void *data,
                                    size_t length) {
    if (journal == 0 || slot >= journal->block_count ||
        journal->storage.write == 0 || length > JOURNAL_DATA_LIMIT ||
        (length != 0 && data == 0)) return K_EINVAL;
    uint8_t block[JOURNAL_BLOCK_SIZE];
    bytes_zero(block, sizeof(block));
    journal_disk_header_t header = {
        .magic = JOURNAL_MAGIC,
        .sequence = sequence,
        .kind = kind,
        .target = target,
        .length = (uint32_t)length,
        .checksum = 0,
    };
    bytes_copy(block, &header, sizeof(header));
    if (length != 0) bytes_copy(block + sizeof(header), data, length);
    header.checksum = journal_crc(block);
    bytes_copy(block, &header, sizeof(header));
    kstatus_t status = journal->storage.write(journal->storage.context,
                                               journal->start_block + slot,
                                               block, sizeof(block));
    return status;
}

static uint32_t journal_next_slot(const journal_t *journal, uint32_t slot) {
    return slot + 1U == journal->block_count ? 0U : slot + 1U;
}

static bool journal_block_range_valid(const journal_block_backend_t *backend,
                                      uint64_t block) {
    return backend != 0 && backend->device != 0 &&
           block < backend->block_count &&
           backend->start_lba <= UINT64_MAX - block;
}

static kstatus_t journal_submit_wait(io_request_t *request) {
    kstatus_t status;
    if (request == 0) return K_EINVAL;
    status = io_submit(request);
    if (status != K_OK) return status;
    for (uint32_t spin = 0U;
         spin < 10000000U &&
         !io_request_is_terminal(request);
         ++spin) {
        (void)nvme_schedule_deferred_poll();
        (void)deferred_run(8U);
        __asm__ volatile ("pause");
    }
    if (!io_request_is_terminal(request)) {
        (void)io_cancel(request);
        /* 取消可能与设备完成并发，返回前必须等最终化摘掉所有链表。 */
        while (!io_request_is_terminal(request)) {
            (void)nvme_schedule_deferred_poll();
            (void)deferred_run(8U);
            __asm__ volatile ("pause");
        }
        return K_ETIMEDOUT;
    }
    return request->status;
}

static kstatus_t journal_block_submit(journal_block_backend_t *backend,
                                      uint64_t block, uint32_t opcode,
                                      uint32_t bio_op, void *buffer) {
    if (!journal_block_range_valid(backend, block) ||
        (opcode != IO_READ && opcode != IO_WRITE && opcode != IO_FLUSH) ||
        (bio_op != BIO_OP_READ && bio_op != BIO_OP_WRITE && bio_op != BIO_OP_FLUSH)) {
        return K_EINVAL;
    }
    page_t *page = 0;
    void *page_memory = 0;
    io_vec_t vector = {0};
    bio_vec_t bio_vector = {0};
    if (bio_op != BIO_OP_FLUSH) {
        if (buffer == 0) return K_EINVAL;
        /*
         * 日志 I/O 恰好为一个 512 字节扇区。写入会映射完整页面供设备读取，因此复制扇区后
         * 要保留清零的尾部。读取只授予设备写权限（或在没有 IOMMU 时发起单扇区 NVMe 读取），
         * 设备覆盖扇区前无需清零。下方完成长度检查会阻止暴露短读结果。
         */
        page = page_alloc(0, opcode == IO_WRITE ?
                          PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32 : PAGE_ALLOC_DMA32);
        if (page == 0) return K_ENOMEM;
        page_memory = phys_to_direct(page_to_phys(page));
        if (page_memory == 0) {
            page_free(page);
            return K_EIO;
        }
        if (opcode == IO_WRITE) bytes_copy(page_memory, buffer, JOURNAL_BLOCK_SIZE);
        vector.base = page_memory;
        vector.length = JOURNAL_BLOCK_SIZE;
        bio_vector.page = page;
        bio_vector.offset = 0;
        bio_vector.length = JOURNAL_BLOCK_SIZE;
    }

    io_request_t request;
    io_request_init(&request, opcode, backend->device, 0,
                    bio_op == BIO_OP_FLUSH ? 0 : &vector,
                    bio_op == BIO_OP_FLUSH ? 0U : 1U);
    bio_t bio = {0};
    bio.lba = backend->start_lba + block;
    bio.op = bio_op;
    bio.vecs = bio_op == BIO_OP_FLUSH ? 0 : &bio_vector;
    bio.vec_count = bio_op == BIO_OP_FLUSH ? 0U : 1U;
    bio.io = &request;
    request.completion_target = &bio;
    kstatus_t status = journal_submit_wait(&request);
    if (status == K_OK && bio_op != BIO_OP_FLUSH &&
        request.bytes_done != JOURNAL_BLOCK_SIZE) status = K_EIO;
    if (status == K_OK && opcode == IO_READ) bytes_copy(buffer, page_memory,
                                                         JOURNAL_BLOCK_SIZE);
    if (page != 0) page_free(page);
    return status;
}

static kstatus_t journal_block_read(void *context, uint64_t block,
                                    void *buffer, size_t length) {
    if (length != JOURNAL_BLOCK_SIZE) return K_EINVAL;
    return journal_block_submit((journal_block_backend_t *)context, block,
                                IO_READ, BIO_OP_READ, buffer);
}

static kstatus_t journal_block_write(void *context, uint64_t block,
                                     const void *buffer, size_t length) {
    if (length != JOURNAL_BLOCK_SIZE) return K_EINVAL;
    return journal_block_submit((journal_block_backend_t *)context, block,
                                IO_WRITE, BIO_OP_WRITE, (void *)buffer);
}

static kstatus_t journal_block_flush(void *context) {
    journal_block_backend_t *backend = (journal_block_backend_t *)context;
    if (backend == 0 || backend->device == 0) return K_EINVAL;
    io_request_t request;
    io_request_init(&request, IO_FLUSH, backend->device, 0, 0, 0);
    bio_t bio = {0};
    bio.op = BIO_OP_FLUSH;
    bio.io = &request;
    request.completion_target = &bio;
    return journal_submit_wait(&request);
}

bool journal_block_storage_init(journal_storage_t *storage,
                                journal_block_backend_t *backend) {
    if (storage == 0 || !journal_block_range_valid(backend, 0) ||
        backend->block_count < 4U) return false;
    storage->read = journal_block_read;
    storage->write = journal_block_write;
    storage->flush = journal_block_flush;
    storage->context = backend;
    return true;
}

static kstatus_t journal_noop_apply(void *context, uint64_t target,
                                    const void *data, size_t length) {
    (void)context;
    (void)target;
    (void)data;
    (void)length;
    return K_OK;
}

bool journal_block_storage_self_test(device_t *device, uint64_t start_lba,
                                     uint32_t block_count) {
    journal_block_backend_t backend = {
        .device = device,
        .start_lba = start_lba,
        .block_count = block_count,
    };
    journal_storage_t storage = {0};
    journal_t journal;
    if (!journal_block_storage_init(&storage, &backend) ||
        journal_init(&journal, &storage, 0, block_count) != K_OK ||
        journal_replay(&journal, journal_noop_apply, 0) != K_OK) return false;
    return true;
}

kstatus_t journal_init(journal_t *journal, const journal_storage_t *storage,
                       uint64_t start_block, uint32_t block_count) {
    if (journal == 0 || storage == 0 || storage->read == 0 ||
        storage->write == 0 || storage->flush == 0 || block_count < 4U) {
        return K_EINVAL;
    }
    bytes_zero(journal, sizeof(*journal));
    journal->storage = *storage;
    journal->start_block = start_block;
    journal->block_count = block_count;
    journal->next_slot = 0;
    journal->next_sequence = 1U;
    spinlock_init(&journal->lock);

    uint64_t latest_sequence = 0;
    uint32_t latest_slot = 0;
    bool found_commit = false;
    for (uint32_t slot = 0; slot < block_count; ++slot) {
        uint8_t block[JOURNAL_BLOCK_SIZE];
        journal_disk_header_t header;
        if (journal_read_slot(journal, slot, block) != K_OK ||
            !journal_decode(block, &header) || header.kind != JOURNAL_KIND_COMMIT) {
            continue;
        }
        if (!found_commit || sequence_newer(header.sequence, latest_sequence)) {
            found_commit = true;
            latest_sequence = header.sequence;
            latest_slot = slot;
        }
    }
    if (found_commit) {
        journal->next_slot = journal_next_slot(journal, latest_slot);
        journal->next_sequence = latest_sequence + 1U;
        if (journal->next_sequence == 0) journal->next_sequence = 1U;
    }
    return K_OK;
}

kstatus_t journal_begin(journal_t *journal, journal_transaction_t *transaction) {
    if (journal == 0 || transaction == 0 || transaction->active || journal->active) {
        return K_EBUSY;
    }
    journal_lock(journal);
    if (journal->active) {
        journal_unlock(journal);
        return K_EBUSY;
    }
    kstatus_t status = journal_write_slot(journal, journal->next_slot,
                                          journal->next_sequence,
                                          JOURNAL_KIND_BEGIN, 0, 0, 0);
    if (status == K_OK) status = journal->storage.flush(journal->storage.context);
    if (status == K_OK) {
        journal->active = true;
        journal->active_sequence = journal->next_sequence;
        journal->active_records = 0;
        journal->next_slot = journal_next_slot(journal, journal->next_slot);
        transaction->journal = journal;
        transaction->sequence = journal->active_sequence;
        transaction->active = true;
    }
    journal_unlock(journal);
    return status;
}

kstatus_t journal_record(journal_transaction_t *transaction, uint64_t target,
                         const void *data, size_t length) {
    if (transaction == 0 || !transaction->active || transaction->journal == 0 ||
        data == 0 || length == 0 || length > JOURNAL_DATA_LIMIT) return K_EINVAL;
    journal_t *journal = transaction->journal;
    journal_lock(journal);
    if (!journal->active || journal->active_sequence != transaction->sequence ||
        journal->active_records + 2U >= journal->block_count) {
        journal_unlock(journal);
        return K_EBUSY;
    }
    kstatus_t status = journal_write_slot(journal, journal->next_slot,
                                          transaction->sequence, JOURNAL_KIND_DATA,
                                          target, data, length);
    if (status == K_OK) {
        ++journal->active_records;
        journal->next_slot = journal_next_slot(journal, journal->next_slot);
    }
    journal_unlock(journal);
    return status;
}

kstatus_t journal_commit(journal_transaction_t *transaction) {
    if (transaction == 0 || !transaction->active || transaction->journal == 0) {
        return K_EINVAL;
    }
    journal_t *journal = transaction->journal;
    journal_lock(journal);
    if (!journal->active || journal->active_sequence != transaction->sequence) {
        journal_unlock(journal);
        return K_EBUSY;
    }
    kstatus_t status = journal_write_slot(journal, journal->next_slot,
                                          transaction->sequence,
                                          JOURNAL_KIND_COMMIT, 0, 0, 0);
    if (status == K_OK) status = journal->storage.flush(journal->storage.context);
    if (status == K_OK) {
        journal->active = false;
        journal->active_records = 0;
        journal->next_slot = journal_next_slot(journal, journal->next_slot);
        journal->next_sequence = transaction->sequence + 1U;
        if (journal->next_sequence == 0) journal->next_sequence = 1U;
        transaction->active = false;
    }
    journal_unlock(journal);
    return status;
}

kstatus_t journal_replay(journal_t *journal, journal_apply_record_t apply,
                         void *context) {
    if (journal == 0 || apply == 0 || journal->block_count < 4U) return K_EINVAL;
    bool after = false;
    uint64_t floor = 0;
    journal_disk_header_t latest_commit = {0};
    uint32_t latest_commit_slot = 0;
    journal_lock(journal);
    bool have_latest = false;
    for (uint32_t slot = 0; slot < journal->block_count; ++slot) {
        uint8_t block[JOURNAL_BLOCK_SIZE];
        journal_disk_header_t header;
        if (journal_read_slot(journal, slot, block) != K_OK ||
            !journal_decode(block, &header) || header.kind != JOURNAL_KIND_COMMIT) {
            continue;
        }
        if (!have_latest || sequence_newer(header.sequence, latest_commit.sequence)) {
            latest_commit = header;
            latest_commit_slot = slot;
            have_latest = true;
        }
    }
    journal_unlock(journal);
    (void)latest_commit_slot;
    if (!have_latest) return K_OK;
    /* 一个事务至少包含 BEGIN 和 COMMIT；遍历 block_count 次足以消费全部提交。 */
    for (uint32_t processed = 0; processed < journal->block_count; ++processed) {
        journal_disk_header_t commit = {0};
        uint32_t commit_slot = 0;
        journal_lock(journal);
        bool found_commit = journal_find_commit(journal, after, floor,
                                                &commit, &commit_slot);
        if (!found_commit) {
            journal_unlock(journal);
            break;
        }
        uint32_t begin_slot = commit_slot;
        bool found_begin = false;
        for (uint32_t distance = 1U; distance <= journal->block_count; ++distance) {
            uint32_t slot = commit_slot >= distance ? commit_slot - distance :
                             journal->block_count + commit_slot - distance;
            uint8_t block[JOURNAL_BLOCK_SIZE];
            journal_disk_header_t header;
            if (journal_read_slot(journal, slot, block) != K_OK ||
                !journal_decode(block, &header)) continue;
            if (header.sequence == commit.sequence &&
                header.kind == JOURNAL_KIND_BEGIN) {
                begin_slot = slot;
                found_begin = true;
                break;
            }
        }
        journal_unlock(journal);
        /* 环形日志中的旧提交可能只留下 COMMIT；它已被更新事务覆盖，跳过即可。
         * 最新提交若缺 BEGIN，则说明掉电或介质损坏破坏了当前事务。 */
        if (!found_begin) {
            if (commit.sequence == latest_commit.sequence) return K_EIO;
            floor = commit.sequence;
            after = true;
            continue;
        }

        uint32_t slot = journal_next_slot(journal, begin_slot);
        while (slot != commit_slot) {
            uint8_t block[JOURNAL_BLOCK_SIZE];
            journal_disk_header_t header;
            if (journal_read_slot(journal, slot, block) != K_OK ||
                !journal_decode(block, &header)) return K_EIO;
            if (header.sequence == commit.sequence &&
                header.kind == JOURNAL_KIND_DATA) {
                kstatus_t status = apply(context, header.target,
                                         block + sizeof(header), header.length);
                if (status != K_OK) return status;
            }
            slot = journal_next_slot(journal, slot);
        }
        floor = commit.sequence;
        after = true;
    }
    return K_OK;
}

typedef struct journal_memory_disk {
    uint8_t volatile_blocks[8][JOURNAL_BLOCK_SIZE];
    uint8_t durable_blocks[8][JOURNAL_BLOCK_SIZE];
    uint8_t recovered[JOURNAL_DATA_LIMIT];
    uint32_t recovered_count;
} journal_memory_disk_t;

static kstatus_t memory_read(void *context, uint64_t block, void *buffer, size_t length) {
    journal_memory_disk_t *disk = (journal_memory_disk_t *)context;
    if (disk == 0 || block >= 8U || length != JOURNAL_BLOCK_SIZE) return K_EINVAL;
    bytes_copy(buffer, disk->volatile_blocks[block], length);
    return K_OK;
}

static kstatus_t memory_write(void *context, uint64_t block,
                              const void *buffer, size_t length) {
    journal_memory_disk_t *disk = (journal_memory_disk_t *)context;
    if (disk == 0 || block >= 8U || length != JOURNAL_BLOCK_SIZE) return K_EINVAL;
    bytes_copy(disk->volatile_blocks[block], buffer, length);
    return K_OK;
}

static kstatus_t memory_flush(void *context) {
    journal_memory_disk_t *disk = (journal_memory_disk_t *)context;
    if (disk == 0) return K_EINVAL;
    bytes_copy(disk->durable_blocks, disk->volatile_blocks,
               sizeof(disk->durable_blocks));
    return K_OK;
}

static kstatus_t memory_apply(void *context, uint64_t target,
                              const void *data, size_t length) {
    journal_memory_disk_t *disk = (journal_memory_disk_t *)context;
    if (disk == 0 || target != 42U || length > sizeof(disk->recovered)) return K_EINVAL;
    bytes_copy(disk->recovered, data, length);
    ++disk->recovered_count;
    return K_OK;
}

bool journal_self_test(void) {
    journal_memory_disk_t disk;
    journal_t journal;
    journal_transaction_t transaction = {0};
    journal_storage_t storage = {
        .read = memory_read,
        .write = memory_write,
        .flush = memory_flush,
        .context = &disk,
    };
    const uint8_t value[] = {'j', 'o', 'u', 'r', 'n', 'a', 'l'};
    bytes_zero(&disk, sizeof(disk));
    if (journal_init(&journal, &storage, 0, 8) != K_OK ||
        journal_begin(&journal, &transaction) != K_OK ||
        journal_record(&transaction, 42U, value, sizeof(value)) != K_OK) return false;
    /* 未提交事务不能在掉电后修改文件系统状态。 */
    bytes_copy(disk.volatile_blocks, disk.durable_blocks, sizeof(disk.durable_blocks));
    if (journal_init(&journal, &storage, 0, 8) != K_OK ||
        journal_replay(&journal, memory_apply, &disk) != K_OK) return false;
    for (size_t i = 0; i < sizeof(value); ++i) if (disk.recovered[i] != 0) return false;
    if (disk.recovered_count != 0U) return false;

    /* 掉电后事务句柄不再有效，模拟重新创建事务。 */
    bytes_zero(&transaction, sizeof(transaction));
    if (journal_begin(&journal, &transaction) != K_OK ||
        journal_record(&transaction, 42U, value, sizeof(value)) != K_OK ||
        journal_commit(&transaction) != K_OK) return false;
    bytes_copy(disk.volatile_blocks, disk.durable_blocks, sizeof(disk.durable_blocks));
    bytes_zero(disk.recovered, sizeof(disk.recovered));
    if (journal_init(&journal, &storage, 0, 8) != K_OK ||
        journal_replay(&journal, memory_apply, &disk) != K_OK) return false;
    for (size_t i = 0; i < sizeof(value); ++i) if (disk.recovered[i] != value[i]) return false;
    if (disk.recovered_count != 1U) return false;

    /* 两笔已提交事务都必须被重放，不能只取最后一笔提交记录。 */
    const uint8_t second_value[] = {'r', 'e', 'p', 'l', 'a', 'y'};
    bytes_zero(&transaction, sizeof(transaction));
    if (journal_begin(&journal, &transaction) != K_OK ||
        journal_record(&transaction, 42U, second_value, sizeof(second_value)) != K_OK ||
        journal_commit(&transaction) != K_OK) return false;
    bytes_copy(disk.volatile_blocks, disk.durable_blocks, sizeof(disk.durable_blocks));
    bytes_zero(disk.recovered, sizeof(disk.recovered));
    disk.recovered_count = 0U;
    if (journal_init(&journal, &storage, 0, 8) != K_OK ||
        journal_replay(&journal, memory_apply, &disk) != K_OK ||
        disk.recovered_count != 2U) return false;
    for (size_t i = 0; i < sizeof(second_value); ++i) {
        if (disk.recovered[i] != second_value[i]) return false;
    }

    uint32_t corrupted_commits = 0U;
    for (uint32_t slot = 0; slot < 8U; ++slot) {
        journal_disk_header_t header;
        bytes_copy(&header, disk.durable_blocks[slot], sizeof(header));
        if (header.magic == JOURNAL_MAGIC && header.kind == JOURNAL_KIND_COMMIT) {
            disk.durable_blocks[slot][32] ^= 0x01U;
            ++corrupted_commits;
        }
    }
    bytes_copy(disk.volatile_blocks, disk.durable_blocks, sizeof(disk.durable_blocks));
    bytes_zero(disk.recovered, sizeof(disk.recovered));
    disk.recovered_count = 0U;
    return journal_init(&journal, &storage, 0, 8) == K_OK &&
           journal_replay(&journal, memory_apply, &disk) == K_OK &&
           corrupted_commits == 2U && disk.recovered_count == 0U &&
           disk.recovered[0] == 0;
}
