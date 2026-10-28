#pragma once

/*
 * NVIDIA 595.84 kernel-open/nvidia 前端 UAPI。
 *
 * 这是 System 侧 ABI 头文件。不可修改的供应商头文件仍是布局参考；不要将这些结构改为
 * System 原生对象。下方类型刻意采用固定宽度 C 类型，使内核与探测二进制中的
 * Linux x86-64 ioctl ABI 保持一致。
 */

#include <stdint.h>

#define NV_FRONTEND_IOCTL_MAGIC 'F'
#define NV_FRONTEND_IOCTL_BASE  200U

#define NV_FRONTEND_ESC_CARD_INFO          (NV_FRONTEND_IOCTL_BASE + 0U)
#define NV_FRONTEND_ESC_REGISTER_FD        (NV_FRONTEND_IOCTL_BASE + 1U)
#define NV_FRONTEND_ESC_ALLOC_OS_EVENT     (NV_FRONTEND_IOCTL_BASE + 6U)
#define NV_FRONTEND_ESC_FREE_OS_EVENT      (NV_FRONTEND_IOCTL_BASE + 7U)
#define NV_FRONTEND_ESC_STATUS_CODE        (NV_FRONTEND_IOCTL_BASE + 9U)
#define NV_FRONTEND_ESC_CHECK_VERSION_STR  (NV_FRONTEND_IOCTL_BASE + 10U)
#define NV_FRONTEND_ESC_IOCTL_XFER_CMD     (NV_FRONTEND_IOCTL_BASE + 11U)
#define NV_FRONTEND_ESC_ATTACH_GPUS_TO_FD  (NV_FRONTEND_IOCTL_BASE + 12U)
#define NV_FRONTEND_ESC_QUERY_DEVICE_INTR  (NV_FRONTEND_IOCTL_BASE + 13U)
#define NV_FRONTEND_ESC_SYS_PARAMS         (NV_FRONTEND_IOCTL_BASE + 14U)
#define NV_FRONTEND_ESC_EXPORT_TO_DMABUF_FD (NV_FRONTEND_IOCTL_BASE + 17U)
#define NV_FRONTEND_ESC_WAIT_OPEN_COMPLETE (NV_FRONTEND_IOCTL_BASE + 18U)

/* 这些旧版 RM escape 编号未列入公共 ioctl 编号头文件，但属于 595.84 Linux 前端 ABI。 */
#define NV_FRONTEND_ESC_RM_FREE             41U
#define NV_FRONTEND_ESC_RM_CONTROL          42U
#define NV_FRONTEND_ESC_RM_ALLOC            43U

typedef struct nv_frontend_pci_info {
    uint32_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
} nv_frontend_pci_info_t;

typedef struct nv_frontend_ioctl_xfer {
    uint32_t cmd;
    uint32_t size;
    uint64_t ptr __attribute__((aligned(8)));
} nv_frontend_ioctl_xfer_t;

typedef struct nv_frontend_card_info {
    uint8_t valid;
    nv_frontend_pci_info_t pci_info;
    uint32_t gpu_id;
    uint16_t interrupt_line;
    uint64_t reg_address __attribute__((aligned(8)));
    uint64_t reg_size __attribute__((aligned(8)));
    uint64_t fb_address __attribute__((aligned(8)));
    uint64_t fb_size __attribute__((aligned(8)));
    uint32_t minor_number;
    uint8_t dev_name[10];
} nv_frontend_card_info_t;

typedef struct nv_frontend_alloc_os_event {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t fd;
    uint32_t status;
} nv_frontend_alloc_os_event_t;

typedef struct nv_frontend_free_os_event {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t fd;
    uint32_t status;
} nv_frontend_free_os_event_t;

typedef struct nv_frontend_status_code {
    uint32_t domain;
    uint8_t bus;
    uint8_t slot;
    uint32_t status;
} nv_frontend_status_code_t;

#define NV_FRONTEND_RM_API_VERSION_STRING_LENGTH 64U

typedef struct nv_frontend_rm_api_version {
    uint32_t cmd;
    uint32_t reply;
    char version_string[NV_FRONTEND_RM_API_VERSION_STRING_LENGTH];
} nv_frontend_rm_api_version_t;

