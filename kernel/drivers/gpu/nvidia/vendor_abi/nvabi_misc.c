#include "nvabi_internal.h"

NvU64 os_page_size = NVABI_PAGE_SIZE;
NvU64 os_max_page_size = NVABI_PAGE_SIZE;
/* NVIDIA 的 os_page_mask 遵循 Linux PAGE_MASK 语义：保留页面对齐的基址，~os_page_mask
 * 则是页内偏移掩码。 */
NvU64 os_page_mask = ~NVABI_PAGE_MASK;
NvU8 os_page_shift = 12U;
NvBool os_cc_enabled = NV_FALSE;
NvBool os_cc_sev_snp_enabled = NV_FALSE;
NvBool os_cc_sme_enabled = NV_FALSE;
NvBool os_cc_snp_vtom_enabled = NV_FALSE;
NvBool os_cc_tdx_enabled = NV_FALSE;
NvBool os_dma_buf_enabled = NV_FALSE;
NvBool os_imex_channel_is_supported = NV_FALSE;

/* 能力文件系统不属于第 3 阶段的 System 接口。根节点为空是有意设计：在后续阶段提供所属
 * 子系统之前，能力入口会安全失败。 */
nv_cap_t *nvidia_caps_root = NULL;

void NV_API_CALL out_string(const char *string) {
    kernel_serial_write(string == NULL ? "<null>" : string);
}

int NV_API_CALL nv_printf(NvU32 debuglevel, const char *format, ...) {
    char buffer[1024];
    va_list arguments;
    NvS32 result;
    (void)debuglevel;
    if (format == NULL) {
        nvabi_log_invalid("nv_printf", "vendor-logging");
        return -1;
    }
    va_start(arguments, format);
    result = os_vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (result >= 0) kernel_serial_write(buffer);
    return result;
}
