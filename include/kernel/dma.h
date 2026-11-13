#pragma once
#pragma once
#include "base.h"
#include "mm.h"

struct device;

enum dma_direction {
    DMA_TO_DEVICE = 0,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL,
};

enum dma_device_access {
    DMA_DEVICE_READ = 0,
    DMA_DEVICE_WRITE,
};

typedef struct dma_segment {
    iova_t addr;
    uint64_t length;
} dma_segment_t;

typedef struct dma_mapping {
    struct device *device;
    page_t **pages;
    uint32_t page_count;
    uint32_t direction;

    dma_segment_t *segments;
    uint32_t segment_count;

    uint64_t mapped_length;
    uint32_t flags;
} dma_mapping_t;

kstatus_t dma_map_pages(struct device *dev, page_t **pages, uint32_t page_count,
                        enum dma_direction dir, dma_mapping_t *out);
/* 尝试解除映射；失败时映射仍归调用者所有，不能释放后备页。 */
/*
 * 将有效的 DMA 映射转移到另一个 dma_mapping_t 对象，不改变其 IOVA、页面固定状态、IOMMU 映射
 * 或设备引用。
 *
 * dma_mapping_t 在 DMA Core 注册期间具有地址标识；调用方必须使用此原语转移映射，不能复制有效映射。
 */
kstatus_t dma_mapping_move(
    dma_mapping_t *destination,
    dma_mapping_t *source);

kstatus_t dma_unmap_checked(dma_mapping_t *mapping);
void dma_unmap(dma_mapping_t *mapping);
void dma_sync_for_device(dma_mapping_t *mapping);
void dma_sync_for_cpu(dma_mapping_t *mapping);
/* 在设备门铃之前发布已经写入的描述符和数据。 */
void dma_wmb(void);
kstatus_t dma_validate_access(struct device *dev, iova_t address,
                              uint64_t length, enum dma_device_access access);

/* 返回当前映射是否仍由 DMA 核心持有，供驱动卸载路径做幂等检查。 */
bool dma_mapping_active(const dma_mapping_t *mapping);