#define NV_FRONTEND_RM_API_VERSION_CMD_STRICT  0U
#define NV_FRONTEND_RM_API_VERSION_CMD_RELAXED ((uint32_t)'1')
#define NV_FRONTEND_RM_API_VERSION_CMD_QUERY   ((uint32_t)'2')

#define NV_FRONTEND_RM_API_VERSION_REPLY_UNRECOGNIZED 0U
#define NV_FRONTEND_RM_API_VERSION_REPLY_RECOGNIZED   1U

typedef struct nv_frontend_query_device_intr {
    uint32_t intr_status __attribute__((aligned(4)));
    uint32_t status;
} nv_frontend_query_device_intr_t;

typedef struct nv_frontend_sys_params {
    uint64_t memblock_size __attribute__((aligned(8)));
} nv_frontend_sys_params_t;

typedef struct nv_frontend_register_fd {
    int32_t ctl_fd;
} nv_frontend_register_fd_t;

#define NV_FRONTEND_DMABUF_EXPORT_MAX_HANDLES 128U

typedef struct nv_frontend_export_to_dma_buf_fd {
    int32_t fd;
    uint32_t hClient;
    uint32_t total_objects;
    uint32_t num_objects;
    uint32_t index;
    uint64_t total_size __attribute__((aligned(8)));
    uint8_t mapping_type;
    uint8_t allow_mmap;
    uint32_t handles[NV_FRONTEND_DMABUF_EXPORT_MAX_HANDLES];
    uint64_t offsets[NV_FRONTEND_DMABUF_EXPORT_MAX_HANDLES]
        __attribute__((aligned(8)));
    uint64_t sizes[NV_FRONTEND_DMABUF_EXPORT_MAX_HANDLES]
        __attribute__((aligned(8)));
    uint32_t status;
} nv_frontend_export_to_dma_buf_fd_t;

typedef struct nv_frontend_wait_open_complete {
    int32_t rc;
    uint32_t adapter_status;
} nv_frontend_wait_open_complete_t;

/* 供应商 NVOS64_PARAMETERS（NV04_ALLOC_WITH_ACCESS），在 System 前端边界处保持为
 * 固定宽度的 Linux x86-64 ABI 类型。 */
typedef struct nv_frontend_nvos64_parameters {
    uint32_t h_root;
    uint32_t h_object_parent;
    uint32_t h_object_new;
    uint32_t h_class;
    uint64_t p_alloc_parms __attribute__((aligned(8)));
    uint64_t p_rights_requested __attribute__((aligned(8)));
    uint32_t params_size;
    uint32_t flags;
    uint32_t status;
} nv_frontend_nvos64_parameters_t;

/* 供应商 NVOS00_PARAMETERS（NV04_FREE）。 */
typedef struct nv_frontend_nvos00_parameters {
    uint32_t h_root;
    uint32_t h_object_parent;
    uint32_t h_object_old;
    uint32_t status;
} nv_frontend_nvos00_parameters_t;

/* 供应商 NVOS54_PARAMETERS（NV04_CONTROL）。 */
typedef struct nv_frontend_nvos54_parameters {
    uint32_t h_client;
    uint32_t h_object;
    uint32_t cmd;
    uint32_t flags;
    uint64_t params __attribute__((aligned(8)));
    uint32_t params_size;
    uint32_t status;
} nv_frontend_nvos54_parameters_t;

/* 不可修改的 595.84 前端使用的 Linux x86-64 _IOC 编码。 */
#define NV_FRONTEND_IOC_NRBITS    8U
#define NV_FRONTEND_IOC_TYPEBITS  8U
#define NV_FRONTEND_IOC_SIZEBITS  14U
#define NV_FRONTEND_IOC_DIRBITS   2U
#define NV_FRONTEND_IOC_NRSHIFT   0U
#define NV_FRONTEND_IOC_TYPESHIFT (NV_FRONTEND_IOC_NRSHIFT + NV_FRONTEND_IOC_NRBITS)
#define NV_FRONTEND_IOC_SIZESHIFT (NV_FRONTEND_IOC_TYPESHIFT + NV_FRONTEND_IOC_TYPEBITS)
#define NV_FRONTEND_IOC_DIRSHIFT  (NV_FRONTEND_IOC_SIZESHIFT + NV_FRONTEND_IOC_SIZEBITS)
#define NV_FRONTEND_IOC_NONE      0U
#define NV_FRONTEND_IOC_WRITE     1U
#define NV_FRONTEND_IOC_READ      2U

