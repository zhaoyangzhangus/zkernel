#include <kernel/litefs.h>
#include <kernel/crc32.h>
#include <kernel/string.h>
#include <kernel/kmem.h>

#define LITEFS_INODE_USED 1U
#define LITEFS_INODES_PER_BLOCK (JOURNAL_BLOCK_SIZE / sizeof(litefs_inode_disk_t))

static void litefs_zero(void *destination, size_t length) {
    memset(destination, 0, length);
}

static void litefs_copy(void *destination, const void *source, size_t length) {
    memmove(destination, source, length);
}

static size_t litefs_string_length(const char *text) {
    size_t length = 0;
    if (text == 0) return 0;
    while (length < LITEFS_NAME_LIMIT && text[length] != 0) ++length;
    return length;
}

static bool litefs_string_equal(const char *left, const char *right,
                                size_t length) {
    for (size_t i = 0; i < length; ++i) if (left[i] != right[i]) return false;
    return left[length] == 0 && right[length] == 0;
}

static uint32_t litefs_crc(const uint8_t *bytes, size_t length,
                           size_t checksum_offset) {
    return crc32_ieee_zeroed(bytes, length, checksum_offset);
}

static void litefs_lock(litefs_t *filesystem) {
    spinlock_lock(&filesystem->lock);
}

static void litefs_unlock(litefs_t *filesystem) {
    spinlock_unlock(&filesystem->lock);
}

static bool litefs_storage_valid(const journal_storage_t *storage) {
    return storage != 0 && storage->read != 0 && storage->write != 0 &&
           storage->flush != 0;
}

static kstatus_t litefs_read_block(const journal_storage_t *storage,
                                   uint64_t block, void *buffer) {
    if (!litefs_storage_valid(storage) || buffer == 0) return K_EINVAL;
    return storage->read(storage->context, block, buffer, JOURNAL_BLOCK_SIZE);
}

static kstatus_t litefs_write_block(const journal_storage_t *storage,
                                    uint64_t block, const void *buffer) {
    if (!litefs_storage_valid(storage) || buffer == 0) return K_EINVAL;
    return storage->write(storage->context, block, buffer, JOURNAL_BLOCK_SIZE);
}

static bool litefs_layout_valid(uint32_t total_blocks) {
    return total_blocks >= LITEFS_MIN_BLOCKS &&
           LITEFS_DATA_START <= total_blocks &&
           LITEFS_DATA_START + LITEFS_MAX_INODES * LITEFS_FILE_EXTENT_BLOCKS <=
               total_blocks;
}

static uint32_t litefs_super_checksum(const litefs_super_disk_t *super) {
    return litefs_crc((const uint8_t *)super, sizeof(*super),
                      __builtin_offsetof(litefs_super_disk_t, checksum));
}

static uint32_t litefs_inode_checksum(const litefs_inode_disk_t *inode) {
    return litefs_crc((const uint8_t *)inode, sizeof(*inode),
                      __builtin_offsetof(litefs_inode_disk_t, checksum));
}

static void litefs_encode_super(litefs_super_disk_t *super, uint32_t total_blocks) {
    litefs_zero(super, sizeof(*super));
    super->magic = LITEFS_MAGIC;
    super->version = LITEFS_VERSION;
    super->block_size = JOURNAL_BLOCK_SIZE;
    super->total_blocks = total_blocks;
    super->journal_start = LITEFS_JOURNAL_START;
    super->journal_blocks = LITEFS_JOURNAL_BLOCKS;
    super->inode_start = LITEFS_INODE_START;
    super->inode_blocks = LITEFS_INODE_BLOCKS;
    super->data_start = LITEFS_DATA_START;
    super->inode_count = LITEFS_MAX_INODES;
    super->generation = 1U;
    super->checksum = litefs_super_checksum(super);
}

