#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/audio.h>
#include <kernel/dma.h>
#include <kernel/hda.h>
#include <kernel/kmem.h>
#include <kernel/io_mutex.h>
#include <kernel/pci.h>

/* Intel High Definition Audio 控制器寄存器。 */
#define HDA_CLASS_CODE       0x04U
#define HDA_SUBCLASS_CODE    0x03U
#define HDA_PROG_IF          0x00U
#define HDA_MMIO_VA          (X86_64_MMIO_BASE + 0x16000000ULL)
#define HDA_MMIO_MAX_SIZE    0x00100000ULL

#define HDA_GCAP             0x00U
#define HDA_GCTL             0x08U
#define HDA_STATESTS         0x0EU
#define HDA_INTCTL           0x20U
#define HDA_INTSTS           0x24U
#define HDA_CORBLBASE        0x40U
#define HDA_CORBUBASE        0x44U
#define HDA_CORBWP           0x48U
#define HDA_CORBRP           0x4AU
#define HDA_CORBCTL          0x4CU
#define HDA_CORBSTS          0x4DU
#define HDA_CORBSIZE         0x4EU
#define HDA_RIRBLBASE        0x50U
#define HDA_RIRBUBASE        0x54U
#define HDA_RIRBWP           0x58U
#define HDA_RINTCNT          0x5AU
#define HDA_RIRBCTL          0x5CU
#define HDA_RIRBSTS          0x5DU
#define HDA_RIRBSIZE         0x5EU

#define HDA_STREAM_BASE      0x80U
#define HDA_STREAM_STRIDE    0x20U
#define HDA_SD_CTL           0x00U
#define HDA_SD_STS           0x03U
#define HDA_SD_LPIB          0x04U
#define HDA_SD_CBL           0x08U
#define HDA_SD_LVI           0x0CU
#define HDA_SD_FMT           0x12U
#define HDA_SD_BDLPL         0x18U
#define HDA_SD_BDLPU         0x1CU
#define HDA_SD_CTL_RUN       (1U << 1)
#define HDA_SD_CTL_IOCE      (1U << 2)
#define HDA_BDL_IOC          (1U << 0)
#define HDA_PCM_PERIODS      2U
#define HDA_MAX_BDL_ENTRIES  256U
#define HDA_SD_CTL_STREAM_TAG_SHIFT 20U

#define HDA_GCTL_CRST        (1U << 0)
#define HDA_CORBRP_RST       (1U << 15)
#define HDA_CORBCTL_RUN      (1U << 1)
#define HDA_RIRBCTL_RUN      (1U << 1)
#define HDA_RIRBCTL_INT      (1U << 0)
#define HDA_RIRB_STS_RINTFL  (1U << 0)

#define HDA_ERROR_PCM_ALLOC       31U
#define HDA_ERROR_PCM_STREAM      32U
#define HDA_ERROR_PCM_BDL         33U
#define HDA_ERROR_PCM_PERIOD      34U
#define HDA_ERROR_PCM_BDL_ENTRY   36U
#define HDA_ERROR_PCM_CTL_CLEAR   37U
#define HDA_ERROR_PCM_CBL         38U
#define HDA_ERROR_PCM_LVI         39U
#define HDA_ERROR_PCM_FORMAT      40U
#define HDA_ERROR_PCM_BDL_BASE    41U
#define HDA_ERROR_PCM_START       42U
#define HDA_ERROR_PCM_NO_PROGRESS 43U
#define HDA_ERROR_PCM_STOP        44U
#define HDA_ERROR_PCM_CLEANUP     45U

/* HDA codec verb：参数查询及转换器的流格式/流标签配置。 */
#define HDA_VERB_PARAMETERS          0xF00U
#define HDA_VERB_SET_STREAM_FORMAT   0x200U
#define HDA_VERB_SET_CHANNEL_STREAM  0x706U
#define HDA_PARAM_VENDOR_ID          0x00U
#define HDA_PARAM_NODE_COUNT         0x04U
#define HDA_PARAM_FUNCTION_TYPE      0x05U
#define HDA_PARAM_WIDGET_CAP         0x09U
#define HDA_FUNCTION_AUDIO            0x01U
#define HDA_WIDGET_AUDIO_OUTPUT      0x00U
#define HDA_WIDGET_AUDIO_INPUT       0x01U

typedef struct hda_dma_page {
    page_t *page;
    dma_mapping_t mapping;
    void *cpu;
} hda_dma_page_t;

typedef struct __attribute__((packed)) hda_bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} hda_bdl_entry_t;

typedef struct hda_audio_binding {
    audio_stream_t *stream;
    hda_dma_page_t bdl;
    uint32_t stream_offset;
    uint32_t entry_count;
    uint16_t stream_format;
    bool input;
    uint32_t last_position;
    uint64_t played_bytes, completed_bytes;
} hda_audio_binding_t;

typedef struct hda_controller {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    uint64_t mmio_span;
    uint8_t output_streams;
    uint8_t input_streams;
    uint8_t bidirectional_streams;
    uint16_t codec_state;
    uint16_t corb_entries;
    uint16_t rirb_entries;
    uint8_t codec_address;
    uint8_t codec_node_start;
    uint8_t codec_node_count;
    uint8_t codec_function_node;
    uint8_t codec_output_node;
    uint8_t codec_input_node;
    uint8_t codec_output_pin;
    uint8_t codec_output_connection;
    bool codec_output_digital;
    uint32_t codec_vendor_id;
    hda_dma_page_t corb;
    hda_dma_page_t rirb;
    io_mutex_t audio_lock;
    hda_audio_binding_t playback;
    hda_audio_binding_t capture;
    bool initialized;
} hda_controller_t;

static hda_controller_t g_hda;
static uint32_t g_hda_error;
static bool g_hda_present;

static bool hda_stop_binding_locked(hda_audio_binding_t *binding);
static bool hda_release_binding_locked(hda_audio_binding_t *binding);
static bool hda_program_binding_locked(hda_audio_binding_t *binding,
                                       const audio_format_t *format);

/* codec 命令允许等待硬件；可睡眠的锁避免同 CPU 抢占后自旋死锁。 */
static void hda_audio_lock(hda_controller_t *controller) {
    io_mutex_lock_required(&controller->audio_lock);
}

static void hda_audio_unlock(hda_controller_t *controller) {
    io_mutex_unlock_required(&controller->audio_lock);
}

static uint32_t hda_read32(const hda_controller_t *controller, uint32_t offset) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint32_t)) return UINT32_MAX;
    return *(volatile const uint32_t *)(controller->mmio + offset);
}

static uint16_t hda_read16(const hda_controller_t *controller, uint32_t offset) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint16_t)) return UINT16_MAX;
    return *(volatile const uint16_t *)(controller->mmio + offset);
}

static uint8_t hda_read8(const hda_controller_t *controller, uint32_t offset) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint8_t)) return UINT8_MAX;
    return *(volatile const uint8_t *)(controller->mmio + offset);
}