#define NV_FRONTEND_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << NV_FRONTEND_IOC_DIRSHIFT) | \
     ((uint32_t)(size) << NV_FRONTEND_IOC_SIZESHIFT) | \
     ((uint32_t)(type) << NV_FRONTEND_IOC_TYPESHIFT) | \
     ((uint32_t)(nr) << NV_FRONTEND_IOC_NRSHIFT))
#define NV_FRONTEND_IOWR(nr, type) \
    NV_FRONTEND_IOC(NV_FRONTEND_IOC_READ | NV_FRONTEND_IOC_WRITE, \
                    NV_FRONTEND_IOCTL_MAGIC, (nr), sizeof(type))

#define NV_FRONTEND_IOCTL_CARD_INFO \
    NV_FRONTEND_IOWR(NV_FRONTEND_ESC_CARD_INFO, nv_frontend_card_info_t)
#define NV_FRONTEND_IOCTL_CHECK_VERSION \
    NV_FRONTEND_IOWR(NV_FRONTEND_ESC_CHECK_VERSION_STR, \
                     nv_frontend_rm_api_version_t)
#define NV_FRONTEND_IOCTL_QUERY_DEVICE_INTR \
    NV_FRONTEND_IOWR(NV_FRONTEND_ESC_QUERY_DEVICE_INTR, \
                     nv_frontend_query_device_intr_t)
#define NV_FRONTEND_IOCTL_WAIT_OPEN_COMPLETE \
    NV_FRONTEND_IOWR(NV_FRONTEND_ESC_WAIT_OPEN_COMPLETE, \
                     nv_frontend_wait_open_complete_t)

#define NV_FRONTEND_IOCTL_NR(request) \
    (((uint32_t)(request) >> NV_FRONTEND_IOC_NRSHIFT) & \
     ((1U << NV_FRONTEND_IOC_NRBITS) - 1U))
#define NV_FRONTEND_IOCTL_SIZE(request) \
    (((uint32_t)(request) >> NV_FRONTEND_IOC_SIZESHIFT) & \
     ((1U << NV_FRONTEND_IOC_SIZEBITS) - 1U))

_Static_assert(sizeof(nv_frontend_pci_info_t) == 12U,
               "595.84 nv_pci_info_t layout changed");
_Static_assert(sizeof(nv_frontend_ioctl_xfer_t) == 16U,
               "595.84 nv_ioctl_xfer_t layout changed");
_Static_assert(sizeof(nv_frontend_card_info_t) == 72U,
               "595.84 nv_ioctl_card_info_t layout changed");
_Static_assert(__builtin_offsetof(nv_frontend_card_info_t, reg_address) == 24U,
               "595.84 card info register address offset changed");
_Static_assert(sizeof(nv_frontend_rm_api_version_t) == 72U,
               "595.84 version ioctl layout changed");
_Static_assert(sizeof(nv_frontend_query_device_intr_t) == 8U,
               "595.84 device interrupt query layout changed");
_Static_assert(sizeof(nv_frontend_sys_params_t) == 8U,
               "595.84 system parameter layout changed");
_Static_assert(sizeof(nv_frontend_wait_open_complete_t) == 8U,
               "595.84 open completion layout changed");
_Static_assert(sizeof(nv_frontend_nvos64_parameters_t) == 48U,
               "595.84 NVOS64 allocation layout changed");
_Static_assert(__builtin_offsetof(nv_frontend_nvos64_parameters_t,
                                  p_alloc_parms) == 16U,
               "595.84 NVOS64 allocation pointer offset changed");
_Static_assert(__builtin_offsetof(nv_frontend_nvos64_parameters_t,
                                   p_rights_requested) == 24U,
               "595.84 NVOS64 rights pointer offset changed");
_Static_assert(sizeof(nv_frontend_nvos00_parameters_t) == 16U,
               "595.84 NVOS00 free layout changed");
_Static_assert(sizeof(nv_frontend_nvos54_parameters_t) == 32U,
               "595.84 NVOS54 control layout changed");
_Static_assert(__builtin_offsetof(nv_frontend_nvos54_parameters_t,
                                  params) == 16U,
               "595.84 NVOS54 control pointer offset changed");