static void litefs_encode_inode(const litefs_inode_t *source,
                                litefs_inode_disk_t *destination) {
    litefs_zero(destination, sizeof(*destination));
    destination->flags = source->flags;
    destination->mode = source->mode;
    destination->inode_id = source->inode_id;
    destination->size = source->size;
    destination->extent_start = source->extent_start;
    destination->extent_blocks = source->extent_blocks;
    destination->name_length = source->name_length;
    litefs_copy(destination->name, source->name, sizeof(destination->name));
    destination->checksum = litefs_inode_checksum(destination);
}

static bool litefs_decode_inode(const litefs_inode_disk_t *source,
                                litefs_inode_t *destination) {
    if (source->flags == 0U) {
        litefs_zero(destination, sizeof(*destination));
        return source->checksum == 0U;
    }
    if ((source->flags & LITEFS_INODE_USED) == 0U ||
        source->name_length == 0U || source->name_length >= LITEFS_NAME_LIMIT ||
        source->extent_blocks != LITEFS_FILE_EXTENT_BLOCKS ||
        source->size > source->extent_blocks * JOURNAL_BLOCK_SIZE ||
        source->checksum != litefs_inode_checksum(source)) return false;
    litefs_zero(destination, sizeof(*destination));
    destination->flags = source->flags;
    destination->mode = source->mode;
    destination->inode_id = source->inode_id;
    destination->size = source->size;
    destination->extent_start = source->extent_start;
    destination->extent_blocks = source->extent_blocks;
    destination->name_length = source->name_length;
    litefs_copy(destination->name, source->name, sizeof(destination->name));
    destination->name[LITEFS_NAME_LIMIT - 1U] = 0;
    return destination->name[destination->name_length] == 0;
}

static bool litefs_inode_identity_valid(const litefs_t *filesystem,
                                        uint32_t inode_index,
                                        const litefs_inode_t *inode) {
    if (filesystem == 0 || inode == 0 || inode_index >= LITEFS_MAX_INODES) {
        return false;
    }
    if ((inode->flags & LITEFS_INODE_USED) == 0U) return true;
    return inode->inode_id == (uint64_t)inode_index + 1U &&
           inode->extent_start == LITEFS_DATA_START +
                                   (uint64_t)inode_index *
                                   LITEFS_FILE_EXTENT_BLOCKS &&
           inode->extent_blocks == LITEFS_FILE_EXTENT_BLOCKS &&
           inode->extent_start + inode->extent_blocks <= filesystem->total_blocks;
}

static void litefs_inode_location(uint32_t inode_index, uint64_t *block,
                                  uint32_t *offset) {
    *block = LITEFS_INODE_START + inode_index / LITEFS_INODES_PER_BLOCK;
    *offset = (inode_index % LITEFS_INODES_PER_BLOCK) * sizeof(litefs_inode_disk_t);
}

static uint64_t litefs_inode_target(uint32_t inode_index) {
    uint64_t block = 0;
    uint32_t offset = 0;
    litefs_inode_location(inode_index, &block, &offset);
    return (block << 32) | offset;
}

static kstatus_t litefs_apply_record(void *context, uint64_t target,
                                     const void *data, size_t length) {
    litefs_t *filesystem = (litefs_t *)context;
    uint64_t block = target >> 32;
    uint32_t offset = (uint32_t)target;
    uint8_t home[JOURNAL_BLOCK_SIZE];
    litefs_inode_disk_t disk_inode;
    litefs_inode_t inode;
    uint32_t inode_index;
    if (filesystem == 0 || data == 0 || length == 0 ||
        offset > JOURNAL_BLOCK_SIZE || length > JOURNAL_BLOCK_SIZE - offset ||
        block < LITEFS_INODE_START || block >= LITEFS_INODE_START + LITEFS_INODE_BLOCKS ||
        length != sizeof(disk_inode) ||
        (offset % sizeof(disk_inode)) != 0U) return K_EIO;
    inode_index = (uint32_t)((block - LITEFS_INODE_START) *
                             LITEFS_INODES_PER_BLOCK +
                             offset / sizeof(disk_inode));
    if (inode_index >= LITEFS_MAX_INODES) return K_EIO;
    litefs_copy(&disk_inode, data, sizeof(disk_inode));
    if (!litefs_decode_inode(&disk_inode, &inode) ||
        !litefs_inode_identity_valid(filesystem, inode_index, &inode) ||
        litefs_read_block(&filesystem->storage, block, home) != K_OK) return K_EIO;
    litefs_copy(home + offset, data, length);
    return litefs_write_block(&filesystem->storage, block, home);
}

