#include <kernel/init_filesystem.h>

#include <arch/x86_64/cpu.h>
#include <kernel/block.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/io.h>
#include <kernel/mm.h>
#include <kernel/nvme_core.h>
#include <kernel/realtest.h>
#include <kernel/vfs.h>
#include <usb/storage.h>
#include <kernel/xhci.h>
#include <kernel/block_device.h>
#include <kernel/fat32.h>
#include "filesystem_root_internal.h"

/* REFACTOR_P3_FILESYSTEM_ROOT_OWNER：UEFI 选择的根卷和卷挂载。 */

#define FILESYSTEM_CANDIDATE_LIMIT 64U
#define FILESYSTEM_VOLUME_LIMIT    32U
#define FILESYSTEM_USB_SLOT_LIMIT  256U
#define FILESYSTEM_SECTOR_BUFFER   4096U
#define FILESYSTEM_PATH_LIMIT      256U

typedef enum {
    FILESYSTEM_SOURCE_NONE = 0,
    FILESYSTEM_SOURCE_USB = 1,
    FILESYSTEM_SOURCE_NVME = 2,
} filesystem_source_t;

typedef struct {
    filesystem_source_t source;
    UINT32 source_index;
    OS_BLOCK_DEVICE *usb_device;
    device_t *nvme_device;
    UINT64 disk_block_count;
} filesystem_disk_t;

typedef struct {
    filesystem_source_t source;
    UINT32 source_index;
    OS_BLOCK_DEVICE *usb_device;
    device_t *nvme_device;
    UINT64 disk_block_count;
    UINT64 start_lba;
    UINT64 block_count;
    UINT32 partition_number;
    UINT8 signature[16];
    UINT8 mbr_type;
    UINT8 signature_type;
    BOOLEAN whole_disk;
    BOOLEAN has_required_files;
    UINT32 mounted_volume;
} filesystem_candidate_t;

typedef struct {
    filesystem_source_t source;
    OS_BLOCK_DEVICE *usb_device;
    device_t *nvme_device;
    UINT64 start_lba;
    UINT64 block_count;
} filesystem_slice_t;

typedef struct {
    filesystem_slice_t slice;
    OS_BLOCK_DEVICE block_device;
    OS_FAT32 filesystem;
} filesystem_volume_t;

static filesystem_candidate_t g_candidates[FILESYSTEM_CANDIDATE_LIMIT];
static UINT32 g_candidate_count;
/* 仅为已挂载卷分配对象；每个对象都包含大型 FAT 打开文件表。
 * 卷发布后的地址在其 VFS 挂载生命周期内保持稳定。 */
static filesystem_volume_t *g_volumes[FILESYSTEM_VOLUME_LIMIT];
/* 临时只读探测；仅在清理需要重试时保留。 */
static filesystem_volume_t *g_probe_volume;
static BOOLEAN g_usb_discovered;
static BOOLEAN g_nvme_discovered;
static BOOLEAN g_root_mounted;
static UINT32 g_root_candidate = UINT32_MAX;

/* 即使编译时关闭常规串口日志，也保留仅在失败时输出的存储诊断。
 * QEMU 回归映像会捕获此路径的 COM1 输出。 */
