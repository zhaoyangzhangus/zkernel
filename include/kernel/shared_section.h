#pragma once

#include <kernel/object.h>
#include <kernel/vm.h>

#define KOBJECT_TYPE_SHARED_SECTION 0x0116U
#define SHARED_SECTION_RIGHT_MAP    (1U << 0)
#define SHARED_SECTION_RIGHT_ALL    SHARED_SECTION_RIGHT_MAP

typedef struct shared_section {
    object_header_t object;
    vm_object_t *vm_object;
    uint64_t size;
} shared_section_t;

kstatus_t shared_section_create(uint64_t size, shared_section_t **out);
kstatus_t shared_section_create_populated(uint64_t size, shared_section_t **out);