static kstatus_t litefs_journal_inode(litefs_t *filesystem, uint32_t inode_index,
                                      const litefs_inode_t *inode) {
    litefs_inode_disk_t disk_inode;
    journal_transaction_t transaction = {0};
    litefs_encode_inode(inode, &disk_inode);
    kstatus_t status = journal_begin(&filesystem->journal, &transaction);
    if (status != K_OK) return status;
    status = journal_record(&transaction, litefs_inode_target(inode_index),
                            &disk_inode, sizeof(disk_inode));
    if (status == K_OK) status = journal_commit(&transaction);
    return status;
}

static kstatus_t litefs_store_inode(litefs_t *filesystem, uint32_t inode_index,
                                    const litefs_inode_t *inode) {
    uint64_t block = 0;
    uint32_t offset = 0;
    uint8_t home[JOURNAL_BLOCK_SIZE];
    litefs_inode_disk_t disk_inode;
    litefs_inode_location(inode_index, &block, &offset);
    litefs_encode_inode(inode, &disk_inode);
    kstatus_t status = litefs_journal_inode(filesystem, inode_index, inode);
    if (status != K_OK) return status;
    status = litefs_read_block(&filesystem->storage, block, home);
    if (status == K_OK) {
        litefs_copy(home + offset, &disk_inode, sizeof(disk_inode));
        status = litefs_write_block(&filesystem->storage, block, home);
    }
    if (status == K_OK) status = filesystem->storage.flush(filesystem->storage.context);
    return status;
}

static kstatus_t litefs_load_super(litefs_t *filesystem) {
    uint8_t block[JOURNAL_BLOCK_SIZE];
    if (litefs_read_block(&filesystem->storage, 0, block) != K_OK) return K_EIO;
    litefs_copy(&filesystem->super, block, sizeof(filesystem->super));
    if (filesystem->super.magic != LITEFS_MAGIC ||
        filesystem->super.version != LITEFS_VERSION ||
        filesystem->super.block_size != JOURNAL_BLOCK_SIZE ||
        filesystem->super.total_blocks != filesystem->total_blocks ||
        filesystem->super.journal_start != LITEFS_JOURNAL_START ||
        filesystem->super.journal_blocks != LITEFS_JOURNAL_BLOCKS ||
        filesystem->super.inode_start != LITEFS_INODE_START ||
        filesystem->super.inode_blocks != LITEFS_INODE_BLOCKS ||
        filesystem->super.data_start != LITEFS_DATA_START ||
        filesystem->super.inode_count != LITEFS_MAX_INODES ||
        filesystem->super.checksum != litefs_super_checksum(&filesystem->super)) {
        return K_EIO;
    }
    return K_OK;
}

static kstatus_t litefs_load_inodes(litefs_t *filesystem) {
    uint8_t block[JOURNAL_BLOCK_SIZE];
    litefs_inode_disk_t disk_inode;
    uint64_t loaded_block = UINT64_MAX;
    for (uint32_t index = 0; index < LITEFS_MAX_INODES; ++index) {
        uint64_t block_number = 0;
        uint32_t offset = 0;
        litefs_inode_location(index, &block_number, &offset);
        if (block_number != loaded_block) {
            if (litefs_read_block(&filesystem->storage, block_number, block) != K_OK) {
                return K_EIO;
            }
            loaded_block = block_number;
        }
        litefs_copy(&disk_inode, block + offset, sizeof(disk_inode));
        if (!litefs_decode_inode(&disk_inode, &filesystem->inodes[index]) ||
            !litefs_inode_identity_valid(filesystem, index,
                                         &filesystem->inodes[index])) return K_EIO;
    }
    return K_OK;
}

