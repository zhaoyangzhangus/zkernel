#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE 0x3000U
#define CODE_OFFSET 0x1000U
#define DATA_OFFSET 0x2000U
#define CODE_ADDRESS 0x00400000ULL
#define DATA_ADDRESS 0x00401000ULL
#define DATA_FILE_SIZE 0x0600U
#define PAGE_SIZE 0x1000ULL
#define DEVICE_ID 0x4C4954454F530001ULL
#define DEVICE_RIGHT_ALL 0x0FU
#define ABI_VERSION 1U

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_header_t;

static void put_u16(uint8_t *image, size_t offset, uint16_t value) {
    memcpy(image + offset, &value, sizeof(value));
}

static void put_u32(uint8_t *image, size_t offset, uint32_t value) {
    memcpy(image + offset, &value, sizeof(value));
}

static void put_u64(uint8_t *image, size_t offset, uint64_t value) {
    memcpy(image + offset, &value, sizeof(value));
}

static void put_header(uint8_t *image, size_t offset, uint32_t size) {
    put_u32(image, offset, size);
    put_u16(image, offset + 4U, ABI_VERSION);
    put_u16(image, offset + 6U, 0U);
}

static void put_path(uint8_t *image, size_t offset, const char *path) {
    memcpy(image + offset, path, strlen(path) + 1U);
}

static int read_blob(const char *path, uint8_t **out, size_t *size) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    /* ftell() returns long.  Every supported host here has a size_t wide
     * enough to represent a positive long; comparing through unsigned long
     * is rejected by MinGW when long is 32-bit and size_t is 64-bit. */
    if (length <= 0) {
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL || fread(data, 1U, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *size = (size_t)length;
    return 1;
}

static int write_image(const char *path, const uint8_t *image) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (fwrite(image, 1U, IMAGE_SIZE, file) != IMAGE_SIZE || fclose(file) != 0) {
        fclose(file);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    uint8_t *blob = NULL;
    size_t blob_size = 0;
    uint8_t *image;
    elf64_header_t *header;
    elf64_program_header_t *programs;
    uint64_t service_offset;
    char *end = NULL;
    int persistent;

    if (argc != 5) {
        fprintf(stderr, "usage: %s blob output persistent service-offset\n", argv[0]);
        return 2;
    }
    persistent = (int)strtol(argv[3], &end, 0);
    if (end == argv[3] || *end != '\0' || (persistent != 0 && persistent != 1)) {
        fprintf(stderr, "invalid persistent flag\n");
        return 2;
    }
    end = NULL;
    service_offset = strtoull(argv[4], &end, 0);
    if (end == argv[4] || *end != '\0' || service_offset >= PAGE_SIZE) {
        fprintf(stderr, "invalid service offset\n");
        return 2;
    }
    if (!read_blob(argv[1], &blob, &blob_size) || blob_size == 0U ||
        blob_size > PAGE_SIZE) {
        fprintf(stderr, "cannot read init blob %s: %s\n", argv[1], strerror(errno));
        free(blob);
        return 1;
    }
    image = (uint8_t *)calloc(1U, IMAGE_SIZE);
    if (image == NULL) {
        free(blob);
        return 1;
    }

    header = (elf64_header_t *)image;
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = 2U;
    header->ident[5] = 1U;
    header->ident[6] = 1U;
    header->type = 2U;
    header->machine = 62U;
    header->version = 1U;
    header->entry = CODE_ADDRESS;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(elf64_program_header_t);
    header->program_header_count = 2U;
    programs = (elf64_program_header_t *)(image + sizeof(*header));
    programs[0].type = 1U;
    programs[0].flags = 5U;
    programs[0].offset = CODE_OFFSET;
    programs[0].virtual_address = CODE_ADDRESS;
    programs[0].file_size = blob_size;
    programs[0].memory_size = PAGE_SIZE;
    programs[0].alignment = PAGE_SIZE;
    programs[1].type = 1U;
    programs[1].flags = 6U;
    programs[1].offset = DATA_OFFSET;
    programs[1].virtual_address = DATA_ADDRESS;
    programs[1].file_size = DATA_FILE_SIZE;
    programs[1].memory_size = PAGE_SIZE;
    programs[1].alignment = PAGE_SIZE;
    memcpy(image + CODE_OFFSET, blob, blob_size);

    image[DATA_OFFSET + 0x40U] = (uint8_t)persistent;
    put_u32(image, DATA_OFFSET + 0x44U, (uint32_t)persistent);
    put_header(image, DATA_OFFSET + 0x50U, 48U);
    put_u64(image, DATA_OFFSET + 0x58U, CODE_ADDRESS + service_offset);
    put_header(image, DATA_OFFSET + 0x90U, 32U);
    put_u64(image, DATA_OFFSET + 0x98U, DEVICE_ID);
    put_u32(image, DATA_OFFSET + 0xA0U, DEVICE_RIGHT_ALL);
    put_header(image, DATA_OFFSET + 0xC0U, 48U);
    put_u32(image, DATA_OFFSET + 0xC8U, 0U);
    put_u64(image, DATA_OFFSET + 0xD8U, DATA_ADDRESS + 0x100U);
    put_u64(image, DATA_OFFSET + 0xE0U, 32U);
    put_header(image, DATA_OFFSET + 0x100U, 32U);
    put_header(image, DATA_OFFSET + 0x140U, 40U);
    put_u32(image, DATA_OFFSET + 0x148U, 0U);
    put_u64(image, DATA_OFFSET + 0x150U, DATA_ADDRESS + 0x100U);
    put_u64(image, DATA_OFFSET + 0x158U, 32U);
    put_path(image, DATA_OFFSET + 0x170U, "/sbin/audiod");
    put_header(image, DATA_OFFSET + 0x180U, 40U);
    put_header(image, DATA_OFFSET + 0x188U, 24U);
    put_u32(image, DATA_OFFSET + 0x190U, 0U);
    put_u32(image, DATA_OFFSET + 0x194U, 48000U);
    put_u16(image, DATA_OFFSET + 0x198U, 2U);
    put_u16(image, DATA_OFFSET + 0x19AU, 1U);
    put_u32(image, DATA_OFFSET + 0x19CU, 128U);
    put_u32(image, DATA_OFFSET + 0x1A0U, 2U);
    put_header(image, DATA_OFFSET + 0x1C0U, 48U);
    put_header(image, DATA_OFFSET + 0x400U, 48U);
    put_path(image, DATA_OFFSET + 0x480U, "/usr/apps/gshell/gshell");
    put_path(image, DATA_OFFSET + 0x4C0U, "/sbin/netd");
    put_path(image, DATA_OFFSET + 0x500U, "/sbin/deviced");
    put_path(image, DATA_OFFSET + 0x520U, "/sbin/logd");
    put_path(image, DATA_OFFSET + 0x540U, "/sbin/crashd");
    put_path(image, DATA_OFFSET + 0x560U, "/usr/apps/chrome/chrome-start");

    if (!write_image(argv[2], image)) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        free(image);
        free(blob);
        return 1;
    }
    free(image);
    free(blob);
    return 0;
}