static bool hda_write32(const hda_controller_t *controller, uint32_t offset,
                        uint32_t value) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint32_t)) return false;
    *(volatile uint32_t *)(controller->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

static bool hda_write16(const hda_controller_t *controller, uint32_t offset,
                        uint16_t value) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint16_t)) return false;
    *(volatile uint16_t *)(controller->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

static bool hda_write8(const hda_controller_t *controller, uint32_t offset,
                       uint8_t value) {
    if (controller == 0 || controller->mmio == 0 ||
        offset > controller->mmio_span - sizeof(uint8_t)) return false;
    *(volatile uint8_t *)(controller->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

static uint64_t hda_dma_address(const hda_dma_page_t *buffer) {
    return buffer != 0 && dma_mapping_active(&buffer->mapping) &&
           buffer->mapping.segment_count != 0 ?
           buffer->mapping.segments[0].addr.value : 0;
}

static bool hda_map_mmio(hda_controller_t *controller) {
    uint32_t bar_index = PCI_MAX_BARS;
    if (controller == 0 || controller->pci == 0) return false;
    for (uint32_t index = 0; index < PCI_MAX_BARS; ++index) {
        if ((controller->pci->bars[index].flags & PCI_RESOURCE_MEMORY) != 0U &&
            controller->pci->bars[index].length != 0U) {
            bar_index = index;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) {
        g_hda_error = 10U;
        return false;
    }
    uint64_t span = controller->pci->bars[bar_index].length;
    if (span > HDA_MMIO_MAX_SIZE) span = HDA_MMIO_MAX_SIZE;
    span = (span + PAGE_SIZE - 1ULL) & ~(uint64_t)(PAGE_SIZE - 1ULL);
    if (span == 0 || span > X86_64_MMIO_END - HDA_MMIO_VA + 1ULL ||
        controller->pci->bars[bar_index].address > UINT64_MAX - span) {
        g_hda_error = 11U;
        return false;
    }
    for (uint64_t offset = 0; offset < span; offset += PAGE_SIZE) {
        if (x86_map_page(x86_current_root_table(), HDA_MMIO_VA + offset,
                         paddr_make(controller->pci->bars[bar_index].address + offset),
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
            while (offset != 0) {
                offset -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(), HDA_MMIO_VA + offset, 0);
            }
            g_hda_error = 12U;
            return false;
        }
    }
    controller->mmio = (volatile uint8_t *)(uintptr_t)HDA_MMIO_VA;
    controller->mmio_span = span;
    return true;
}

static void hda_unmap_mmio(hda_controller_t *controller) {
    if (controller == 0 || controller->mmio == 0) return;
    for (uint64_t offset = 0; offset < controller->mmio_span; offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(), HDA_MMIO_VA + offset, 0);
    }
    controller->mmio = 0;
    controller->mmio_span = 0;
}

static bool hda_free_dma(hda_dma_page_t *buffer) {
    if (buffer == 0) return true;
    if (buffer->mapping.device != 0 &&
        dma_unmap_checked(&buffer->mapping) != K_OK) return false;
    if (buffer->page != 0) page_free(buffer->page);
    buffer->page = 0;
    buffer->cpu = 0;
    return true;
}

static bool hda_alloc_dma(hda_controller_t *controller, hda_dma_page_t *buffer) {
    if (controller == 0 || controller->pci == 0 || buffer == 0) return false;
    buffer->page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (buffer->page == 0) return false;
    page_t *pages[1] = {buffer->page};
    if (dma_map_pages((device_t *)&controller->pci->device, pages, 1U,
                      DMA_BIDIRECTIONAL, &buffer->mapping) != K_OK) {
        page_free(buffer->page);
        buffer->page = 0;
        return false;
    }
    buffer->cpu = phys_to_direct(page_to_phys(buffer->page));
    if (buffer->cpu == 0 || hda_dma_address(buffer) == 0) {
        if (dma_unmap_checked(&buffer->mapping) != K_OK) {
            buffer->cpu = 0;
            return false;
        }
        page_free(buffer->page);
        buffer->page = 0;
        buffer->cpu = 0;
        return false;
    }
    return true;
}

static uint16_t hda_select_ring_size(uint8_t capability) {
    /* 编码 0/1/2 分别代表 2/16/256 个条目；优先使用最大的环。 */
    if ((capability & (1U << 6)) != 0U) return 256U;
    if ((capability & (1U << 5)) != 0U) return 16U;
    if ((capability & (1U << 4)) != 0U) return 2U;
    return 0U;
}

/* 控制器复位和 codec 上电需要真实时间，不能用与 CPU 频率相关的空循环替代。 */
static void hda_delay_us(uint32_t microseconds) {
    uint64_t start = x86_read_tsc();
    uint64_t duration = x86_timeout_ns_to_tsc((uint64_t)microseconds * 1000U);
    while (x86_read_tsc() - start < duration) __asm__ volatile ("pause");
}

static bool hda_reset_controller(hda_controller_t *controller) {
    uint32_t control = hda_read32(controller, HDA_GCTL);
    if (control == UINT32_MAX || !hda_write32(controller, HDA_GCTL,
                                               control & ~HDA_GCTL_CRST)) return false;
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        if ((hda_read32(controller, HDA_GCTL) & HDA_GCTL_CRST) == 0U) break;
        __asm__ volatile ("pause");
        if (spin == 99999U) return false;
    }
    hda_delay_us(100U);
    if (!hda_write32(controller, HDA_GCTL, control | HDA_GCTL_CRST)) return false;
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        if ((hda_read32(controller, HDA_GCTL) & HDA_GCTL_CRST) != 0U) {
            hda_delay_us(1000U);
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool hda_stop_command_rings(hda_controller_t *controller) {
    if (!controller->mmio) return true;
    if (!hda_write8(controller, HDA_CORBCTL, 0U) ||
        !hda_write8(controller, HDA_RIRBCTL, 0U)) return false;
    for (uint32_t spin=0U; spin<100000U; ++spin) {
        uint8_t corb=hda_read8(controller,HDA_CORBCTL);
        uint8_t rirb=hda_read8(controller,HDA_RIRBCTL);
        if (corb==UINT8_MAX || rirb==UINT8_MAX) return false;
        if (!(corb&HDA_CORBCTL_RUN) && !(rirb&HDA_RIRBCTL_RUN)) return true;
        __asm__ volatile("pause");
    }
    return false;
}

static bool hda_setup_command_rings(hda_controller_t *controller) {
    uint8_t corb_cap = hda_read8(controller, HDA_CORBSIZE);
    uint8_t rirb_cap = hda_read8(controller, HDA_RIRBSIZE);
    controller->corb_entries = hda_select_ring_size(corb_cap);
    controller->rirb_entries = hda_select_ring_size(rirb_cap);
    if (controller->corb_entries == 0U || controller->rirb_entries == 0U ||
        !hda_alloc_dma(controller, &controller->corb) ||
        !hda_alloc_dma(controller, &controller->rirb)) return false;

    /* 高四位是只读能力，低两位必须选择实际启用的环大小。 */
    uint8_t corb_size = controller->corb_entries == 256U ? 2U :
                        controller->corb_entries == 16U ? 1U : 0U;
    uint8_t rirb_size = controller->rirb_entries == 256U ? 2U :
                        controller->rirb_entries == 16U ? 1U : 0U;
    if (!hda_write8(controller, HDA_CORBCTL, 0U) ||
        !hda_write8(controller, HDA_RIRBCTL, 0U) ||
        !hda_write8(controller, HDA_CORBSIZE, corb_size) ||
        !hda_write8(controller, HDA_RIRBSIZE, rirb_size) ||
        !hda_write16(controller, HDA_CORBRP, HDA_CORBRP_RST) ||
        !hda_write16(controller, HDA_CORBRP, 0U) ||
        !hda_write32(controller, HDA_CORBLBASE, (uint32_t)hda_dma_address(&controller->corb)) ||
        !hda_write32(controller, HDA_CORBUBASE, (uint32_t)(hda_dma_address(&controller->corb) >> 32)) ||
        !hda_write32(controller, HDA_RIRBLBASE, (uint32_t)hda_dma_address(&controller->rirb)) ||
        !hda_write32(controller, HDA_RIRBUBASE, (uint32_t)(hda_dma_address(&controller->rirb) >> 32)) ||
        !hda_write16(controller, HDA_CORBWP, 0U) ||
        !hda_write16(controller, HDA_RIRBWP, 1U << 15U) ||
        !hda_write16(controller, HDA_RINTCNT, 1U) ||
        !hda_write8(controller, HDA_RIRBSTS, 0x05U) ||
        !hda_write8(controller, HDA_RIRBCTL, HDA_RIRBCTL_RUN) ||
        !hda_write8(controller, HDA_CORBCTL, HDA_CORBCTL_RUN)) return false;
    return hda_dma_address(&controller->corb) != 0U &&
           hda_dma_address(&controller->rirb) != 0U;
}

/* 通过 CORB/RIRB 提交一个 codec verb；启动阶段没有中断依赖，使用有界轮询。 */
static bool hda_codec_verb(hda_controller_t *controller, uint32_t command,
                           uint32_t *response) {
    uint16_t write_pointer;
    uint16_t read_pointer;
    uint16_t next_pointer;
    uint16_t rirb_pointer;
    if (controller == 0 || response == 0 || controller->corb.cpu == 0 ||
        controller->rirb.cpu == 0 || controller->corb_entries == 0U ||
        controller->rirb_entries == 0U) {
        g_hda_error = 25U;
        return false;
    }
    write_pointer = hda_read16(controller, HDA_CORBWP);
    read_pointer = hda_read16(controller, HDA_CORBRP);
    rirb_pointer = hda_read16(controller, HDA_RIRBWP);
    if (write_pointer == UINT16_MAX || read_pointer == UINT16_MAX ||
        rirb_pointer == UINT16_MAX) {
        g_hda_error = 26U;
        return false;
    }
    write_pointer = (uint16_t)(write_pointer % controller->corb_entries);
    read_pointer = (uint16_t)(read_pointer % controller->corb_entries);
    rirb_pointer = (uint16_t)(rirb_pointer % controller->rirb_entries);
    next_pointer = (uint16_t)((write_pointer + 1U) % controller->corb_entries);
    for (uint32_t spin = 0; next_pointer == read_pointer && spin < 100000U; ++spin) {
        __asm__ volatile ("pause");
        read_pointer = hda_read16(controller, HDA_CORBRP);
        if (read_pointer == UINT16_MAX) return false;
        read_pointer = (uint16_t)(read_pointer % controller->corb_entries);
    }
    if (next_pointer == read_pointer) {
        g_hda_error = 27U;
        return false;
    }
    ((uint32_t *)controller->corb.cpu)[next_pointer] = command;
    dma_sync_for_device(&controller->corb.mapping);
    if (!hda_write16(controller, HDA_CORBWP, next_pointer)) {
        g_hda_error = 28U;
        return false;
    }
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint16_t current = hda_read16(controller, HDA_RIRBWP);
        if (current == UINT16_MAX) {
            g_hda_error = 29U;
            return false;
        }
        current = (uint16_t)(current % controller->rirb_entries);
        if (current == rirb_pointer) {
            __asm__ volatile ("pause");
            continue;
        }
        dma_sync_for_cpu(&controller->rirb.mapping);
        while (rirb_pointer != current) {
            rirb_pointer = (uint16_t)((rirb_pointer + 1U) % controller->rirb_entries);
            uint64_t entry = ((volatile uint64_t *)controller->rirb.cpu)[rirb_pointer];
            uint32_t flags = (uint32_t)(entry >> 32U);
            if ((flags & (1U << 4U)) || (flags & 15U) != (command >> 28U)) continue;
            *response = (uint32_t)entry;
            (void)hda_write8(controller, HDA_RIRBSTS, HDA_RIRB_STS_RINTFL);
            return true;
        }
    }
    g_hda_error = 30U;
    return false;
}

static bool hda_codec_command(hda_controller_t *controller, uint8_t address,
                              uint8_t node, uint16_t verb, uint16_t payload,
                              uint32_t *response) {
    uint32_t command = ((uint32_t)address << 28U) |
                       ((uint32_t)node << 20U) |
                       ((uint32_t)verb << 8U) | payload;
    return hda_codec_verb(controller, command, response);
}

static bool hda_codec_set(hda_controller_t *controller, uint8_t node,
                          uint16_t verb, uint16_t payload) {
    uint32_t response = 0U;
    return hda_codec_command(controller, controller->codec_address, node,
                             verb, payload, &response);
}

static uint16_t hda_codec_node_start(uint32_t node_count) {
    return (uint16_t)(node_count >> 16U);
}

static uint16_t hda_codec_node_count(uint32_t node_count) {
    return (uint16_t)(node_count & 0xFFFFU);
}

static bool hda_discover_codec(hda_controller_t *controller) {
    if (controller == 0 || controller->codec_state == 0U) return true;
    for (uint8_t address = 0U; address < 16U; ++address) {
        if ((controller->codec_state & (uint16_t)(1U << address)) == 0U) continue;
        uint32_t vendor_id = 0U;
        uint32_t root_node_count = 0U;
        uint32_t function_type = 0U;
        uint32_t function_node_count = 0U;
        if (!hda_codec_command(controller, address, 0U, HDA_VERB_PARAMETERS,
                               HDA_PARAM_VENDOR_ID, &vendor_id) ||
            !hda_codec_command(controller, address, 0U, HDA_VERB_PARAMETERS,
                               HDA_PARAM_NODE_COUNT, &root_node_count) ||
            vendor_id == UINT32_MAX || root_node_count == UINT32_MAX) {
            return false;
        }
        controller->codec_address = address;
        controller->codec_vendor_id = vendor_id;
        controller->codec_node_start =
            (uint8_t)hda_codec_node_start(root_node_count);
        controller->codec_node_count =
            (uint8_t)hda_codec_node_count(root_node_count);
        for (uint16_t function = controller->codec_node_start;
             function < (uint16_t)controller->codec_node_start +
                            controller->codec_node_count;
             ++function) {
            if (!hda_codec_command(controller, address, (uint8_t)function,
                                   HDA_VERB_PARAMETERS,
                                   HDA_PARAM_FUNCTION_TYPE, &function_type) ||
                function_type == UINT32_MAX) {
                return false;
            }
            if ((function_type & 0xFFU) != HDA_FUNCTION_AUDIO) continue;
            controller->codec_function_node = (uint8_t)function;
            if (!hda_codec_command(controller, address, (uint8_t)function,
                                   HDA_VERB_PARAMETERS,
                                   HDA_PARAM_NODE_COUNT,
                                   &function_node_count)) {
                return false;
            }
            uint16_t widget_start = hda_codec_node_start(function_node_count);
            uint16_t widget_count = hda_codec_node_count(function_node_count);
            for (uint16_t widget = widget_start;
                 widget < widget_start + widget_count; ++widget) {
                uint32_t widget_caps = 0U;
                if (!hda_codec_command(controller, address, (uint8_t)widget,
                                       HDA_VERB_PARAMETERS,
                                       HDA_PARAM_WIDGET_CAP,
                                       &widget_caps) ||
                    widget_caps == UINT32_MAX) {
                    return false;
                }
                uint32_t widget_type = (widget_caps >> 20U) & 0x0FU;
                if (widget_type == HDA_WIDGET_AUDIO_OUTPUT &&
                    controller->codec_output_node == 0U) {
                    controller->codec_output_node = (uint8_t)widget;
                } else if (widget_type == HDA_WIDGET_AUDIO_INPUT &&
                           controller->codec_input_node == 0U) {
                    controller->codec_input_node = (uint8_t)widget;
                }
            }
            break;
        }
        return controller->codec_output_node != 0U ||
               controller->codec_input_node != 0U;
    }
    return true;
}

/* 从输出 pin 的连接表选转换器，不能假定第一个 DAC 就接着当前显示器。 */
static bool hda_discover_output_route(hda_controller_t *controller) {
    uint32_t nodes;
    if (!hda_codec_command(controller, controller->codec_address,
            controller->codec_function_node, HDA_VERB_PARAMETERS,
            HDA_PARAM_NODE_COUNT, &nodes)) return false;
    uint16_t first = hda_codec_node_start(nodes), count = hda_codec_node_count(nodes);
    if (first + count > 256U) return false;
    uint8_t fallback_pin = 0U, fallback_dac = 0U, fallback_select = 0U;
    bool fallback_digital = false;
    for (uint16_t pin = first; pin < first + count; ++pin) {
        uint32_t caps, pin_caps, length, sense = 0U;
        if (!hda_codec_command(controller, controller->codec_address, (uint8_t)pin,
                HDA_VERB_PARAMETERS, HDA_PARAM_WIDGET_CAP, &caps)) return false;
        if (((caps >> 20U) & 15U) != 4U) continue;
        if (!hda_codec_command(controller, controller->codec_address, (uint8_t)pin,
                HDA_VERB_PARAMETERS, 0x0CU, &pin_caps) ||
            !hda_codec_command(controller, controller->codec_address, (uint8_t)pin,
                HDA_VERB_PARAMETERS, 0x0EU, &length)) return false;
        if (!(pin_caps & (1U << 4U))) continue;
        if (pin_caps & (1U << 2U))
            (void)hda_codec_command(controller, controller->codec_address,
                                   (uint8_t)pin, 0xF09U, 0U, &sense);
        /* HDMI/DP 的连接表通常为短格式；范围项按前一节点递增展开。 */
        uint16_t previous = 0U, connection = 0U;
        uint32_t packed = 0U;
        uint32_t step = (length & 0x80U) ? 2U : 4U;
        for (uint32_t item = 0U; item < (length & 0x7FU); ++item) {
            if (item % step == 0U && !hda_codec_command(controller,
                    controller->codec_address, (uint8_t)pin, 0xF02U,
                    (uint16_t)item, &packed)) return false;
            uint32_t value = (packed >> ((item % step) * (32U / step))) &
                             (step == 2U ? 0xFFFFU : 0xFFU);
            uint16_t range = step == 2U ? 0x8000U : 0x80U;
            uint16_t end = (uint16_t)(value & (range - 1U));
            uint16_t start = (value & range) ? previous + 1U : end;
            if (end > 255U || start > end) return false;
            for (uint16_t dac = start; dac <= end; ++dac, ++connection) {
                uint32_t dac_caps;
                if (!hda_codec_command(controller, controller->codec_address,
                        (uint8_t)dac, HDA_VERB_PARAMETERS,
                        HDA_PARAM_WIDGET_CAP, &dac_caps)) return false;
                if (((dac_caps >> 20U) & 15U) != HDA_WIDGET_AUDIO_OUTPUT) continue;
                if (!fallback_pin || (sense & (1U << 31U))) {
                    fallback_pin = (uint8_t)pin;
                    fallback_dac = (uint8_t)dac;
                    fallback_select = (uint8_t)connection;
                    fallback_digital = (caps & (1U << 9U)) != 0U;
                }
                if (sense & (1U << 31U)) goto selected;
            }
            previous = end;
        }
    }
selected:
    controller->codec_output_pin = fallback_pin;
    controller->codec_output_connection = fallback_select;
    controller->codec_output_digital = fallback_digital;
    if (fallback_pin) controller->codec_output_node = fallback_dac;
    return true;
}

static bool hda_program_output_route(hda_controller_t *controller,
                                     uint16_t format) {
    uint8_t pin = controller->codec_output_pin;
    uint8_t dac = controller->codec_output_node;
    if (!pin || !dac || (controller->codec_output_digital && (format & 15U) != 1U)) return false;
    uint32_t caps;
    if (!hda_codec_set(controller, controller->codec_function_node, 0x705U, 0U) ||
        !hda_codec_command(controller, controller->codec_address, dac,
                           HDA_VERB_PARAMETERS, HDA_PARAM_WIDGET_CAP, &caps)) return false;
    if ((caps & (1U << 10U)) && !hda_codec_set(controller, dac, 0x705U, 0U)) return false;
    if (!hda_codec_set(controller, pin, 0x701U, controller->codec_output_connection) ||
        !hda_codec_set(controller, pin, 0x707U, 0x40U)) return false;
    if (!controller->codec_output_digital) return true;
    /* 先支持标准双声道 LPCM；多声道需按接收端 ELD 选择扬声器分配。 */
    if ((format & 15U) != 1U ||
        !hda_codec_set(controller, dac, 0x70DU, 1U) ||
        !hda_codec_set(controller, dac, 0x72DU, 1U) ||
        !hda_codec_set(controller, pin, 0x734U, 0x00U) ||
        !hda_codec_set(controller, pin, 0x734U, 0x11U)) return false;
    /* HDMI Audio InfoFrame：双声道、默认声道布局，校验字节使总和模 256 为零。 */
    uint8_t infoframe[14] = {0x84U, 1U, 10U, 0x70U, 1U};
    uint32_t eld = 0U;
    if (hda_codec_command(controller, controller->codec_address, pin,
                           0xF2FU, 5U, &eld) && (eld & (1U << 31U)) &&
        ((eld >> 2U) & 3U) == 1U) {
        /* DP 使用自己的头部；NVIDIA 接收端仍要求保留 HDMI 校验字节布局。 */
        infoframe[1] = 0x1BU;
        infoframe[2] = 0x44U;
        if ((controller->codec_vendor_id >> 16U) == 0x10DEU)
            infoframe[3] = (uint8_t)(0U - (0x84U + 0x1BU + 0x44U + 1U));
        else {infoframe[3] = 1U; infoframe[4] = 0U;}
    }
    if (!hda_codec_set(controller, pin, 0x732U, 0U) ||
        !hda_codec_set(controller, pin, 0x730U, 0U)) return false;
    for (uint32_t i = 0U; i < sizeof(infoframe); ++i)
        if (!hda_codec_set(controller, pin, 0x731U, infoframe[i])) return false;
    return hda_codec_set(controller, pin, 0x732U, 2U << 6U);
}

static void hda_decode_gcap(hda_controller_t *controller, uint16_t capability) {
    controller->output_streams = (uint8_t)((capability >> 12U) & 0x0FU);
    controller->input_streams = (uint8_t)((capability >> 8U) & 0x0FU);
    controller->bidirectional_streams = (uint8_t)((capability >> 3U) & 0x1FU);
}

static bool hda_initialize(hda_controller_t *controller, const pci_device_t *pci) {
    uint32_t capability;
    if (controller == 0 || pci == 0 || controller->initialized) return false;
    for (size_t index = 0; index < sizeof(*controller); ++index) {
        ((uint8_t *)controller)[index] = 0;
    }
    io_mutex_init(&controller->audio_lock, true);
    controller->pci = pci;
    if (pci_enable_memory_busmaster((pci_device_t *)pci) != K_OK ||
        !hda_map_mmio(controller) || !hda_reset_controller(controller)) {
        g_hda_error = g_hda_error == 0U ? 20U : g_hda_error;
        hda_unmap_mmio(controller);
        return false;
    }
    capability = hda_read32(controller, HDA_GCAP);
    if (capability == UINT32_MAX) {
        g_hda_error = 21U;
        hda_unmap_mmio(controller);
        return false;
    }
    hda_decode_gcap(controller, (uint16_t)capability);
    controller->codec_state = hda_read16(controller, HDA_STATESTS);
    if (controller->output_streams == 0U && controller->input_streams == 0U &&
        controller->bidirectional_streams == 0U) {
        g_hda_error = 22U;
        hda_unmap_mmio(controller);
        return false;
    }
    if (!hda_setup_command_rings(controller)) {
        g_hda_error = 23U;
        if (!hda_stop_command_rings(controller)) return false;
        if (!hda_free_dma(&controller->rirb) || !hda_free_dma(&controller->corb)) return false;
        hda_unmap_mmio(controller);
        return false;
    }
    if (!hda_discover_codec(controller) || !hda_discover_output_route(controller)) {
        if (g_hda_error == 0U) g_hda_error = 24U;
        if (!hda_stop_command_rings(controller)) return false;
        if (!hda_free_dma(&controller->rirb) || !hda_free_dma(&controller->corb)) return false;
        hda_unmap_mmio(controller);
        return false;
    }
    controller->initialized = true;
    return true;
}

static bool hda_destroy(hda_controller_t *controller) {
    if (controller == 0) return false;
    if (controller->initialized) {
        hda_audio_lock(controller);
        bool audio_released = hda_release_binding_locked(&controller->playback) &&
                              hda_release_binding_locked(&controller->capture);
        hda_audio_unlock(controller);
        if (!audio_released) return false;
    }
    if (!hda_stop_command_rings(controller)) return false;
    bool released_rirb = hda_free_dma(&controller->rirb);
    bool released_corb = hda_free_dma(&controller->corb);
    bool released = released_rirb && released_corb;
    if (!released) return false;
    hda_unmap_mmio(controller);
    controller->initialized = false;
    controller->pci = 0;
    return true;
}

static uint32_t hda_stream_offset(const hda_controller_t *controller,
                                  bool input, uint8_t index) {
    uint32_t stream_index;
    if (controller == 0) return UINT32_MAX;
    /* 描述符依次为输入、输出、双向；双向流必须显式设置 DIR。 */
    if (input) {
        if (index >= controller->input_streams) return UINT32_MAX;
        stream_index = index;
    } else {
        if (index >= controller->output_streams) return UINT32_MAX;
        stream_index = (uint32_t)controller->input_streams + index;
    }
    return HDA_STREAM_BASE + stream_index * HDA_STREAM_STRIDE;
}

static uint16_t hda_audio_format_value(const audio_format_t *format) {
    uint16_t sample_bits;
    uint16_t rate_code;
    if (format == 0 || format->channels == 0U || format->channels > 16U) return 0U;
    switch ((audio_sample_format_t)format->sample_format) {
        case AUDIO_SAMPLE_S16_LE: sample_bits = 1U; break;
        case AUDIO_SAMPLE_S24_LE: sample_bits = 3U; break;
        case AUDIO_SAMPLE_S32_LE: sample_bits = 4U; break;
        default: return 0U;
    }
    /* 位 14 选择基频，位 13:11 是倍频减一，位 10:8 是分频减一。 */
    switch (format->sample_rate) {
        case 48000U:  rate_code = 0x0000U; break;
        case 44100U:  rate_code = 0x4000U; break;
        case 32000U:  rate_code = 0x0A00U; break;
        case 22050U:  rate_code = 0x4100U; break;
        case 24000U:  rate_code = 0x0100U; break;
        case 16000U:  rate_code = 0x0200U; break;
        case 11025U:  rate_code = 0x4300U; break;
        case 8000U:   rate_code = 0x0500U; break;
        case 88200U:  rate_code = 0x4800U; break;
        case 96000U:  rate_code = 0x0800U; break;
        case 176400U: rate_code = 0x5800U; break;
        case 192000U: rate_code = 0x1800U; break;
        default: return 0U;
    }
    return (uint16_t)((format->channels - 1U) | (sample_bits << 4U) | rate_code);
}

static hda_audio_binding_t *hda_binding_for_stream(audio_stream_t *stream) {
    return audio_stream_direction(stream) == AUDIO_CAPTURE ?
           &g_hda.capture : &g_hda.playback;
}

static bool hda_stop_binding_locked(hda_audio_binding_t *binding) {
    if (binding == 0 || binding->stream == 0 || g_hda.mmio == 0) return true;
    (void)hda_write32(&g_hda, binding->stream_offset + HDA_SD_CTL, 0U);
    for (uint32_t spin = 0; spin < 1000U; ++spin) {
        uint32_t control = hda_read32(&g_hda,
                                      binding->stream_offset + HDA_SD_CTL);
        if (control == UINT32_MAX) return false;
        if ((control & HDA_SD_CTL_RUN) == 0U) return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool hda_program_codec_locked(const hda_audio_binding_t *binding) {
    uint8_t node;
    uint16_t tag;
    if (binding == 0 || binding->stream == 0) return false;
    node = binding->input ? g_hda.codec_input_node : g_hda.codec_output_node;
    if (node == 0U) return false;
    tag = binding->input ? 2U : 1U;
    if (!binding->input && !hda_program_output_route(&g_hda, binding->stream_format)) return false;
    /* 先设置转换器格式，再把转换器绑定到本次 DMA 的流号和声道。 */
    return hda_codec_set(&g_hda, node, HDA_VERB_SET_STREAM_FORMAT,
                         binding->stream_format) &&
           hda_codec_set(&g_hda, node, HDA_VERB_SET_CHANNEL_STREAM,
                         (uint16_t)(tag << 4U));
}

static bool hda_release_binding_locked(hda_audio_binding_t *binding) {
    if (binding == 0 || binding->stream == 0) return true;
    if (!hda_stop_binding_locked(binding)) return false;
    if (!hda_free_dma(&binding->bdl)) return false;
    *binding = (hda_audio_binding_t){0};
    return true;
}

/* 停止后复位描述符，清除旧 LPIB；否则重开流可能把上一轮位置计作已播放。 */
static bool hda_reset_stream_locked(hda_audio_binding_t *binding) {
    if (!hda_stop_binding_locked(binding) ||
        !hda_write8(&g_hda, binding->stream_offset + HDA_SD_CTL, 1U)) return false;
    for (uint32_t i=0U; i<100000U; ++i) {
        if (hda_read8(&g_hda,binding->stream_offset+HDA_SD_CTL)&1U) break;
        if (i==99999U) return false;
    }
    if (!hda_write8(&g_hda,binding->stream_offset+HDA_SD_CTL,0U)) return false;
    for (uint32_t i=0U; i<100000U; ++i) {
        if (!(hda_read8(&g_hda,binding->stream_offset+HDA_SD_CTL)&1U)) {
            binding->last_position=0U;
            binding->played_bytes=binding->completed_bytes=0U;
            return true;
        }
    }
    return false;
}

static bool hda_program_binding_locked(hda_audio_binding_t *binding,
                                       const audio_format_t *format) {
    uint64_t period_bytes;
    uint64_t total_bytes;
    uint64_t bdl_address;
    uint32_t tag;
    if (binding == 0 || binding->stream == 0 || format == 0 ||
        binding->entry_count == 0U || binding->entry_count > HDA_MAX_BDL_ENTRIES ||
        binding->stream_offset == UINT32_MAX) return false;
    period_bytes = audio_stream_period_bytes(binding->stream);
    if (period_bytes == 0U || format->period_count == 0U ||
        period_bytes > UINT64_MAX / format->period_count) return false;
    total_bytes = period_bytes * format->period_count;
    if (total_bytes == 0U || total_bytes > UINT32_MAX) return false;
    bdl_address = hda_dma_address(&binding->bdl);
    if (bdl_address == 0U) return false;
    tag = binding->input ? 2U : 1U;
    if (!hda_reset_stream_locked(binding)) return false;
    if (!hda_program_codec_locked(binding)) return false;
    if (!hda_write8(&g_hda, binding->stream_offset + HDA_SD_STS, 0xFFU) ||
        !hda_write32(&g_hda, binding->stream_offset + HDA_SD_CBL,
                     (uint32_t)total_bytes) ||
        !hda_write8(&g_hda, binding->stream_offset + HDA_SD_LVI,
                    (uint8_t)(binding->entry_count - 1U)) ||
        !hda_write16(&g_hda, binding->stream_offset + HDA_SD_FMT,
                     binding->stream_format) ||
        !hda_write32(&g_hda, binding->stream_offset + HDA_SD_BDLPL,
                     (uint32_t)bdl_address) ||
        !hda_write32(&g_hda, binding->stream_offset + HDA_SD_BDLPU,
                     (uint32_t)(bdl_address >> 32)) ||
        !hda_write32(&g_hda, binding->stream_offset + HDA_SD_CTL,
                     HDA_SD_CTL_IOCE | (tag << HDA_SD_CTL_STREAM_TAG_SHIFT))) {
        return false;
    }
    return true;
}

static kstatus_t hda_build_binding_locked(hda_audio_binding_t *binding,
                                          audio_stream_t *stream) {
    audio_format_t format;
    uint32_t entry_count = 0U;
    if (binding == 0 || stream == 0) return K_EINVAL;
    binding->stream = stream;
    binding->input = audio_stream_direction(stream) == AUDIO_CAPTURE;
    binding->stream_offset = hda_stream_offset(&g_hda, binding->input, 0U);
    if (binding->stream_offset == UINT32_MAX) return K_ENOENT;
    audio_stream_get_format(stream, &format);
    binding->stream_format = hda_audio_format_value(&format);
    if (binding->stream_format == 0U) return K_EINVAL;
    for (uint32_t period = 0; period < format.period_count; ++period) {
        uint32_t segments = audio_stream_period_dma_segment_count(stream, period);
        if (segments == 0U || segments > HDA_MAX_BDL_ENTRIES - entry_count) {
            return K_EINVAL;
        }
        entry_count += segments;
    }
    if (entry_count == 0U || !hda_alloc_dma(&g_hda, &binding->bdl)) return K_ENOMEM;
    if (binding->bdl.mapping.segment_count == 0U) return K_EIO;
    hda_bdl_entry_t *entries = (hda_bdl_entry_t *)binding->bdl.cpu;
    uint32_t entry = 0U;
    for (uint32_t period = 0; period < format.period_count; ++period) {
        uint32_t segments = audio_stream_period_dma_segment_count(stream, period);
        for (uint32_t segment = 0; segment < segments; ++segment) {
            uint64_t address = audio_stream_period_dma_address(stream, period, segment);
            uint64_t length = audio_stream_period_dma_length(stream, period, segment);
            if (address == 0U || length == 0U || length > UINT32_MAX) {
                return K_EIO;
            }
            entries[entry].address = address;
            entries[entry].length = (uint32_t)length;
            entries[entry].flags = HDA_BDL_IOC;
            ++entry;
        }
    }
    dma_sync_for_device(&binding->bdl.mapping);
    binding->entry_count = entry_count;
    if (!hda_program_binding_locked(binding, &format)) return K_EIO;
    return K_OK;
}

kstatus_t hda_audio_stream_configure(struct audio_stream *stream) {
    audio_stream_t *audio = (audio_stream_t *)stream;
    hda_audio_binding_t *binding;
    kstatus_t status;
    if (audio == 0) return K_EINVAL;
    if (!g_hda.initialized || audio_stream_device(audio) != hda_audio_device()) {
        return K_ENOENT;
    }
    binding = hda_binding_for_stream(audio);
    hda_audio_lock(&g_hda);
    if (binding->stream != 0 && binding->stream != audio) {
        hda_audio_unlock(&g_hda);
        return K_EBUSY;
    }
    if (binding->stream == audio && !hda_release_binding_locked(binding)) {
        hda_audio_unlock(&g_hda);
        return K_EIO;
    }
    *binding = (hda_audio_binding_t){0};
    status = hda_build_binding_locked(binding, audio);
    if (status != K_OK) (void)hda_release_binding_locked(binding);
    hda_audio_unlock(&g_hda);
    return status;
}

kstatus_t hda_audio_stream_start(struct audio_stream *stream) {
    audio_stream_t *audio = (audio_stream_t *)stream;
    hda_audio_binding_t *binding;
    uint32_t tag;
    if (audio == 0) return K_EINVAL;
    if (!g_hda.initialized || audio_stream_device(audio) != hda_audio_device()) {
        return K_ENOENT;
    }
    binding = hda_binding_for_stream(audio);
    hda_audio_lock(&g_hda);
    if (binding->stream != audio) {
        hda_audio_unlock(&g_hda);
        return K_ENOENT;
    }
    tag = binding->input ? 2U : 1U;
    bool success = hda_write32(&g_hda, binding->stream_offset + HDA_SD_CTL,
                               HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE |
                               (tag << HDA_SD_CTL_STREAM_TAG_SHIFT));
    hda_audio_unlock(&g_hda);
    return success ? K_OK : K_EIO;
}

/* 用 DMA 位置确认完成，禁止用户态把尚未播放的周期当作已经完成。 */
kstatus_t hda_audio_stream_complete(struct audio_stream *stream,
                                   uint32_t period, uint32_t frames) {
    if (!stream || audio_stream_device(stream) != hda_audio_device()) return K_ENOENT;
    hda_audio_binding_t *binding = hda_binding_for_stream(stream);
    audio_format_t format;
    audio_stream_get_format(stream, &format);
    audio_stream_stats_t stats = {0};
    audio_stream_get_stats(stream, &stats);
    if (stats.state != AUDIO_STREAM_RUNNING || stats.active_period != period) return K_EBUSY;
    uint64_t bytes = audio_stream_period_bytes(stream);
    uint64_t total = bytes * format.period_count;
    if (!bytes || !total || (frames && frames != format.period_frames)) return K_EINVAL;
    hda_audio_lock(&g_hda);
    kstatus_t status = K_EBUSY;
    if (binding->stream == stream &&
        (hda_read32(&g_hda, binding->stream_offset + HDA_SD_CTL) & HDA_SD_CTL_RUN)) {
        uint32_t position = hda_read32(&g_hda, binding->stream_offset + HDA_SD_LPIB);
        /* NVIDIA 在回绕瞬间可报告 CBL 本身；这是环起点，不是越界读数。 */
        if (position == total) position = 0U;
        if (position > total) status = K_EIO;
        else {
            uint32_t delta = position >= binding->last_position ?
                position - binding->last_position :
                (uint32_t)total - binding->last_position + position;
            binding->played_bytes += delta;
            binding->last_position = position;
            if (period != (binding->completed_bytes / bytes) % format.period_count)
                status = K_EINVAL;
            else if (binding->played_bytes < binding->completed_bytes + bytes)
                status = K_EAGAIN;
            else {
                binding->completed_bytes += bytes;
                status = K_OK;
            }
        }
    }
    hda_audio_unlock(&g_hda);
    return status;
}

kstatus_t hda_audio_stream_stop(struct audio_stream *stream) {
    audio_stream_t *audio = (audio_stream_t *)stream;
    hda_audio_binding_t *binding;
    if (audio == 0) return K_EINVAL;
    if (!g_hda.initialized || audio_stream_device(audio) != hda_audio_device()) {
        return K_ENOENT;
    }
    binding = hda_binding_for_stream(audio);
    hda_audio_lock(&g_hda);
    if (binding->stream != audio) {
        hda_audio_unlock(&g_hda);
        return K_ENOENT;
    }
    bool stopped = hda_stop_binding_locked(binding);
    hda_audio_unlock(&g_hda);
    return stopped ? K_OK : K_EIO;
}

kstatus_t hda_audio_stream_reset(struct audio_stream *stream) {
    audio_stream_t *audio = (audio_stream_t *)stream;
    hda_audio_binding_t *binding;
    audio_format_t format;
    if (audio == 0) return K_EINVAL;
    if (!g_hda.initialized || audio_stream_device(audio) != hda_audio_device()) {
        return K_ENOENT;
    }
    binding = hda_binding_for_stream(audio);
    audio_stream_get_format(audio, &format);
    hda_audio_lock(&g_hda);
    if (binding->stream != audio || !hda_program_binding_locked(binding, &format)) {
        hda_audio_unlock(&g_hda);
        return K_EIO;
    }
    hda_audio_unlock(&g_hda);
    return K_OK;
}

kstatus_t hda_audio_stream_disconnect(struct audio_stream *stream) {
    audio_stream_t *audio = (audio_stream_t *)stream;
    hda_audio_binding_t *binding;
    if (audio == 0) return K_EINVAL;
    if (!g_hda.initialized || audio_stream_device(audio) != hda_audio_device()) {
        return K_ENOENT;
    }
    binding = hda_binding_for_stream(audio);
    hda_audio_lock(&g_hda);
    if (binding->stream != audio) {
        hda_audio_unlock(&g_hda);
        return K_ENOENT;
    }
    bool success = hda_release_binding_locked(binding);
    hda_audio_unlock(&g_hda);
    return success ? K_OK : K_EIO;
}

bool hda_pcm_self_test(void) {
    hda_dma_page_t *bdl = 0;
    hda_dma_page_t *periods = 0;
    uint32_t stream = UINT32_MAX;
    bool success = false;
    bool progressed = false;
    if (!g_hda.initialized) return !g_hda_present;
    bdl = (hda_dma_page_t *)kzalloc(sizeof(*bdl), 0);
    periods = (hda_dma_page_t *)kzalloc(sizeof(*periods) * HDA_PCM_PERIODS, 0);
    if (bdl == 0 || periods == 0) {
        g_hda_error = HDA_ERROR_PCM_ALLOC;
        goto cleanup;
    }
    stream = hda_stream_offset(&g_hda, false, 0U);
    if (stream == UINT32_MAX) {
        g_hda_error = HDA_ERROR_PCM_STREAM;
        goto cleanup;
    }
    if (!hda_alloc_dma(&g_hda, bdl)) {
        g_hda_error = HDA_ERROR_PCM_BDL;
        goto cleanup;
    }
    for (uint32_t i = 0; i < HDA_PCM_PERIODS; ++i) {
        if (!hda_alloc_dma(&g_hda, &periods[i])) {
            g_hda_error = HDA_ERROR_PCM_PERIOD + i;
            goto cleanup;
        }
    }
    hda_bdl_entry_t *entries = (hda_bdl_entry_t *)bdl->cpu;
    for (uint32_t i = 0; i < HDA_PCM_PERIODS; ++i) {
        entries[i].address = hda_dma_address(&periods[i]);
        entries[i].length = PAGE_SIZE;
        entries[i].flags = HDA_BDL_IOC;
        if (entries[i].address == 0U) {
            g_hda_error = HDA_ERROR_PCM_BDL_ENTRY;
            goto cleanup;
        }
    }
    dma_sync_for_device(&bdl->mapping);
    if (!hda_write32(&g_hda, stream + HDA_SD_CTL, 0U)) {
        g_hda_error = HDA_ERROR_PCM_CTL_CLEAR;
        goto cleanup;
    }
    if (!hda_write32(&g_hda, stream + HDA_SD_CBL,
                     PAGE_SIZE * HDA_PCM_PERIODS)) {
        g_hda_error = HDA_ERROR_PCM_CBL;
        goto cleanup;
    }
    if (!hda_write8(&g_hda, stream + HDA_SD_LVI, HDA_PCM_PERIODS - 1U)) {
        g_hda_error = HDA_ERROR_PCM_LVI;
        goto cleanup;
    }
    if (!hda_write16(&g_hda, stream + HDA_SD_FMT, 0x0011U)) {
        g_hda_error = HDA_ERROR_PCM_FORMAT;
        goto cleanup;
    }
    uint64_t bdl_address = hda_dma_address(bdl);
    if (bdl_address == 0U ||
        !hda_write32(&g_hda, stream + HDA_SD_BDLPL,
                     (uint32_t)bdl_address) ||
        !hda_write32(&g_hda, stream + HDA_SD_BDLPU,
                     (uint32_t)(bdl_address >> 32))) {
        g_hda_error = HDA_ERROR_PCM_BDL_BASE;
        goto cleanup;
    }
    /* Stream tags are one-based.  A zero tag is unused on physical HDA
     * controllers even though some emulators tolerate it. */
    if (!hda_write32(&g_hda, stream + HDA_SD_CTL,
                     HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE |
                     (1U << HDA_SD_CTL_STREAM_TAG_SHIFT))) {
        g_hda_error = HDA_ERROR_PCM_START;
        goto cleanup;
    }
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint32_t position = hda_read32(&g_hda, stream + HDA_SD_LPIB);
        if (position == UINT32_MAX) {
            g_hda_error = HDA_ERROR_PCM_NO_PROGRESS;
            goto cleanup;
        }
        if (position != 0U) {
            progressed = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    if (!progressed) {
        g_hda_error = HDA_ERROR_PCM_NO_PROGRESS;
        goto cleanup;
    }
    success = hda_write32(&g_hda, stream + HDA_SD_CTL, 0U);
    if (!success) g_hda_error = HDA_ERROR_PCM_STOP;
    (void)hda_write8(&g_hda, stream + HDA_SD_STS, 0xFFU);

cleanup:
    if (g_hda.mmio != 0 && stream != UINT32_MAX) {
        hda_audio_binding_t temporary = {.stream=(audio_stream_t *)1,.stream_offset=stream};
        if (!hda_stop_binding_locked(&temporary)) return false;
    }
    bool cleanup_success = true;
    if (periods != 0) {
        for (uint32_t i = 0; i < HDA_PCM_PERIODS; ++i) {
            if (!hda_free_dma(&periods[i])) cleanup_success = false;
        }
    }
    if (!hda_free_dma(bdl)) cleanup_success = false;
    if (!cleanup_success && g_hda_error == 0U) {
        g_hda_error = HDA_ERROR_PCM_CLEANUP;
    }
    if (cleanup_success) {
        kfree(periods);
        kfree(bdl);
    }
    return success && cleanup_success;
}

bool hda_hardware_present(void) {
    return g_hda_present;
}

struct device *hda_audio_device(void) {
    return g_hda.initialized && g_hda.pci != 0 ?
           (struct device *)&g_hda.pci->device : 0;
}

uint32_t hda_last_error(void) {
    return g_hda_error;
}

uint8_t hda_output_stream_count(void) {
    return g_hda.output_streams;
}

uint8_t hda_input_stream_count(void) {
    return g_hda.input_streams;
}

bool hda_controller_reset(void) {
    bool success;
    if (!g_hda.initialized) return !g_hda_present;
    /* 复位会使旧环的控制器所有权失效，先释放 DMA 映射再重新建立环。 */
    hda_audio_lock(&g_hda);
    if (!hda_stop_binding_locked(&g_hda.playback) ||
        !hda_stop_binding_locked(&g_hda.capture) || !hda_stop_command_rings(&g_hda)) {
        hda_audio_unlock(&g_hda);
        return false;
    }
    bool released_rirb = hda_free_dma(&g_hda.rirb);
    bool released_corb = hda_free_dma(&g_hda.corb);
    if (!released_rirb || !released_corb) {
        hda_audio_unlock(&g_hda);
        return false;
    }
    success = hda_reset_controller(&g_hda) && hda_setup_command_rings(&g_hda);
    if (!success) {
        hda_free_dma(&g_hda.rirb);
        hda_free_dma(&g_hda.corb);
    } else {
        audio_format_t format;
        if (g_hda.playback.stream != 0) {
            audio_stream_get_format(g_hda.playback.stream, &format);
            success = hda_program_binding_locked(&g_hda.playback, &format);
        }
        if (success && g_hda.capture.stream != 0) {
            audio_stream_get_format(g_hda.capture.stream, &format);
            success = hda_program_binding_locked(&g_hda.capture, &format);
        }
    }
    hda_audio_unlock(&g_hda);
    return success;
}

bool hda_hardware_self_test(void) {
    const pci_host_t *host = pci_current_host();
    const pci_device_t *pci = host == 0 ? 0 :
        pci_find_class(host, HDA_CLASS_CODE, HDA_SUBCLASS_CODE, HDA_PROG_IF);
    g_hda_present = pci != 0;
    g_hda_error = 0U;
    if (pci == 0) return true;
    if (!hda_destroy(&g_hda)) return false;
    if (!hda_initialize(&g_hda, pci)) return false;
    if (!hda_pcm_self_test()) {
        if (g_hda_error == 0U) g_hda_error = HDA_ERROR_PCM_CLEANUP;
        hda_destroy(&g_hda);
        return false;
    }
    return true;
}