kstatus_t litefs_format(const journal_storage_t *storage, uint32_t total_blocks) {
    uint8_t block[JOURNAL_BLOCK_SIZE];
    litefs_super_disk_t super;
    if (!litefs_storage_valid(storage) || !litefs_layout_valid(total_blocks)) return K_EINVAL;
    litefs_zero(block, sizeof(block));
    for (uint32_t index = 0; index < total_blocks; ++index) {
        if (litefs_write_block(storage, index, block) != K_OK) return K_EIO;
    }
    litefs_encode_super(&super, total_blocks);
    litefs_copy(block, &super, sizeof(super));
    if (litefs_write_block(storage, 0, block) != K_OK ||
        storage->flush(storage->context) != K_OK) return K_EIO;
    return K_OK;
}

kstatus_t litefs_mount(litefs_t *filesystem, const journal_storage_t *storage,
                       uint32_t total_blocks) {
    if (filesystem == 0 || !litefs_storage_valid(storage) ||
        !litefs_layout_valid(total_blocks)) return K_EINVAL;
    litefs_zero(filesystem, sizeof(*filesystem));
    filesystem->storage = *storage;
    filesystem->total_blocks = total_blocks;
    spinlock_init(&filesystem->lock);
    if (litefs_load_super(filesystem) != K_OK ||
        journal_init(&filesystem->journal, &filesystem->storage,
                     LITEFS_JOURNAL_START, LITEFS_JOURNAL_BLOCKS) != K_OK ||
        journal_replay(&filesystem->journal, litefs_apply_record, filesystem) != K_OK ||
        filesystem->storage.flush(filesystem->storage.context) != K_OK ||
        litefs_load_inodes(filesystem) != K_OK) return K_EIO;
    filesystem->mounted = true;
    return K_OK;
}

static int32_t litefs_find_inode(const litefs_t *filesystem, const char *name) {
    for (uint32_t index = 0; index < LITEFS_MAX_INODES; ++index) {
        const litefs_inode_t *inode = &filesystem->inodes[index];
        if ((inode->flags & LITEFS_INODE_USED) != 0U &&
            litefs_string_equal(inode->name, name, inode->name_length)) {
            return (int32_t)index;
        }
    }
    return -1;
}

kstatus_t litefs_create_file(litefs_t *filesystem, const char *name,
                             uint32_t mode, uint32_t *inode_index) {
    uint8_t zero[JOURNAL_BLOCK_SIZE];
    if (filesystem == 0 || !filesystem->mounted || name == 0 || inode_index == 0) {
        return K_EINVAL;
    }
    size_t length = litefs_string_length(name);
    if (length == 0 || length >= LITEFS_NAME_LIMIT ||
        litefs_find_inode(filesystem, name) >= 0) return K_EINVAL;
    litefs_lock(filesystem);
    uint32_t index = LITEFS_MAX_INODES;
    for (uint32_t candidate = 0; candidate < LITEFS_MAX_INODES; ++candidate) {
        if ((filesystem->inodes[candidate].flags & LITEFS_INODE_USED) == 0U) {
            index = candidate;
            break;
        }
    }
    if (index == LITEFS_MAX_INODES) {
        litefs_unlock(filesystem);
        return K_ENOMEM;
    }
    litefs_inode_t inode;
    litefs_zero(&inode, sizeof(inode));
    inode.flags = LITEFS_INODE_USED;
    inode.mode = mode;
    inode.inode_id = index + 1U;
    inode.extent_start = LITEFS_DATA_START +
                         index * LITEFS_FILE_EXTENT_BLOCKS;
    inode.extent_blocks = LITEFS_FILE_EXTENT_BLOCKS;
    inode.name_length = (uint32_t)length;
    litefs_copy(inode.name, name, length);
    litefs_zero(zero, sizeof(zero));
    kstatus_t status = K_OK;
    for (uint64_t block = 0; block < inode.extent_blocks; ++block) {
        status = litefs_write_block(&filesystem->storage,
                                    inode.extent_start + block, zero);
        if (status != K_OK) break;
    }
    if (status == K_OK) status = filesystem->storage.flush(filesystem->storage.context);
    if (status == K_OK) status = litefs_store_inode(filesystem, index, &inode);
    if (status == K_OK) {
        filesystem->inodes[index] = inode;
        *inode_index = index;
    }
    litefs_unlock(filesystem);
    return status;
}