static void filesystem_diag_putc(char value) {
    uint16_t status_port = 0x3FDU;
    uint16_t data_port = 0x3F8U;
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        uint8_t ready;
        __asm__ volatile ("inb %w1, %0" : "=a"(ready) : "d"(status_port));
        if ((ready & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((uint8_t)value), "d"(data_port));
}

static void filesystem_diag_text(const char *text) {
    while (*text != '\0') filesystem_diag_putc(*text++);
}

static void filesystem_diag_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    filesystem_diag_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        filesystem_diag_putc(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static void filesystem_nvme_diag(uint32_t stage, uint64_t lba,
                                 uint32_t sectors, kstatus_t status,
                                 const io_request_t *request) {
    filesystem_diag_text("OS_NVME_IO_FAIL stage=");
    filesystem_diag_hex(stage);
    filesystem_diag_text(" lba=");
    filesystem_diag_hex(lba);
    filesystem_diag_text(" sectors=");
    filesystem_diag_hex(sectors);
    filesystem_diag_text(" status=");
    filesystem_diag_hex((uint32_t)status);
    filesystem_diag_text(" request_status=");
    filesystem_diag_hex(request != 0 ? (uint32_t)request->status : 0U);
    filesystem_diag_text(" bytes=");
    filesystem_diag_hex(request != 0 ? request->bytes_done : 0U);
    filesystem_diag_text(" cqe=");
    filesystem_diag_hex(nvme_last_completion_status());
    filesystem_diag_text("\r\n");
}

static UINT32 filesystem_load_u32(const UINT8 *source) {
    return (UINT32)source[0] | ((UINT32)source[1] << 8) |
           ((UINT32)source[2] << 16) | ((UINT32)source[3] << 24);
}

static UINT64 filesystem_load_u64(const UINT8 *source) {
    UINT64 value = 0U;
    for (UINT32 index = 0U; index < 8U; ++index) {
        value |= (UINT64)source[index] << (index * 8U);
    }
    return value;
}

static void filesystem_zero(void *memory, UINTN size) {
    if (memory != 0) __builtin_memset(memory, 0, size);
}

static void filesystem_copy(UINT8 *destination, const UINT8 *source,
                            UINTN size) {
    if (destination != 0 && source != 0) {
        __builtin_memcpy(destination, source, size);
    }
}

static void filesystem_log_source(filesystem_source_t source, UINT32 index) {
    kernel_serial_write(source == FILESYSTEM_SOURCE_USB ? "USB" : "NVME");
    kernel_serial_write(" INDEX=");
    kernel_serial_write_u32(index);
}

static void filesystem_log_candidate(const char *stage,
                                     const filesystem_candidate_t *candidate) {
    if (stage == 0 || candidate == 0) return;
    kernel_serial_write(stage);
    kernel_serial_write(" SOURCE=");
    filesystem_log_source(candidate->source, candidate->source_index);
    kernel_serial_write(" START=");
    kernel_serial_write_u32((UINT32)candidate->start_lba);
    kernel_serial_write(" COUNT=");
    kernel_serial_write_u32((UINT32)candidate->block_count);
    kernel_serial_write(" PART=");
    kernel_serial_write_u32(candidate->partition_number);
    kernel_serial_write("\r\n");
}

static BOOLEAN filesystem_range_valid(UINT64 disk_count, UINT64 start_lba,
                                      UINT64 block_count) {
    return disk_count != 0U && block_count != 0U && start_lba < disk_count &&
           block_count <= disk_count - start_lba;
}

static BOOLEAN filesystem_usb_read(VOID *context, UINT64 lba, UINT32 count,
                                   VOID *buffer) {
    filesystem_slice_t *slice = (filesystem_slice_t *)context;
    if (slice == 0 || slice->usb_device == 0 || buffer == 0 || count == 0U ||
        !filesystem_range_valid(slice->usb_device->BlockCount,
                                slice->start_lba, slice->block_count) ||
        lba >= slice->block_count || count > slice->block_count - lba ||
        slice->start_lba > UINT64_MAX - lba) return 0;
    return block_read(slice->usb_device, slice->start_lba + lba,
                             count, buffer);
}

static BOOLEAN filesystem_usb_write(VOID *context, UINT64 lba, UINT32 count,
                                    const VOID *buffer) {
    filesystem_slice_t *slice = (filesystem_slice_t *)context;
    if (slice == 0 || slice->usb_device == 0 || buffer == 0 || count == 0U ||
        !filesystem_range_valid(slice->usb_device->BlockCount,
                                slice->start_lba, slice->block_count) ||
        lba >= slice->block_count || count > slice->block_count - lba ||
        slice->start_lba > UINT64_MAX - lba) return 0;
    return block_write(slice->usb_device, slice->start_lba + lba,
                              count, buffer);
}

static BOOLEAN filesystem_usb_flush(VOID *context) {
    filesystem_slice_t *slice = (filesystem_slice_t *)context;
    return slice != 0 && slice->usb_device != 0 &&
           block_flush(slice->usb_device);
}

static kstatus_t filesystem_nvme_submit(filesystem_slice_t *slice,
                                        UINT64 lba, UINT32 opcode,
                                        UINT32 bio_opcode,
                                        UINT32 sector_count, VOID *buffer) {
    page_t *page = 0;
    VOID *page_memory = 0;
    BOOLEAN owns_page = 0;
    io_vec_t vector = {0};
    bio_vec_t bio_vector = {0};
    io_request_t request = {0};
    bio_t bio = {0};
    kstatus_t status = K_EIO;
    UINT32 diag_stage = 1U;

    if (slice == 0 || slice->nvme_device == 0 ||
        (opcode != IO_READ && opcode != IO_WRITE && opcode != IO_FLUSH) ||
        (bio_opcode != BIO_OP_READ && bio_opcode != BIO_OP_WRITE &&
         bio_opcode != BIO_OP_FLUSH) ||
        lba > slice->block_count) return K_EINVAL;
    if (bio_opcode == BIO_OP_FLUSH) {
        if (sector_count != 0U || buffer != 0) return K_EINVAL;
    } else if (buffer == 0 || sector_count == 0U ||
               sector_count > 2U * PAGE_SIZE / 512U ||
               sector_count > slice->block_count - lba) {
        return K_EINVAL;
    }

    if (bio_opcode != BIO_OP_FLUSH) {
        /* VFS 文件缺页已提供按页对齐的直接映射页面。直接对其执行 DMA，避免每次 4 KiB 文件读取都
         * 分配并复制到临时 DMA32 页面。栈、slab 和块缓存缓冲区仍走下方的安全 bounce 路径。 */
        uint64_t physical = direct_to_phys(buffer).value;
        if (physical != UINT64_MAX &&
            (physical & (PAGE_SIZE - 1ULL)) == 0U) {
            page = phys_to_page(paddr_make(physical));
            if (page != 0 &&
                (page->flags & (PAGE_FREE | PAGE_COMPOUND_TAIL)) == 0U &&
                phys_to_direct(page_to_phys(page)) == buffer &&
                ((page->flags & PAGE_COMPOUND_HEAD) != 0U ||
                 sector_count <= PAGE_SIZE / 512U) &&
                (uint64_t)sector_count * 512U <=
                    ((page->flags & PAGE_COMPOUND_HEAD) != 0U ?
                     ((uint64_t)PAGE_SIZE << page->order) : PAGE_SIZE)) {
                page_memory = buffer;
            } else {
                page = 0;
            }
        }
        if (page == 0) {
            page = page_alloc(sector_count > PAGE_SIZE / 512U ? 1U : 0U,
                              PAGE_ALLOC_DMA32);
            if (page == 0) {
                status = K_ENOMEM;
                goto done;
            }
            owns_page = 1;
            page_memory = phys_to_direct(page_to_phys(page));
            if (page_memory == 0) {
                status = K_EIO;
                goto done;
            }
            if (opcode == IO_WRITE) {
                /* 提交给 DMA 的每个字节都会在 io_submit 前覆写；分配尾部未使用部分不会参与传输。 */
                filesystem_copy(page_memory, buffer, (UINTN)sector_count * 512U);
            }
        }
        vector.base = page_memory;
        vector.length = sector_count * 512U;
        bio_vector.page = page;
        bio_vector.offset = 0U;
        bio_vector.length = sector_count * 512U;
    }

    for (UINT32 attempt = 0U; attempt < 2U; ++attempt) {
        diag_stage = 2U;
        io_request_init(&request, opcode, slice->nvme_device, 0,
                        bio_opcode == BIO_OP_FLUSH ? 0 : &vector,
                        bio_opcode == BIO_OP_FLUSH ? 0U : 1U);
        bio = (bio_t){0};
        bio.lba = slice->start_lba + lba;
        bio.op = bio_opcode;
        bio.vecs = bio_opcode == BIO_OP_FLUSH ? 0 : &bio_vector;
        bio.vec_count = bio_opcode == BIO_OP_FLUSH ? 0U : 1U;
        bio.io = &request;
        list_init(&bio.node);
        request.completion_target = &bio;

        status = io_submit(&request);
        if (status != K_OK) {
            diag_stage = 3U;
            break;
        }

        uint64_t start_tsc = x86_read_tsc();
        uint64_t timeout_ticks = x86_timeout_ns_to_tsc(5000000000ULL);
        while (!io_request_is_terminal(&request)) {
            (void)nvme_poll_device_completions(slice->nvme_device, 8U);
            if (io_request_is_terminal(&request)) break;
            if (timeout_ticks != 0U &&
                x86_read_tsc() - start_tsc >= timeout_ticks) {
                diag_stage = 4U;
                kernel_serial_write("OS_VOLUME_NVME_TIMEOUT LBA_LOW=");
                kernel_serial_write_u32((UINT32)(slice->start_lba + lba));
                kernel_serial_write("\r\n");
                status = nvme_recover_after_timeout(slice->nvme_device);
                if (status != K_OK) goto done;
                while (!io_request_is_terminal(&request)) {
                    (void)nvme_schedule_deferred_poll();
                    (void)deferred_run(8U);
                    __asm__ volatile ("pause");
                }
                status = K_ETIMEDOUT;
                break;
            }
            __asm__ volatile ("pause");
        }
        if (status == K_ETIMEDOUT && attempt == 0U) continue;
        if (status == K_ETIMEDOUT) break;
        status = request.status;
        diag_stage = 5U;
        if (status == K_OK && bio_opcode != BIO_OP_FLUSH &&
            request.bytes_done != (uint64_t)sector_count * 512U) {
            status = K_EIO;
            diag_stage = 6U;
        }
        break;
    }

    if (status == K_OK && opcode == IO_READ && owns_page) {
        filesystem_copy(buffer, page_memory, (UINTN)sector_count * 512U);
    }

done:
    if (status != K_OK) {
        filesystem_nvme_diag(diag_stage, slice->start_lba + lba,
                             sector_count, status, &request);
    }
    if (owns_page && page != 0) page_free(page);
    return status;
}

static BOOLEAN filesystem_nvme_read(VOID *context, UINT64 lba, UINT32 count,
                                    VOID *buffer) {
    filesystem_slice_t *slice = (filesystem_slice_t *)context;
    if (slice == 0 || buffer == 0 || count == 0U ||
        lba >= slice->block_count || count > slice->block_count - lba) return 0;
    for (UINT32 index = 0U; index < count;) {
        UINT32 chunk = count - index;
        if (chunk > 2U * PAGE_SIZE / 512U) chunk = 2U * PAGE_SIZE / 512U;
        if (filesystem_nvme_submit(slice, lba + index, IO_READ, BIO_OP_READ,
                                    chunk,
                                    (UINT8 *)buffer + index * 512U) != K_OK) {
            return 0;
        }
        index += chunk;
    }
    return 1;
}

static BOOLEAN filesystem_nvme_write(VOID *context, UINT64 lba, UINT32 count,
                                     const VOID *buffer) {
    filesystem_slice_t *slice = (filesystem_slice_t *)context;
    if (slice == 0 || buffer == 0 || count == 0U ||
        lba >= slice->block_count || count > slice->block_count - lba) return 0;
    for (UINT32 index = 0U; index < count;) {
        UINT32 chunk = count - index;
        if (chunk > 2U * PAGE_SIZE / 512U) chunk = 2U * PAGE_SIZE / 512U;
        if (filesystem_nvme_submit(slice, lba + index, IO_WRITE, BIO_OP_WRITE,
                                    chunk,
                                    (UINT8 *)buffer + index * 512U) != K_OK) {
            return 0;
        }
        index += chunk;
    }
    return 1;
}

static BOOLEAN filesystem_nvme_flush(VOID *context) {
    return filesystem_nvme_submit((filesystem_slice_t *)context, 0U,
                                   IO_FLUSH, BIO_OP_FLUSH, 0U, 0) == K_OK;
}

static BOOLEAN filesystem_slice_read(filesystem_slice_t *slice, UINT64 lba,
                                     UINT32 count, VOID *buffer) {
    if (slice == 0) return 0;
    return slice->source == FILESYSTEM_SOURCE_USB ?
        filesystem_usb_read(slice, lba, count, buffer) :
        filesystem_nvme_read(slice, lba, count, buffer);
}

static void filesystem_prepare_slice(const filesystem_candidate_t *candidate,
                                     filesystem_slice_t *slice) {
    filesystem_zero(slice, sizeof(*slice));
    slice->source = candidate->source;
    slice->usb_device = candidate->usb_device;
    slice->nvme_device = candidate->nvme_device;
    slice->start_lba = candidate->start_lba;
    slice->block_count = candidate->block_count;
}

static void filesystem_prepare_block_device(filesystem_slice_t *slice,
                                            OS_BLOCK_DEVICE *device) {
    filesystem_zero(device, sizeof(*device));
    device->Name[0] = 'f';
    device->Name[1] = 's';
    device->Name[2] = 'v';
    device->Name[3] = 'o';
    device->Name[4] = 'l';
    device->BlockSize = slice->source == FILESYSTEM_SOURCE_USB &&
                        slice->usb_device != 0 ? slice->usb_device->BlockSize :
                        512U;
    device->BlockCount = slice->block_count;
    device->Read = slice->source == FILESYSTEM_SOURCE_USB ?
                   filesystem_usb_read : filesystem_nvme_read;
    device->Write = slice->source == FILESYSTEM_SOURCE_USB ?
                    filesystem_usb_write : filesystem_nvme_write;
    device->Flush = slice->source == FILESYSTEM_SOURCE_USB ?
                    filesystem_usb_flush : filesystem_nvme_flush;
    device->Context = slice;
    device->Registered = 1U;
}

static BOOLEAN filesystem_has_required_files(OS_FAT32 *filesystem) {
    os_file_info_t info = {0};
    if (filesystem == 0 ||
        !fat32_stat_path(filesystem, "/EFI/OS/init", &info) ||
        info.type != OS_FILE_TYPE_REGULAR) return 0;
    info = (os_file_info_t){0};
    return fat32_stat_path(filesystem, "/EFI/OS/init-runtime", &info) &&
           info.type == OS_FILE_TYPE_REGULAR;
}

static BOOLEAN filesystem_release_probe(void) {
    if (g_probe_volume == 0) return 1U;
    OS_FAT32 *filesystem = &g_probe_volume->filesystem;
    if (filesystem->Mounted) {
        if (!fat32_destroy(filesystem)) return 0;
    } else if (filesystem->Cache.Initialized) {
        /* 初始化可能在创建块缓存后失败。 */
        if (!block_cache_destroy(&filesystem->Cache)) return 0;
    }
    kfree(g_probe_volume);
    g_probe_volume = 0;
    return 1U;
}

static BOOLEAN filesystem_probe_candidate(filesystem_candidate_t *candidate) {
    if (candidate == 0 || !filesystem_release_probe()) return 0;
    g_probe_volume = kzalloc(sizeof(*g_probe_volume), 0);
    if (g_probe_volume == 0) return 0;
    filesystem_prepare_slice(candidate, &g_probe_volume->slice);
    filesystem_prepare_block_device(&g_probe_volume->slice,
                                    &g_probe_volume->block_device);
    /* init/stat 不会修改磁盘。只读探测时不刷新设备，也不允许通过此临时视图意外写入。 */
    g_probe_volume->block_device.Write = 0;
    g_probe_volume->block_device.Flush = 0;
    BOOLEAN initialized = fat32_init(&g_probe_volume->filesystem,
                                           &g_probe_volume->block_device);
    if (initialized) {
        candidate->has_required_files = filesystem_has_required_files(
            &g_probe_volume->filesystem);
    }
    if (!filesystem_release_probe()) return 0;
    return initialized;
}

static BOOLEAN filesystem_candidate_equal(const filesystem_candidate_t *left,
                                          const filesystem_candidate_t *right) {
    if (left == 0 || right == 0 || left->source != right->source ||
        left->source_index != right->source_index ||
        left->start_lba != right->start_lba ||
        left->block_count != right->block_count ||
        left->partition_number != right->partition_number ||
        left->signature_type != right->signature_type) return 0;
    for (UINT32 index = 0U; index < sizeof(left->signature); ++index) {
        if (left->signature[index] != right->signature[index]) return 0;
    }
    return 1U;
}

static BOOLEAN filesystem_add_candidate(const filesystem_disk_t *disk,
                                        UINT64 start_lba, UINT64 block_count,
                                        UINT32 partition_number,
                                        UINT8 mbr_type, UINT8 signature_type,
                                        const UINT8 signature[16],
                                        BOOLEAN whole_disk) {
    filesystem_candidate_t candidate = {0};
    if (disk == 0 || !filesystem_range_valid(disk->disk_block_count,
                                             start_lba, block_count)) return 0;
    candidate.source = disk->source;
    candidate.source_index = disk->source_index;
    candidate.usb_device = disk->usb_device;
    candidate.nvme_device = disk->nvme_device;
    candidate.disk_block_count = disk->disk_block_count;
    candidate.start_lba = start_lba;
    candidate.block_count = block_count;
    candidate.partition_number = partition_number;
    candidate.mbr_type = mbr_type;
    candidate.signature_type = signature_type;
    candidate.whole_disk = whole_disk;
    candidate.mounted_volume = UINT32_MAX;
    filesystem_copy(candidate.signature, signature, sizeof(candidate.signature));

    for (UINT32 index = 0U; index < g_candidate_count; ++index) {
        if (filesystem_candidate_equal(&g_candidates[index], &candidate)) {
            return 1U;
        }
    }
    if (!filesystem_probe_candidate(&candidate)) return 1U;
    if (g_candidate_count >= FILESYSTEM_CANDIDATE_LIMIT) {
        kernel_serial_write("OS_VOLUME_CANDIDATE_LIMIT\r\n");
        return 0U;
    }
    g_candidates[g_candidate_count++] = candidate;
    filesystem_log_candidate("OS_VOLUME_CANDIDATE", &candidate);
    return 1U;
}

static BOOLEAN filesystem_read_disk(const filesystem_disk_t *disk, UINT64 lba,
                                    UINT32 count, VOID *buffer) {
    filesystem_slice_t slice = {0};
    if (disk == 0 || buffer == 0 || count == 0U ||
        lba >= disk->disk_block_count ||
        count > disk->disk_block_count - lba) return 0;
    slice.source = disk->source;
    slice.usb_device = disk->usb_device;
    slice.nvme_device = disk->nvme_device;
    slice.start_lba = 0U;
    slice.block_count = disk->disk_block_count;
    return filesystem_slice_read(&slice, lba, count, buffer);
}

static BOOLEAN filesystem_scan_gpt(const filesystem_disk_t *disk,
                                   BOOLEAN *table_valid) {
    UINT8 sector[FILESYSTEM_SECTOR_BUFFER];
    UINT64 entries_lba;
    UINT32 entry_count;
    UINT32 entry_size;
    UINT32 entries_per_block;
    UINT64 loaded_lba = UINT64_MAX;
    BOOLEAN found = 0;
    static const UINT8 signature[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };

    if (table_valid != 0) *table_valid = 0;
    if (disk == 0 || disk->source == FILESYSTEM_SOURCE_NONE ||
        disk->disk_block_count <= 2U ||
        ((disk->source == FILESYSTEM_SOURCE_USB && disk->usb_device == 0) ||
         (disk->source == FILESYSTEM_SOURCE_NVME && disk->nvme_device == 0))) {
        return 0U;
    }
    if (!filesystem_read_disk(disk, 1U, 1U, sector)) return 0U;
    for (UINT32 index = 0U; index < sizeof(signature); ++index) {
        if (sector[index] != signature[index]) return 0U;
    }
    if (disk->source == FILESYSTEM_SOURCE_USB &&
        disk->usb_device->BlockSize > sizeof(sector)) return 0U;
    entries_lba = filesystem_load_u64(sector + 72U);
    entry_count = filesystem_load_u32(sector + 80U);
    entry_size = filesystem_load_u32(sector + 84U);
    if (entries_lba == 0U || entries_lba >= disk->disk_block_count ||
        entry_count == 0U || entry_size < 128U || entry_size > 4096U ||
        (disk->source == FILESYSTEM_SOURCE_USB &&
         entry_size > disk->usb_device->BlockSize) ||
        (disk->source == FILESYSTEM_SOURCE_USB &&
         disk->usb_device->BlockSize % entry_size != 0U)) return 0U;
    UINT32 block_size = disk->source == FILESYSTEM_SOURCE_USB ?
                        disk->usb_device->BlockSize : 512U;
    if (block_size > sizeof(sector) || block_size % entry_size != 0U) return 0U;
    entries_per_block = block_size / entry_size;
    if (entry_count > 128U) entry_count = 128U;
    if (table_valid != 0) *table_valid = 1U;

    for (UINT32 index = 0U; index < entry_count; ++index) {
        UINT64 entry_block = entries_lba + (UINT64)(index / entries_per_block);
        UINT32 entry_offset = (index % entries_per_block) * entry_size;
        UINT8 *entry;
        BOOLEAN type_nonzero = 0U;
        if (entry_block < entries_lba || entry_block >= disk->disk_block_count) {
            break;
        }
        if (loaded_lba != entry_block) {
            if (!filesystem_read_disk(disk, entry_block, 1U, sector)) break;
            loaded_lba = entry_block;
        }
        entry = sector + entry_offset;
        for (UINT32 byte = 0U; byte < 16U; ++byte) {
            if (entry[byte] != 0U) {
                type_nonzero = 1U;
                break;
            }
        }
        if (!type_nonzero) continue;
        UINT64 first_lba = filesystem_load_u64(entry + 32U);
        UINT64 last_lba = filesystem_load_u64(entry + 40U);
        if (last_lba < first_lba ||
            last_lba == UINT64_MAX ||
            !filesystem_range_valid(disk->disk_block_count, first_lba,
                                    last_lba - first_lba + 1U)) continue;
        if (filesystem_add_candidate(disk, first_lba,
                                      last_lba - first_lba + 1U,
                                      index + 1U, 2U, 2U, entry + 16U, 0U)) {
            found = 1U;
        }
    }
    return found;
}

static BOOLEAN filesystem_scan_disk(const filesystem_disk_t *disk) {
    UINT8 sector[FILESYSTEM_SECTOR_BUFFER];
    BOOLEAN has_partition_table = 0U;
    BOOLEAN protective_gpt = 0U;
    if (disk == 0 || disk->disk_block_count == 0U ||
        (disk->source == FILESYSTEM_SOURCE_USB &&
         (disk->usb_device == 0 || disk->usb_device->BlockSize < 512U ||
          disk->usb_device->BlockSize > sizeof(sector))) ||
        (disk->source == FILESYSTEM_SOURCE_NVME && disk->nvme_device == 0)) {
        return 0U;
    }
    if (!filesystem_read_disk(disk, 0U, 1U, sector)) return 0U;
    if (sector[510] == 0x55U && sector[511] == 0xAAU) {
        for (UINT32 index = 0U; index < 4U; ++index) {
            UINT8 type = sector[446U + index * 16U + 4U];
            if (type == 0xEEU) {
                protective_gpt = 1U;
                break;
            }
        }
        if (protective_gpt) {
            BOOLEAN gpt_valid = 0U;
            (void)filesystem_scan_gpt(disk, &gpt_valid);
            has_partition_table = gpt_valid;
        } else {
            UINT8 mbr_signature[16] = {0};
            filesystem_copy(mbr_signature, sector + 440U, 4U);
            for (UINT32 index = 0U; index < 4U; ++index) {
                UINT32 offset = 446U + index * 16U;
                UINT8 type = sector[offset + 4U];
                UINT64 start_lba = filesystem_load_u32(sector + offset + 8U);
                UINT64 block_count = filesystem_load_u32(sector + offset + 12U);
                if (type == 0U || start_lba == 0U || block_count == 0U) continue;
                has_partition_table = 1U;
                (void)filesystem_add_candidate(disk, start_lba, block_count,
                                                index + 1U, 1U, 1U,
                                                mbr_signature, 0U);
            }
        }
    }
    if (has_partition_table) return 1U;
    return filesystem_add_candidate(disk, 0U, disk->disk_block_count, 0U,
                                    0U, 0U, 0, 1U);
}

static void filesystem_discover_usb(void) {
    if (g_usb_discovered) return;
    if (!xhci_hardware_present() || !xhci_usb_mass_storage_configured()) {
        realtest_mark("ROOT_USB_NO_MSC");
        g_usb_discovered = 1U;
        return;
    }
    for (UINT32 slot = 1U; slot < FILESYSTEM_USB_SLOT_LIMIT; ++slot) {
        UINT8 interface_number = 0U;
        UINT8 bulk_in = 0U;
        UINT8 bulk_out = 0U;
        if (!xhci_usb_msc_query((UINT8)slot, &interface_number,
                                &bulk_in, &bulk_out)) continue;
        if (!usb_msc_present((UINT8)slot) &&
            !usb_msc_attach((UINT8)slot)) {
            realtest_mark_number("ROOT_USB_ATTACH_FAIL", slot);
            continue;
        }
        OS_BLOCK_DEVICE *device = usb_msc_block_device((UINT8)slot);
        if (device == 0) {
            realtest_mark_number("ROOT_USB_NO_BLOCK_DEVICE", slot);
            continue;
        }
        filesystem_disk_t disk = {
            .source = FILESYSTEM_SOURCE_USB,
            .source_index = slot,
            .usb_device = device,
            .disk_block_count = device->BlockCount,
        };
        kernel_serial_write("OS_VOLUME_SCAN USB SLOT=");
        kernel_serial_write_u32(slot);
        kernel_serial_write("\r\n");
        if (!filesystem_scan_disk(&disk)) {
            realtest_mark_number("ROOT_USB_SCAN_FAIL", slot);
        }
    }
    g_usb_discovered = 1U;
}

static void filesystem_discover_nvme(const nvme_controller_t *fallback) {
    (void)fallback;
    if (g_nvme_discovered) return;
    for (UINT32 index = 0U; index < NVME_MAX_CONTROLLERS; ++index) {
        nvme_controller_t *controller = nvme_controller_at(index);
        if (controller == 0 || controller->device == 0 || !controller->started ||
            !controller->identified || controller->namespace_count == 0U) continue;
        if (controller->namespace_block_size != 512U ||
            controller->namespace_block_count == 0U) {
            kernel_serial_write("OS_VOLUME_NVME_UNSUPPORTED_BLOCK_SIZE INDEX=");
            kernel_serial_write_u32(index);
            kernel_serial_write(" SIZE=");
            kernel_serial_write_u32(controller->namespace_block_size);
            kernel_serial_write("\r\n");
            continue;
        }
        filesystem_disk_t disk = {
            .source = FILESYSTEM_SOURCE_NVME,
            .source_index = index,
            .nvme_device = controller->device,
            .disk_block_count = controller->namespace_block_count,
        };
        kernel_serial_write("OS_VOLUME_SCAN NVME INDEX=");
        kernel_serial_write_u32(index);
        kernel_serial_write("\r\n");
        if (!filesystem_scan_disk(&disk)) {
            kernel_serial_write("OS_VOLUME_NVME_SCAN_FAIL INDEX=");
            kernel_serial_write_u32(index);
            kernel_serial_write("\r\n");
        }
    }
    g_nvme_discovered = 1U;
}

static BOOLEAN filesystem_boot_source_matches(filesystem_source_t source,
                                              const OS_BOOT_INFO *info) {
    if (info == 0 || (info->Flags & OS_BOOTINFO_HAS_BOOT_DEVICE) == 0U) {
        return 0U;
    }
    if (info->BootDeviceTransport == OS_BOOT_DEVICE_TRANSPORT_USB) {
        return source == FILESYSTEM_SOURCE_USB;
    }
    if (info->BootDeviceTransport == OS_BOOT_DEVICE_TRANSPORT_NVME) {
        return source == FILESYSTEM_SOURCE_NVME;
    }
    return 1U;
}

static BOOLEAN filesystem_boot_candidate_matches(
    const filesystem_candidate_t *candidate, const OS_BOOT_INFO *info,
    filesystem_source_t source_filter) {
    if (candidate == 0 || !filesystem_boot_source_matches(candidate->source, info) ||
        (source_filter != FILESYSTEM_SOURCE_NONE &&
         candidate->source != source_filter)) return 0U;
    if (info->BootDevicePartitionSizeLba != 0U) {
        if (candidate->start_lba != info->BootDevicePartitionStartLba ||
            candidate->block_count != info->BootDevicePartitionSizeLba) return 0U;
        if (info->BootDevicePartitionNumber != 0U &&
            candidate->partition_number != info->BootDevicePartitionNumber) return 0U;
    } else if (info->BootDevicePartitionNumber != 0U ||
               !candidate->whole_disk) {
        return 0U;
    }
    if (info->BootDevicePartitionSignatureType != 0U) {
        if (info->BootDevicePartitionMbrType != 0U &&
            candidate->mbr_type != info->BootDevicePartitionMbrType) {
            return 0U;
        }
        if (candidate->signature_type != info->BootDevicePartitionSignatureType) {
            return 0U;
        }
        for (UINT32 index = 0U; index < sizeof(candidate->signature); ++index) {
            if (candidate->signature[index] !=
                info->BootDevicePartitionSignature[index]) return 0U;
        }
    }
    return 1U;
}

static UINT32 filesystem_find_boot_candidate(const OS_BOOT_INFO *info,
                                             filesystem_source_t source_filter) {
    UINT32 selected = UINT32_MAX;
    UINT32 match_count = 0U;
    UINT32 required_count = 0U;
    UINT32 required_selected = UINT32_MAX;
    if (info == 0 || (info->Flags & OS_BOOTINFO_HAS_BOOT_DEVICE) == 0U) {
        kernel_serial_write("OS_ROOT_BOOT_INFO_MISSING\r\n");
        return UINT32_MAX;
    }
    for (UINT32 index = 0U; index < g_candidate_count; ++index) {
        filesystem_candidate_t *candidate = &g_candidates[index];
        if (!filesystem_boot_candidate_matches(candidate, info, source_filter)) {
            continue;
        }
        selected = index;
        ++match_count;
        if (candidate->has_required_files) {
            required_selected = index;
            ++required_count;
        }
    }
    if (match_count == 1U) return selected;
    if (match_count > 1U && required_count == 1U) return required_selected;
    if (match_count == 0U) {
        kernel_serial_write("OS_ROOT_BOOT_VOLUME_NOT_FOUND\r\n");
    } else {
        kernel_serial_write("OS_ROOT_BOOT_VOLUME_AMBIGUOUS COUNT=");
        kernel_serial_write_u32(match_count);
        kernel_serial_write("\r\n");
    }
    return UINT32_MAX;
}

static BOOLEAN filesystem_append_u32(CHAR8 *text, UINT32 *position,
                                     UINT32 capacity, UINT32 value) {
    CHAR8 digits[10];
    UINT32 count = 0U;
    if (text == 0 || position == 0 || *position >= capacity) return 0U;
    do {
        digits[count++] = (CHAR8)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    if (*position + count >= capacity) return 0U;
    while (count != 0U) text[(*position)++] = digits[--count];
    text[*position] = 0;
    return 1U;
}

static BOOLEAN filesystem_make_mount_path(const filesystem_candidate_t *candidate,
                                          CHAR8 path[FILESYSTEM_PATH_LIMIT]) {
    UINT32 position = 0U;
    if (candidate == 0 || path == 0) return 0U;
    filesystem_zero(path, FILESYSTEM_PATH_LIMIT);
    path[position++] = '/'; path[position++] = 'm'; path[position++] = 'n';
    path[position++] = 't'; path[position++] = '/';
    if (candidate->source == FILESYSTEM_SOURCE_USB) {
        path[position++] = 'u'; path[position++] = 's'; path[position++] = 'b';
    } else {
        path[position++] = 'n'; path[position++] = 'v'; path[position++] = 'm';
        path[position++] = 'e';
    }
    if (!filesystem_append_u32(path, &position, FILESYSTEM_PATH_LIMIT,
                               candidate->source_index)) return 0U;
    if (candidate->partition_number != 0U) {
        path[position++] = 'p';
        if (!filesystem_append_u32(path, &position, FILESYSTEM_PATH_LIMIT,
                                   candidate->partition_number)) return 0U;
    } else {
        if (position + 4U >= FILESYSTEM_PATH_LIMIT) return 0U;
        path[position++] = 'd'; path[position++] = 'i';
        path[position++] = 's'; path[position++] = 'k';
        path[position] = 0;
    }
    return 1U;
}

static BOOLEAN filesystem_mount_candidate(UINT32 candidate_index,
                                          BOOLEAN root) {
    filesystem_candidate_t *candidate;
    filesystem_volume_t *volume = 0;
    UINT32 volume_index = FILESYSTEM_VOLUME_LIMIT;
    CHAR8 mount_path[FILESYSTEM_PATH_LIMIT];
    if (candidate_index >= g_candidate_count) return 0U;
    candidate = &g_candidates[candidate_index];
    if (candidate->mounted_volume != UINT32_MAX) {
        return root && candidate->mounted_volume < FILESYSTEM_VOLUME_LIMIT &&
               g_volumes[candidate->mounted_volume] != 0;
    }
    for (UINT32 index = 0U; index < FILESYSTEM_VOLUME_LIMIT; ++index) {
        if (g_volumes[index] == 0) {
            volume_index = index;
            break;
        }
    }
    if (volume_index == FILESYSTEM_VOLUME_LIMIT) {
        filesystem_log_candidate("OS_VOLUME_MOUNT_LIMIT", candidate);
        return 0U;
    }
    volume = kzalloc(sizeof(*volume), 0);
    if (volume == 0) {
        filesystem_log_candidate("OS_VOLUME_MOUNT_OOM", candidate);
        return 0U;
    }
    filesystem_prepare_slice(candidate, &volume->slice);
    filesystem_prepare_block_device(&volume->slice, &volume->block_device);
    filesystem_zero(&volume->filesystem, sizeof(volume->filesystem));
    if (!fat32_init(&volume->filesystem, &volume->block_device)) {
        filesystem_log_candidate("OS_VOLUME_MOUNT_FAIL", candidate);
        kfree(volume);
        return 0U;
    }
    if (root && !filesystem_has_required_files(&volume->filesystem)) {
        kernel_serial_write("OS_ROOT_REQUIRED_FILES_FAIL\r\n");
        realtest_fat_lost("required-files");
        (void)fat32_destroy(&volume->filesystem);
        kfree(volume);
        return 0U;
    }
    if (root) {
        if (vfs_mount_fat32("/", &volume->filesystem) != K_OK) {
            kernel_serial_write("OS_ROOT_VFS_MOUNT_FAIL\r\n");
            realtest_fat_lost("vfs-mount");
            (void)fat32_destroy(&volume->filesystem);
            kfree(volume);
            return 0U;
        }
        realtest_fat_ready(&volume->filesystem);
        g_root_mounted = 1U;
        g_root_candidate = candidate_index;
    } else {
        if (!filesystem_make_mount_path(candidate, mount_path) ||
            vfs_mount_fat32(mount_path, &volume->filesystem) != K_OK) {
            filesystem_log_candidate("OS_VOLUME_MOUNT_FAIL", candidate);
            (void)fat32_destroy(&volume->filesystem);
            kfree(volume);
            return 0U;
        }
    }
    g_volumes[volume_index] = volume;
    candidate->mounted_volume = volume_index;
    filesystem_log_candidate(root ? "OS_ROOT_VOLUME_MOUNTED" :
                             "OS_VOLUME_MOUNTED", candidate);
    return 1U;
}

static BOOLEAN filesystem_mount_selected_root(const OS_BOOT_INFO *info,
                                              filesystem_source_t source_filter) {
    UINT32 candidate = filesystem_find_boot_candidate(info, source_filter);
    if (candidate == UINT32_MAX) return 0U;
    if (g_root_mounted && g_root_candidate != candidate) {
        kernel_serial_write("OS_ROOT_BOOT_VOLUME_CHANGED\r\n");
        return 0U;
    }
    if (g_root_mounted) return 1U;
    return filesystem_mount_candidate(candidate, 1U);
}

static BOOLEAN filesystem_ensure_mount_directory(void) {
    os_file_info_t info = {0};
    kstatus_t status = vfs_stat_kernel("/mnt", &info);
    if (status == K_OK) return info.type == OS_FILE_TYPE_DIRECTORY;
    if (status != K_ENOENT || vfs_mkdir_kernel("/mnt", 0755U) != K_OK) {
        kernel_serial_write("OS_VOLUME_MOUNT_ROOT_DIR_FAIL\r\n");
        return 0U;
    }
    return 1U;
}

static BOOLEAN filesystem_mount_all_secondary(void) {
    for (UINT32 index = 0U; index < g_candidate_count; ++index) {
        if (index == g_root_candidate ||
            g_candidates[index].mounted_volume != UINT32_MAX) continue;
        (void)filesystem_mount_candidate(index, 0U);
    }
    return 1U;
}

BOOLEAN filesystem_mount_usb_root(const OS_BOOT_INFO *boot_info) {
    filesystem_discover_usb();
    return filesystem_mount_selected_root(boot_info, FILESYSTEM_SOURCE_USB);
}

BOOLEAN filesystem_mount_nvme_root(const OS_BOOT_INFO *boot_info,
                                   device_t *fallback_device) {
    (void)fallback_device;
    filesystem_discover_nvme(nvme_active_controller());
    return filesystem_mount_selected_root(boot_info, FILESYSTEM_SOURCE_NVME);
}

BOOLEAN filesystem_mount_all_volumes(const OS_BOOT_INFO *boot_info,
                                     const nvme_controller_t *active_controller,
                                     BOOLEAN *root_is_nvme) {
    UINT32 root_candidate;
    filesystem_discover_usb();
    filesystem_discover_nvme(active_controller);
    if (g_root_mounted && g_root_candidate < g_candidate_count &&
        filesystem_boot_candidate_matches(&g_candidates[g_root_candidate],
                                          boot_info, FILESYSTEM_SOURCE_NONE)) {
        root_candidate = g_root_candidate;
    } else {
        root_candidate = filesystem_find_boot_candidate(
            boot_info, FILESYSTEM_SOURCE_NONE);
    }
    if (root_candidate == UINT32_MAX) return 0U;
    if (g_root_mounted && g_root_candidate != root_candidate) {
        kernel_serial_write("OS_ROOT_BOOT_VOLUME_CHANGED\r\n");
        return 0U;
    }
    if (!g_root_mounted && !filesystem_mount_candidate(root_candidate, 1U)) {
        return 0U;
    }
    if (!filesystem_ensure_mount_directory()) {
        /* 只读根卷仍可通过 VFS 暴露挂载点下的子目录。 */
        kernel_serial_write("OS_VOLUME_MOUNT_ROOT_DIR_CONTINUE\r\n");
    }
    (void)filesystem_mount_all_secondary();
    if (root_is_nvme != 0) {
        *root_is_nvme = g_candidates[root_candidate].source ==
                        FILESYSTEM_SOURCE_NVME;
    }
    return 1U;
}