static bool litefs_inode_valid(const litefs_t *filesystem, uint32_t inode_index) {
    return filesystem != 0 && inode_index < LITEFS_MAX_INODES &&
           (filesystem->inodes[inode_index].flags & LITEFS_INODE_USED) != 0U;
}

kstatus_t litefs_read_file(litefs_t *filesystem, uint32_t inode_index,
                           uint64_t offset, void *buffer, size_t length,
                           uint64_t *bytes) {
    uint8_t block[JOURNAL_BLOCK_SIZE];
    if (!litefs_inode_valid(filesystem, inode_index) || bytes == 0 ||
        (buffer == 0 && length != 0)) return K_EINVAL;
    litefs_lock(filesystem);
    const litefs_inode_t *inode = &filesystem->inodes[inode_index];
    if (offset > inode->size) {
        litefs_unlock(filesystem);
        return K_EINVAL;
    }
    uint64_t available = inode->size - offset;
    if (available > length) available = length;
    uint64_t transferred = 0;
    while (transferred < available) {
        uint64_t position = offset + transferred;
        uint64_t block_index = position / JOURNAL_BLOCK_SIZE;
        uint32_t in_block = (uint32_t)(position % JOURNAL_BLOCK_SIZE);
        size_t chunk = JOURNAL_BLOCK_SIZE - in_block;
        if (chunk > available - transferred) chunk = (size_t)(available - transferred);
        kstatus_t status = litefs_read_block(&filesystem->storage,
                                             inode->extent_start + block_index,
                                             block);
        if (status != K_OK) {
            litefs_unlock(filesystem);
            return status;
        }
        litefs_copy((uint8_t *)buffer + transferred, block + in_block, chunk);
        transferred += chunk;
    }
    *bytes = transferred;
    litefs_unlock(filesystem);
    return K_OK;
}

kstatus_t litefs_write_file(litefs_t *filesystem, uint32_t inode_index,
                            uint64_t offset, const void *buffer, size_t length,
                            uint64_t *bytes) {
    uint8_t block[JOURNAL_BLOCK_SIZE];
    if (!litefs_inode_valid(filesystem, inode_index) || bytes == 0 ||
        (buffer == 0 && length != 0)) return K_EINVAL;
    litefs_lock(filesystem);
    litefs_inode_t *inode = &filesystem->inodes[inode_index];
    uint64_t capacity = inode->extent_blocks * JOURNAL_BLOCK_SIZE;
    if (offset > inode->size || offset > UINT64_MAX - length ||
        offset + length > capacity) {
        litefs_unlock(filesystem);
        return K_EINVAL;
    }
    uint64_t transferred = 0;
    kstatus_t status = K_OK;
    while (transferred < length) {
        uint64_t position = offset + transferred;
        uint64_t block_index = position / JOURNAL_BLOCK_SIZE;
        uint32_t in_block = (uint32_t)(position % JOURNAL_BLOCK_SIZE);
        size_t chunk = JOURNAL_BLOCK_SIZE - in_block;
        if (chunk > length - transferred) chunk = length - (size_t)transferred;
        if (in_block != 0U || chunk != JOURNAL_BLOCK_SIZE) {
            status = litefs_read_block(&filesystem->storage,
                                       inode->extent_start + block_index, block);
            if (status != K_OK) break;
        }
        litefs_copy(block + in_block, (const uint8_t *)buffer + transferred, chunk);
        status = litefs_write_block(&filesystem->storage,
                                    inode->extent_start + block_index, block);
        if (status != K_OK) break;
        transferred += chunk;
    }
    uint64_t old_size = inode->size;
    if (status == K_OK) status = filesystem->storage.flush(filesystem->storage.context);
    if (status == K_OK && offset + length > inode->size) inode->size = offset + length;
    if (status == K_OK && inode->size != old_size) {
        status = litefs_store_inode(filesystem, inode_index, inode);
        if (status != K_OK) inode->size = old_size;
    }
    *bytes = status == K_OK ? length : transferred;
    litefs_unlock(filesystem);
    return status;
}

kstatus_t litefs_fsync(litefs_t *filesystem) {
    if (filesystem == 0 || !filesystem->mounted) return K_EINVAL;
    return filesystem->storage.flush(filesystem->storage.context);
}

typedef struct litefs_test_disk {
    uint8_t volatile_blocks[64][JOURNAL_BLOCK_SIZE];
    uint8_t durable_blocks[64][JOURNAL_BLOCK_SIZE];
    uint32_t read_counts[64];
} litefs_test_disk_t;

static litefs_test_disk_t g_litefs_test_disk;
static litefs_t g_litefs_test_filesystem;

static kstatus_t litefs_test_read(void *context, uint64_t block, void *buffer,
                                  size_t length) {
    litefs_test_disk_t *disk = (litefs_test_disk_t *)context;
    if (disk == 0 || buffer == 0 || block >= 64U || length != JOURNAL_BLOCK_SIZE) {
        return K_EINVAL;
    }
    ++disk->read_counts[block];
    litefs_copy(buffer, disk->volatile_blocks[block], length);
    return K_OK;
}

static kstatus_t litefs_test_write(void *context, uint64_t block,
                                   const void *buffer, size_t length) {
    litefs_test_disk_t *disk = (litefs_test_disk_t *)context;
    if (disk == 0 || buffer == 0 || block >= 64U || length != JOURNAL_BLOCK_SIZE) {
        return K_EINVAL;
    }
    litefs_copy(disk->volatile_blocks[block], buffer, length);
    return K_OK;
}

static kstatus_t litefs_test_flush(void *context) {
    litefs_test_disk_t *disk = (litefs_test_disk_t *)context;
    if (disk == 0) return K_EINVAL;
    litefs_copy(disk->durable_blocks, disk->volatile_blocks,
                sizeof(disk->durable_blocks));
    return K_OK;
}

static void litefs_test_power_cycle(litefs_test_disk_t *disk) {
    litefs_copy(disk->volatile_blocks, disk->durable_blocks,
                sizeof(disk->volatile_blocks));
}

bool litefs_self_test(void) {
    static bool completed;
    static const char sample[] = "hello";
    static const char world[] = "world";
    journal_storage_t storage = {
        .read = litefs_test_read,
        .write = litefs_test_write,
        .flush = litefs_test_flush,
        .context = &g_litefs_test_disk,
    };
    litefs_t recovered;
    litefs_inode_t staged;
    uint32_t inode_index = 0;
    uint32_t full_block_inode = 0;
    uint64_t bytes = 0;
    uint64_t full_block_start = 0;
    uint32_t full_block_reads = 0;
    uint8_t result[sizeof(sample) + sizeof(world) - 2U];
    uint8_t saved_super[JOURNAL_BLOCK_SIZE];
    uint8_t saved_inode[JOURNAL_BLOCK_SIZE];
    if (completed) return true;
    litefs_zero(&g_litefs_test_disk, sizeof(g_litefs_test_disk));
    if (litefs_format(&storage, 64U) != K_OK ||
        litefs_mount(&g_litefs_test_filesystem, &storage, 64U) != K_OK ||
        g_litefs_test_disk.read_counts[LITEFS_INODE_START] != 1U ||
        litefs_create_file(&g_litefs_test_filesystem, "sample.txt", 0100666U,
                            &inode_index) != K_OK ||
        litefs_write_file(&g_litefs_test_filesystem, inode_index, 0, sample,
                           sizeof(sample) - 1U, &bytes) != K_OK ||
        bytes != sizeof(sample) - 1U ||
        litefs_fsync(&g_litefs_test_filesystem) != K_OK) return false;

    for (size_t i = 0; i < sizeof(saved_super); ++i) saved_super[i] = (uint8_t)i;
    if (litefs_create_file(&g_litefs_test_filesystem, "block.bin", 0100666U,
                           &full_block_inode) != K_OK) return false;
    full_block_start = g_litefs_test_filesystem.inodes[full_block_inode].extent_start;
    full_block_reads = g_litefs_test_disk.read_counts[full_block_start];
    if (litefs_write_file(&g_litefs_test_filesystem, full_block_inode, 0,
                          saved_super, sizeof(saved_super), &bytes) != K_OK ||
        bytes != sizeof(saved_super) ||
        g_litefs_test_disk.read_counts[full_block_start] != full_block_reads) {
        return false;
    }
    for (size_t i = 0; i < sizeof(saved_super); ++i) {
        if (g_litefs_test_disk.volatile_blocks[full_block_start][i] != saved_super[i]) {
            return false;
        }
    }

    /* 先把追加的数据写入持久盘，再只提交 inode journal，模拟掉电时 home inode 尚未写回。 */
    staged = g_litefs_test_filesystem.inodes[inode_index];
    litefs_copy(g_litefs_test_disk.volatile_blocks[staged.extent_start] + 5U,
                world, sizeof(world) - 1U);
    if (litefs_test_flush(&g_litefs_test_disk) != K_OK) return false;
    staged.size = sizeof(sample) + sizeof(world) - 2U;
    if (litefs_journal_inode(&g_litefs_test_filesystem, inode_index, &staged) != K_OK) {
        return false;
    }
    litefs_test_power_cycle(&g_litefs_test_disk);
    if (litefs_mount(&recovered, &storage, 64U) != K_OK ||
        recovered.inodes[inode_index].size != sizeof(result) ||
        litefs_read_file(&recovered, inode_index, 0, result, sizeof(result), &bytes) != K_OK ||
        bytes != sizeof(result)) return false;
    for (size_t i = 0; i < sizeof(sample) - 1U; ++i) if (result[i] != sample[i]) return false;
    for (size_t i = 0; i < sizeof(world) - 1U; ++i) {
        if (result[sizeof(sample) - 1U + i] != world[i]) return false;
    }

    /* 超级块校验和损坏必须阻止挂载，随后恢复测试盘供 VFS 后端继续使用。 */
    litefs_copy(saved_super, g_litefs_test_disk.durable_blocks[0], sizeof(saved_super));
    g_litefs_test_disk.durable_blocks[0][
        __builtin_offsetof(litefs_super_disk_t, checksum)] ^= 1U;
    litefs_test_power_cycle(&g_litefs_test_disk);
    if (litefs_mount(&recovered, &storage, 64U) != K_EIO) return false;
    litefs_copy(g_litefs_test_disk.durable_blocks[0], saved_super, sizeof(saved_super));
    litefs_test_power_cycle(&g_litefs_test_disk);

    /* inode 校验和损坏也必须阻止挂载，不能把坏元数据当成可回放状态。 */
    litefs_copy(saved_inode,
                g_litefs_test_disk.durable_blocks[LITEFS_INODE_START],
                sizeof(saved_inode));
    g_litefs_test_disk.durable_blocks[LITEFS_INODE_START][
        inode_index * sizeof(litefs_inode_disk_t) +
        __builtin_offsetof(litefs_inode_disk_t, checksum)] ^= 1U;
    for (uint32_t block = 0; block < LITEFS_JOURNAL_BLOCKS; ++block) {
        litefs_zero(g_litefs_test_disk.durable_blocks[LITEFS_JOURNAL_START + block],
                    JOURNAL_BLOCK_SIZE);
    }
    litefs_test_power_cycle(&g_litefs_test_disk);
    if (litefs_mount(&recovered, &storage, 64U) != K_EIO) return false;
    litefs_copy(g_litefs_test_disk.durable_blocks[LITEFS_INODE_START],
                saved_inode, sizeof(saved_inode));
    litefs_test_power_cycle(&g_litefs_test_disk);

    if (litefs_mount(&g_litefs_test_filesystem, &storage, 64U) != K_OK) return false;
    /* 自检只使用私有测试盘，不向用户文件系统注册永久入口。 */
    completed = true;
    return true;
}
