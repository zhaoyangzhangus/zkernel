#include "core_internal.h"

bool e1000_hardware_present(void) {
    return e1000_pci_find() != 0 || rtl8126_pci_find() != 0;
}

bool e1000_intel_hardware_present(void) {
    return e1000_pci_find() != 0;
}
#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/dma.h>
#include <kernel/deferred.h>
#include <kernel/e1000.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/pci.h>
#include <kernel/telemetry.h>
#include <kernel/socket.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#define E1000_MMIO_VA (X86_64_MMIO_BASE + 0x10000000ULL)
#define E1000_BUFFER_SIZE 2048U
#define E1000_REG_CTRL   0x0000U
#define E1000_REG_STATUS 0x0008U
#define E1000_REG_MDIC   0x0020U
#define E1000_REG_TCTL   0x0400U
#define E1000_REG_TIPG   0x0410U
#define E1000_REG_RCTL   0x0100U
#define E1000_REG_TDBAL  0x3800U
#define E1000_REG_TDBAH  0x3804U
#define E1000_REG_TDLEN  0x3808U
#define E1000_REG_TDH    0x3810U
#define E1000_REG_TDT    0x3818U
#define E1000_REG_RDBAL  0x2800U
#define E1000_REG_RDBAH  0x2804U
#define E1000_REG_RDLEN  0x2808U
#define E1000_REG_RDH    0x2810U
#define E1000_REG_RAL0   0x5400U
#define E1000_REG_RAH0   0x5404U
#define E1000_REG_IMC    0x00D8U

#define E1000_CTRL_RST  (1U << 26)
#define E1000_CTRL_SLU  (1U << 6)
#define E1000_STATUS_LU (1U << 1)
#define E1000_TCTL_EN   (1U << 1)
#define E1000_TCTL_PSP  (1U << 3)
#define E1000_RCTL_EN   (1U << 1)
#define E1000_RCTL_BAM  (1U << 15)
#define E1000_MDIC_PHY_SHIFT 21U
#define E1000_MDIC_OP_WRITE  (1U << 26)
#define E1000_MDIC_OP_READ   (1U << 27)
#define E1000_MDIC_READY     (1U << 28)
#define E1000_MDIC_ERROR     (1U << 30)
#define E1000_MII_BMCR_LOOPBACK (1U << 14)
#define E1000_TX_CMD_EOP  (1U << 0)
#define E1000_TX_CMD_IFCS (1U << 1)
#define E1000_TX_CMD_RS   (1U << 3)
#define E1000_TX_STATUS_DD (1U << 0)

static e1000_state_t g_e1000;
static uint32_t g_e1000_error;
static atomic_bool g_e1000_poll_queued;
static atomic_bool g_e1000_poll_retry;
/* 这是启动期计数器，不是每包日志。将等待共享延迟工作线程的时间与真正处理一批数据的时间分开统计。 */
static struct {
    atomic_uint_fast64_t queued_tsc;
    atomic_uint_fast64_t samples;
    atomic_uint_fast64_t total_ns;
    atomic_uint_fast64_t max_ns;
    atomic_uint_fast64_t over_1ms;
    atomic_uint_fast64_t over_10ms;
    atomic_uint_fast64_t over_100ms;
} g_e1000_poll_timing;
static atomic_uint g_e1000_lifecycle_lock;
/* 只有在自检完成后，运行时才会进入处理循环；在此之前即使有后台工作也不能接收中断事件。 */
static bool g_e1000_runtime_ready;

void e1000_record_error(uint32_t error) {
    if (g_e1000_error == 0U) g_e1000_error = error;
}

static bool e1000_lifecycle_try_lock(void) {
    /* 不要让延迟工作线程取代本地生命周期所有者，并在该所有者无法释放设备时反复重试 RX。
     * IRQ 仍可能尝试获取锁，但失败时不会等待，并会恢复此前的嵌套状态。 */
    sched_preempt_disable();
    if (atomic_exchange_explicit(&g_e1000_lifecycle_lock, 1U,
                                 memory_order_acquire) == 0U) return true;
    sched_preempt_enable();
    return false;
}

static void e1000_lifecycle_unlock(void) {
    atomic_store_explicit(&g_e1000_lifecycle_lock, 0U, memory_order_release);
    sched_preempt_enable();
}

void e1000_configure_link_local(e1000_state_t *state) {
    if (state == 0) return;
    memset(state->ipv6_address, 0, sizeof(state->ipv6_address));
    state->ipv6_address[0] = 0xFEU;
    state->ipv6_address[1] = 0x80U;
    state->ipv6_address[8] = state->mac[0] ^ 0x02U;
    state->ipv6_address[9] = state->mac[1];
    state->ipv6_address[10] = state->mac[2];
    state->ipv6_address[11] = 0xFFU;
    state->ipv6_address[12] = 0xFEU;
    state->ipv6_address[13] = state->mac[3];
    state->ipv6_address[14] = state->mac[4];
    state->ipv6_address[15] = state->mac[5];
    state->ipv6_address_configured = true;
}

static void e1000_initialize_software_queues(e1000_state_t *state) {
    uint32_t cpu_count = x86_smp_discovered_count();
    if (cpu_count == 0U) cpu_count = 1U;
    if (cpu_count > E1000_SOFTWARE_QUEUE_COUNT) {
        cpu_count = E1000_SOFTWARE_QUEUE_COUNT;
    }
    state->hardware_queue_count = 1U;
    state->software_queue_count = cpu_count;
    for (uint32_t index = 0; index < E1000_SOFTWARE_QUEUE_COUNT; ++index) {
        e1000_queue_init(&state->software_queues[index]);
    }
}

void e1000_state_prepare(e1000_state_t *state, const pci_device_t *pci,
                         net_backend_t backend) {
    if (state == 0) return;
    memset(state, 0, sizeof(*state));
    spinlock_init(&state->tx_lock);
    e1000_initialize_software_queues(state);
    net_arp_cache_init(&state->arp_cache);
    net_ipv6_neighbor_cache_init(&state->ndp_cache);
    state->pci = pci;
    state->backend = backend;
}

void e1000_install_socket_outputs(e1000_state_t *state) {
    if (state == 0) return;
    socket_set_tcp_ipv4_output(e1000_tcp_ipv4_output, state);
    socket_set_tcp_ipv6_output(e1000_tcp_ipv6_output, state);
    socket_set_udp_ipv4_output(e1000_udp_ipv4_output, state);
    socket_set_udp_ipv6_output(e1000_udp_ipv6_output, state);
}

void e1000_remove_socket_outputs(void) {
    socket_set_tcp_ipv4_output(0, 0);
    socket_set_tcp_ipv6_output(0, 0);
    socket_set_udp_ipv4_output(0, 0);
    socket_set_udp_ipv6_output(0, 0);
}

void e1000_runtime_set_ready(bool ready) {
    g_e1000_runtime_ready = ready;
}

static uint32_t e1000_recovery_read(const void *owner, uint32_t offset) {
    return e1000_read((const e1000_state_t *)owner, offset);
}

static void e1000_recovery_write(const void *owner, uint32_t offset,
                                 uint32_t value) {
    e1000_write((const e1000_state_t *)owner, offset, value);
}

static bool e1000_phy_write(e1000_state_t *state, uint8_t reg, uint16_t value) {
    /* I/O port 妯″紡涓?MDIC 涓嶅彲鐢紝涓嶈兘鎶?PHY 鍐欏叆浼鎴愭垚鍔熴€?*/
    if (state == 0 || state->io_mode || reg >= 32U) return false;
    uint32_t command = (uint32_t)value | (1U << E1000_MDIC_PHY_SHIFT) |
                       ((uint32_t)reg << 16) | E1000_MDIC_OP_WRITE;
    e1000_write(state, E1000_REG_MDIC, command);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint32_t result = e1000_read(state, E1000_REG_MDIC);
        if ((result & E1000_MDIC_READY) == 0) {
            __asm__ volatile ("pause");
            continue;
        }
        return (result & E1000_MDIC_ERROR) == 0;
    }
    return false;
}

static bool e1000_enable_phy_loopback(e1000_state_t *state) {
    if (state == 0 || state->io_mode) return false;
    uint32_t command = (1U << E1000_MDIC_PHY_SHIFT) |
                       ((uint32_t)E1000_MII_BMCR << 16) | E1000_MDIC_OP_READ;
    e1000_write(state, E1000_REG_MDIC, command);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        uint32_t result = e1000_read(state, E1000_REG_MDIC);
        if ((result & E1000_MDIC_READY) == 0) {
            __asm__ volatile ("pause");
            continue;
        }
        if ((result & E1000_MDIC_ERROR) != 0) return false;
        state->phy_control = (uint16_t)result;
        if (!e1000_phy_write(state, E1000_MII_BMCR,
                             (uint16_t)(state->phy_control | E1000_MII_BMCR_LOOPBACK))) {
            return false;
        }
        state->phy_loopback = true;
        return true;
    }
    return false;
}

static bool e1000_map_mmio(e1000_state_t *state) {
    uint32_t bar_index = PCI_MAX_BARS;
    for (uint32_t index = 0; index < PCI_MAX_BARS; ++index) {
        if ((state->pci->bars[index].flags & PCI_RESOURCE_MEMORY) != 0 &&
            state->pci->bars[index].length != 0) {
            bar_index = index;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) {
        for (uint32_t index = 0; index < PCI_MAX_BARS; ++index) {
            if ((state->pci->bars[index].flags & PCI_RESOURCE_IO) != 0 &&
                state->pci->bars[index].length >= 8U) {
                state->io_mode = true;
                state->io_base = (uint16_t)state->pci->bars[index].address;
                if (state->pci->bars[index].address > UINT16_MAX) {
                    g_e1000_error = 33U;
                    return false;
                }
                return true;
            }
        }
        g_e1000_error = 30U;
        return false;
    }
    state->mmio_bar = (uint8_t)bar_index;
    uint64_t span = state->pci->bars[bar_index].length;
    uint64_t mapped = 0;
    if ((state->pci->bars[bar_index].flags & PCI_RESOURCE_MEMORY) == 0 || span == 0) {
        g_e1000_error = 34U;
        return false;
    }
    /* e1000 鐨勫瘎瀛樺櫒绐楀彛涓嶈秴杩?128 KiB锛涢檺鍒剁獥鍙ｄ篃閬垮厤寮傚父 BAR 闀垮害
       鎶婁竴涓崯鍧忕殑 PCI 璧勬簮鎻忚堪鎵╁睍鎴愯秴澶ф槧灏勩€?*/
    if (span > 0x20000ULL) span = 0x20000ULL;
    span = (span + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (span > X86_64_MMIO_END - E1000_MMIO_VA + 1ULL) {
        g_e1000_error = 31U;
        return false;
    }
    while (mapped < span) {
        if (x86_map_page(x86_current_root_table(), (vaddr_t)(E1000_MMIO_VA + mapped),
                         paddr_make(state->pci->bars[bar_index].address + mapped),
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
            while (mapped != 0) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(),
                                     (vaddr_t)(E1000_MMIO_VA + mapped), 0);
            }
            g_e1000_error = 32U;
            return false;
        }
        mapped += PAGE_SIZE;
    }
    state->mmio = (volatile uint8_t *)(uintptr_t)E1000_MMIO_VA;
    state->mmio_span = span;
    return true;
}

static void e1000_unmap_mmio(e1000_state_t *state) {
    if (state == 0 || state->io_mode || state->mmio == 0) return;
    for (uint64_t offset = 0; offset < state->mmio_span; offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(),
                             (vaddr_t)(E1000_MMIO_VA + offset), 0);
    }
    state->mmio = 0;
    state->mmio_span = 0;
}

static bool e1000_map_page_dma(const pci_device_t *pci, page_t *page,
                               enum dma_direction direction, dma_mapping_t *mapping) {
    page_t *pages[1] = {page};
    return dma_map_pages((device_t *)&pci->device, pages, 1U, direction, mapping) == K_OK;
}

static uint64_t e1000_dma_address(const dma_mapping_t *mapping) {
    return mapping->segment_count == 0 ? 0 : mapping->segments[0].addr.value;
}

static bool e1000_release_dma(e1000_state_t *state) {
    bool success = true;
    if (state == 0) return false;
    for (uint32_t i = 0; i < E1000_RING_COUNT; ++i) {
        bool tx_released = state->tx_dma[i].device == 0;
        bool rx_released = state->rx_dma[i].device == 0;
        if (!tx_released) {
            tx_released = dma_unmap_checked(&state->tx_dma[i]) == K_OK;
            if (!tx_released) success = false;
        }
        if (!rx_released) {
            rx_released = dma_unmap_checked(&state->rx_dma[i]) == K_OK;
            if (!rx_released) success = false;
        }
        if (tx_released) {
            if (state->tx_pages[i] != 0) page_free(state->tx_pages[i]);
            state->tx_pages[i] = 0;
        }
        if (rx_released) {
            if (state->rx_pages[i] != 0) page_free(state->rx_pages[i]);
            state->rx_pages[i] = 0;
        }
    }
    bool tx_ring_released = state->tx_ring_dma.device == 0;
    bool rx_ring_released = state->rx_ring_dma.device == 0;
    if (!tx_ring_released) {
        tx_ring_released = dma_unmap_checked(&state->tx_ring_dma) == K_OK;
        if (!tx_ring_released) success = false;
    }
    if (!rx_ring_released) {
        rx_ring_released = dma_unmap_checked(&state->rx_ring_dma) == K_OK;
        if (!rx_ring_released) success = false;
    }
    if (tx_ring_released) {
        if (state->tx_ring_page != 0) page_free(state->tx_ring_page);
        state->tx_ring_page = 0;
        state->tx_ring = 0;
    }
    if (rx_ring_released) {
        if (state->rx_ring_page != 0) page_free(state->rx_ring_page);
        state->rx_ring_page = 0;
        state->rx_ring = 0;
    }
    return success;
}

static bool e1000_alloc_dma(e1000_state_t *state) {
    state->tx_ring_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    state->rx_ring_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (state->tx_ring_page == 0 || state->rx_ring_page == 0 ||
        !e1000_map_page_dma(state->pci, state->tx_ring_page, DMA_BIDIRECTIONAL,
                            &state->tx_ring_dma) ||
        !e1000_map_page_dma(state->pci, state->rx_ring_page, DMA_BIDIRECTIONAL,
                            &state->rx_ring_dma)) return false;
    state->tx_ring = (e1000_descriptor_t *)phys_to_direct(page_to_phys(state->tx_ring_page));
    state->rx_ring = (e1000_descriptor_t *)phys_to_direct(page_to_phys(state->rx_ring_page));
    for (uint32_t i = 0; i < E1000_RING_COUNT; ++i) {
        state->tx_pages[i] = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
        state->rx_pages[i] = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
        if (state->tx_pages[i] == 0 || state->rx_pages[i] == 0 ||
            !e1000_map_page_dma(state->pci, state->tx_pages[i], DMA_TO_DEVICE,
                                &state->tx_dma[i]) ||
            !e1000_map_page_dma(state->pci, state->rx_pages[i], DMA_FROM_DEVICE,
                                &state->rx_dma[i])) return false;
        state->tx_ring[i].address = e1000_dma_address(&state->tx_dma[i]);
        state->tx_ring[i].status = E1000_TX_STATUS_DD;
        state->rx_ring[i].address = e1000_dma_address(&state->rx_dma[i]);
        state->rx_ring[i].status = 0;
    }
    dma_sync_for_device(&state->tx_ring_dma);
    dma_sync_for_device(&state->rx_ring_dma);
    dma_wmb();
    return true;
}

static bool e1000_initialize(e1000_state_t *state, const pci_device_t *pci) {
    if (state == 0 || pci == 0 || state->initialized) {
        g_e1000_error = 1U;
        return false;
    }
    e1000_state_prepare(state, pci, NET_BACKEND_E1000);
    if (pci_enable_memory_busmaster((pci_device_t *)pci) != K_OK) {
        g_e1000_error = 2U;
        return false;
    }
    if (!e1000_map_mmio(state)) {
        if (g_e1000_error == 0U) g_e1000_error = 3U;
        return false;
    }
    e1000_write(state, E1000_REG_IMC, UINT32_MAX);
    e1000_write(state, E1000_REG_CTRL, E1000_CTRL_RST);
    for (uint32_t spin = 0; spin < 100000U; ++spin) {
        if ((e1000_read(state, E1000_REG_CTRL) & E1000_CTRL_RST) == 0) break;
        __asm__ volatile ("pause");
    }
    uint32_t ral = e1000_read(state, E1000_REG_RAL0);
    uint32_t rah = e1000_read(state, E1000_REG_RAH0);
    state->mac[0] = (uint8_t)ral;
    state->mac[1] = (uint8_t)(ral >> 8);
    state->mac[2] = (uint8_t)(ral >> 16);
    state->mac[3] = (uint8_t)(ral >> 24);
    state->mac[4] = (uint8_t)rah;
    state->mac[5] = (uint8_t)(rah >> 8);
    e1000_configure_link_local(state);
    if (!e1000_alloc_dma(state)) {
        g_e1000_error = 4U;
        (void)e1000_release_dma(state);
        e1000_unmap_mmio(state);
        return false;
    }
    e1000_write(state, E1000_REG_TDBAL, (uint32_t)e1000_dma_address(&state->tx_ring_dma));
    e1000_write(state, E1000_REG_TDBAH, (uint32_t)(e1000_dma_address(&state->tx_ring_dma) >> 32));
    e1000_write(state, E1000_REG_TDLEN, E1000_RING_COUNT * sizeof(e1000_descriptor_t));
    e1000_write(state, E1000_REG_TDH, 0);
    e1000_write(state, E1000_REG_TDT, 0);
    e1000_write(state, E1000_REG_RDBAL, (uint32_t)e1000_dma_address(&state->rx_ring_dma));
    e1000_write(state, E1000_REG_RDBAH, (uint32_t)(e1000_dma_address(&state->rx_ring_dma) >> 32));
    e1000_write(state, E1000_REG_RDLEN, E1000_RING_COUNT * sizeof(e1000_descriptor_t));
    e1000_write(state, E1000_REG_RDH, 0);
    e1000_write(state, E1000_REG_RDT, E1000_RING_COUNT - 1U);
    e1000_write(state, E1000_REG_TIPG, 0x00702008U);
    e1000_write(state, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                      (0x10U << 4) | (0x40U << 12));
    e1000_write(state, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM);
    e1000_write(state, E1000_REG_CTRL, E1000_CTRL_SLU);
    state->link_up = (e1000_read(state, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    net_device_init(&state->device, "e1000", state->mac, 1500U,
                    e1000_device_transmit, state);
    state->device.link_up = state->link_up;
    state->initialized = true;
    e1000_install_socket_outputs(state);
    /* 自测完成后才绑定中断，避免启动自测产生的回环流量进入运行时队列。 */
    return true;
}

static bool e1000_stop_controller(e1000_state_t *state) {
    bool reset = false;
    e1000_write(state, E1000_REG_IMC, UINT32_MAX);
    e1000_write(state, E1000_REG_TCTL, 0U);
    e1000_write(state, E1000_REG_RCTL, 0U);
    e1000_write(state, E1000_REG_CTRL, E1000_CTRL_RST);
    for (uint32_t spin = 0U; spin < 100000U; ++spin) {
        uint32_t control = e1000_read(state, E1000_REG_CTRL);
        if (control == UINT32_MAX) break;
        if ((control & E1000_CTRL_RST) == 0U) {
            reset = true;
            break;
        }
        __asm__ volatile ("pause");
    }
    /* 即使复位超时，也始终尝试阻止新的 DMA。除 BME 外保留 PCI Command 原值，且绝不能意外确认 W1C 状态位。 */
    uint32_t command;
    if (pci_read_config32(state->pci, 4U, &command) != K_OK ||
        command == UINT32_MAX) return false;
    command = (uint16_t)command & (uint16_t)~4U;
    if (pci_write_config32(state->pci, 4U, command) != K_OK ||
        pci_read_config32(state->pci, 4U, &command) != K_OK ||
        command == UINT32_MAX || (command & 4U) != 0U) return false;
    ((pci_device_t *)state->pci)->command = (uint16_t)command;
    return reset;
}

static bool e1000_destroy(e1000_state_t *state) {
    if (state == 0) return true;
    if (state->backend == NET_BACKEND_RTL8126) {
        atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
        return rtl8126_destroy_state(state);
    }
    if (!state->initialized) return true;
    g_e1000_runtime_ready = false;
    e1000_remove_socket_outputs();
    atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
    e1000_recovery_unbind(&state->recovery);
    if (!e1000_stop_controller(state)) return false;
    if (state->phy_loopback) {
        (void)e1000_phy_write(state, E1000_MII_BMCR, state->phy_control);
        state->phy_loopback = false;
    }
    if (!e1000_release_dma(state)) return false;
    e1000_unmap_mmio(state);
    state->initialized = false;
    return true;
}

static bool e1000_transmit_locked(e1000_state_t *state, const uint8_t *frame, size_t length) {
    if (state == 0 || !state->initialized || frame == 0 || length == 0 ||
        length > E1000_BUFFER_SIZE) return false;
    e1000_descriptor_t *descriptor = &state->tx_ring[state->tx_tail];
    if ((descriptor->status & E1000_TX_STATUS_DD) == 0) return false;
    uint8_t *buffer = (uint8_t *)phys_to_direct(page_to_phys(state->tx_pages[state->tx_tail]));
    memcpy(buffer, frame, length);
    descriptor->length = (uint16_t)length;
    descriptor->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    descriptor->status = 0;
    dma_sync_for_device(&state->tx_dma[state->tx_tail]);
    dma_wmb();
    state->tx_tail = (state->tx_tail + 1U) % E1000_RING_COUNT;
    e1000_write(state, E1000_REG_TDT, state->tx_tail);

    /*
    * TX 提交为异步操作。槽位复用前会检查 DD，因此在硬件门铃响起后于此等待只会阻塞调用方。
     */
    return true;
}

kstatus_t e1000_transmit_status(e1000_state_t *state, const uint8_t *frame,
                               size_t length) {
    if (state == 0 || frame == 0 || length == 0U ||
        length > E1000_BUFFER_SIZE) return K_EINVAL;
    /* Socket 输出也会在不持有 RX/生命周期锁时进入此处。描述符选择、负载复制和门铃通知必须作为一个事务。 */
    sched_preempt_disable();
    spinlock_lock(&state->tx_lock);
    kstatus_t status = K_EIO;
    if (state->initialized) {
        if (state->backend == NET_BACKEND_RTL8126) {
            status = rtl8126_transmit_status(state, frame, length);
        } else if (state->backend == NET_BACKEND_E1000 && state->tx_ring != 0) {
            dma_sync_for_cpu(&state->tx_ring_dma);
            if ((state->tx_ring[state->tx_tail].status & E1000_TX_STATUS_DD) == 0U)
                status = K_EAGAIN;
            else if (e1000_transmit_locked(state, frame, length))
                status = K_OK;
        }
    }
    spinlock_unlock(&state->tx_lock);
    sched_preempt_enable();
    return status;
}

bool e1000_transmit(e1000_state_t *state, const uint8_t *frame, size_t length) {
    return e1000_transmit_status(state, frame, length) == K_OK;
}

/* 只短暂读取活动后端身份，不访问未接管的其他网卡寄存器。 */
kstatus_t e1000_active_adapter(const struct pci_device **device) {
    if (!device) return K_EINVAL;
    *device=0;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (g_e1000.initialized) *device=g_e1000.pci;
    e1000_lifecycle_unlock();
    return K_OK;
}

e1000_state_t *e1000_controller_state(void) {
    return &g_e1000;
}

void e1000_self_test_begin(void) {
    g_e1000_runtime_ready = false;
    atomic_init(&g_e1000_poll_queued, false);
    atomic_init(&g_e1000_poll_retry, false);
    g_e1000_error = 0U;
}

bool e1000_self_test_initialize(e1000_state_t *state, const pci_device_t *pci) {
    return e1000_initialize(state, pci);
}

bool e1000_self_test_destroy(e1000_state_t *state) {
    return e1000_destroy(state);
}

bool e1000_self_test_enable_phy_loopback(e1000_state_t *state) {
    return e1000_enable_phy_loopback(state);
}

bool e1000_self_test_restore_phy(e1000_state_t *state, uint16_t value) {
    return e1000_phy_write(state, E1000_MII_BMCR, value);
}

bool e1000_self_test_poll_receive(e1000_state_t *state) {
    return e1000_poll_receive(state);
}

uint32_t e1000_self_test_recovery_read(const void *owner, uint32_t offset) {
    return e1000_recovery_read(owner, offset);
}

void e1000_self_test_recovery_write(const void *owner, uint32_t offset,
                                    uint32_t value) {
    e1000_recovery_write(owner, offset, value);
}

bool e1000_rss_self_test(void) {
    if (!e1000_hardware_present()) return true;
    if (!g_e1000.initialized) return false;
    return e1000_rss_self_test_state(g_e1000.software_queue_count);
}

uint32_t e1000_hardware_queue_count(void) {
    return g_e1000.initialized ? g_e1000.hardware_queue_count : 0U;
}

uint32_t e1000_software_queue_count(void) {
    return g_e1000.initialized ? g_e1000.software_queue_count : 0U;
}

kstatus_t e1000_send_frame(const void *frame, size_t length) {
    bool sent;
    if (frame == 0 || length == 0U || length > E1000_BUFFER_SIZE) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized || !g_e1000_runtime_ready) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    /* 生命周期所有权已经串行化 TX、RX 和设备复位。 */
    sent = net_device_send(&g_e1000.device, frame, length) == K_OK;
    e1000_lifecycle_unlock();
    return sent ? K_OK : K_EIO;
}

kstatus_t e1000_get_mac_address(uint8_t mac[6]) {
    if (mac == 0) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    memcpy(mac, g_e1000.mac, 6U);
    e1000_lifecycle_unlock();
    return K_OK;
}

kstatus_t e1000_set_ipv4_address(uint32_t address) {
    return e1000_set_ipv4_config(address, 0U, 0U);
}

kstatus_t e1000_set_ipv4_config(uint32_t address, uint8_t prefix_length,
                                uint32_t gateway) {
    if (prefix_length > 32U) return K_EINVAL;
    if (gateway != 0U && address == 0U) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    g_e1000.ipv4_address = address;
    g_e1000.ipv4_prefix_length = prefix_length;
    g_e1000.ipv4_gateway = gateway;
    e1000_lifecycle_unlock();
    return K_OK;
}

uint32_t e1000_ipv4_address(void) {
    uint32_t address = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) address = g_e1000.ipv4_address;
    e1000_lifecycle_unlock();
    return address;
}

uint8_t e1000_ipv4_prefix_length(void) {
    uint8_t prefix_length = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) prefix_length = g_e1000.ipv4_prefix_length;
    e1000_lifecycle_unlock();
    return prefix_length;
}

uint32_t e1000_ipv4_gateway(void) {
    uint32_t gateway = 0U;
    if (!e1000_lifecycle_try_lock()) return 0U;
    if (g_e1000.initialized) gateway = g_e1000.ipv4_gateway;
    e1000_lifecycle_unlock();
    return gateway;
}

kstatus_t e1000_set_ipv6_address(const uint8_t address[16]) {
    if (address == 0 || e1000_address6_zero(address)) return K_EINVAL;
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return K_ENOENT;
    }
    memcpy(g_e1000.ipv6_address, address, 16U);
    g_e1000.ipv6_address_configured = true;
    e1000_lifecycle_unlock();
    return K_OK;
}

bool e1000_ipv6_address(uint8_t address[16]) {
    bool configured = false;
    if (address == 0 || !e1000_lifecycle_try_lock()) return false;
    if (g_e1000.initialized && g_e1000.ipv6_address_configured) {
        memcpy(address, g_e1000.ipv6_address, 16U);
        configured = true;
    }
    e1000_lifecycle_unlock();
    return configured;
}

bool e1000_reset(void) {
    const pci_device_t *pci;
    net_backend_t backend;
    uint32_t ipv4_address;
    uint8_t ipv4_prefix_length;
    uint32_t ipv4_gateway;
    uint8_t ipv6_address[16];
    bool ipv6_address_configured;
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized || g_e1000.pci == 0) {
        e1000_lifecycle_unlock();
        return false;
    }
    pci = g_e1000.pci;
    backend = g_e1000.backend;
    ipv4_address = g_e1000.ipv4_address;
    ipv4_prefix_length = g_e1000.ipv4_prefix_length;
    ipv4_gateway = g_e1000.ipv4_gateway;
    memcpy(ipv6_address, g_e1000.ipv6_address, sizeof(ipv6_address));
    ipv6_address_configured = g_e1000.ipv6_address_configured;
    g_e1000_runtime_ready = false;
    if (!e1000_destroy(&g_e1000)) {
        g_e1000_runtime_ready = false;
        e1000_lifecycle_unlock();
        return false;
    }
    if (backend == NET_BACKEND_RTL8126) {
        if (!rtl8126_initialize_state(&g_e1000, pci) ||
            !rtl8126_bind_interrupt_state(&g_e1000)) {
            g_e1000_runtime_ready = false;
            if (g_e1000.initialized) (void)rtl8126_destroy_state(&g_e1000);
            if (g_e1000_error == 0U) g_e1000_error = 0x812607U;
            e1000_lifecycle_unlock();
            return false;
        }
    } else if (!e1000_initialize(&g_e1000, pci)) {
        g_e1000_runtime_ready = false;
        if (g_e1000_error == 0U) g_e1000_error = 7U;
        e1000_lifecycle_unlock();
        return false;
    }
    /* reset 浼氶噸寤?DMA 鐘舵€侊紝浣嗕笉鑳戒涪澶辩綉缁滅鐞嗗櫒宸茬粡閰嶇疆鐨勫湴鍧€銆?*/
    g_e1000.ipv4_address = ipv4_address;
    g_e1000.ipv4_prefix_length = ipv4_prefix_length;
    g_e1000.ipv4_gateway = ipv4_gateway;
    memcpy(g_e1000.ipv6_address, ipv6_address, sizeof(ipv6_address));
    g_e1000.ipv6_address_configured = ipv6_address_configured;
    /* Reset 鍚庨噸鏂板彂甯冭繍琛屾€侊紱閾捐矾鐘舵€佺敱涓柇宸ヤ綔椤?鍏煎绠＄悊鍣ㄧ‘璁ゃ€?*/
    /* 部分旧款 e1000 功能仅提供 INTx，没有 MSI 能力。为这些设备保留有界兼容路径；PCI 功能声明支持 MSI
     * 时则使用 MSI。 */
    if (backend == NET_BACKEND_E1000) {
        (void)e1000_recovery_bind(&g_e1000.recovery, g_e1000.pci, &g_e1000,
                                  e1000_recovery_read, e1000_recovery_write);
    }
    g_e1000_runtime_ready = true;
    e1000_lifecycle_unlock();
    return true;
}

kstatus_t e1000_shutdown(void) {
    if (!e1000_lifecycle_try_lock()) return K_EBUSY;
    g_e1000_runtime_ready = false;
    atomic_store_explicit(&g_e1000_poll_retry, false, memory_order_release);
    bool stopped = e1000_destroy(&g_e1000);
    e1000_lifecycle_unlock();
    return stopped ? K_OK : K_EIO;
}

bool e1000_link_up(void) {
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized) {
        e1000_lifecycle_unlock();
        return false;
    }
    /* 保留生命周期所有权（复位可能取消这些寄存器的映射），但只读链路查询不得在缓慢的 MMIO 访问期间
     * 固定当前 CPU。仅释放 try_lock 获取的嵌套层级，外层保护仍保持。延迟 RX 遇到竞争时记录重试，而不
     * 是立即对被抢占的生命周期所有者自旋。 */
    sched_preempt_enable();
    bool link_up = g_e1000.backend == NET_BACKEND_RTL8126 ?
        rtl8126_link_up_state(&g_e1000) :
        (e1000_read(&g_e1000, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    sched_preempt_disable();
    g_e1000.link_up = link_up;
    g_e1000.device.link_up = g_e1000.link_up;
    e1000_lifecycle_unlock();
    return link_up;
}

static bool e1000_receive_mode_matches(bool interrupt_driven) {
    /* 发生竞争表示模式未知，而非中断缺失。任何查询都不得将繁忙的中断驱动网卡改为轮询网卡。 */
    if (!e1000_lifecycle_try_lock()) return false;
    bool matches = false;
    if (g_e1000.initialized && g_e1000_runtime_ready) {
        if (g_e1000.backend == NET_BACKEND_RTL8126)
            matches = g_e1000.rtl_irq_bound == interrupt_driven;
        else if (g_e1000.backend == NET_BACKEND_E1000)
            matches = g_e1000.recovery.irq_bound == interrupt_driven;
    }
    e1000_lifecycle_unlock();
    return matches;
}

bool e1000_poll(void) {
    bool received;
    uint64_t start_tsc;
    if (!e1000_lifecycle_try_lock()) return false;
    if (!g_e1000.initialized || !g_e1000_runtime_ready) {
        e1000_lifecycle_unlock();
        return false;
    }
    /* 生命周期所有权是 TX/RX 唯一的互斥边界。 */
    start_tsc = telemetry_timestamp();
    if (g_e1000.backend == NET_BACKEND_RTL8126) {
        g_e1000.link_up = rtl8126_link_up_state(&g_e1000);
    } else {
        g_e1000.link_up = (e1000_read(&g_e1000, E1000_REG_STATUS) & E1000_STATUS_LU) != 0U;
    }
    g_e1000.device.link_up = g_e1000.link_up;
    if (!g_e1000.link_up) {
        e1000_lifecycle_unlock();
        (void)telemetry_record_latency(TELEMETRY_CATEGORY_NETWORK_BATCH,
                                       g_e1000.pci != 0 ?
                                           g_e1000.pci->device.device_id : 0U,
                                       start_tsc);
        return false;
    }
    received = g_e1000.backend == NET_BACKEND_RTL8126 ?
               rtl8126_poll_receive_state(&g_e1000) :
               e1000_poll_receive(&g_e1000);
    e1000_lifecycle_unlock();
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_NETWORK_BATCH,
                                   g_e1000.pci != 0 ?
                                       g_e1000.pci->device.device_id : 0U,
                                   start_tsc);
    return received;
}

bool e1000_interrupt_ready(void) {
    return e1000_receive_mode_matches(true);
}

bool e1000_polling_required(void) {
    return e1000_receive_mode_matches(false);
}

static void e1000_record_poll_wait(void) {
    uint64_t queued = atomic_load_explicit(&g_e1000_poll_timing.queued_tsc,
                                           memory_order_acquire);
    if (queued == 0U) return;
    uint64_t delay = x86_tsc_to_ns(telemetry_timestamp() - queued);
    atomic_fetch_add_explicit(&g_e1000_poll_timing.samples, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_e1000_poll_timing.total_ns, delay, memory_order_relaxed);
    uint_fast64_t maximum = atomic_load_explicit(&g_e1000_poll_timing.max_ns,
                                                memory_order_relaxed);
    while (maximum < delay &&
           !atomic_compare_exchange_weak_explicit(&g_e1000_poll_timing.max_ns,
               &maximum, delay, memory_order_relaxed, memory_order_relaxed)) {}
    if (delay >= 1000000ULL)
        atomic_fetch_add_explicit(&g_e1000_poll_timing.over_1ms, 1U, memory_order_relaxed);
    if (delay >= 10000000ULL)
        atomic_fetch_add_explicit(&g_e1000_poll_timing.over_10ms, 1U, memory_order_relaxed);
    if (delay >= 100000000ULL)
        atomic_fetch_add_explicit(&g_e1000_poll_timing.over_100ms, 1U, memory_order_relaxed);
}

bool e1000_schedule_deferred_poll(void) {
    bool expected = false;
    if (!g_e1000_runtime_ready ||
        !atomic_compare_exchange_strong_explicit(&g_e1000_poll_queued, &expected,
                                                 true, memory_order_acq_rel,
                                                 memory_order_acquire)) return false;
    /* 先发布状态再放入队列：其他 CPU 可能在生产者返回前就开始执行。合并的 IRQ 不得改动原始时间戳。 */
    atomic_store_explicit(&g_e1000_poll_timing.queued_tsc, telemetry_timestamp(),
                           memory_order_release);
    if (!deferred_try_schedule(e1000_deferred_poll, 0)) {
        atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);
        /* ISR 已清除中断原因。共享队列已满时仍须保留请求，因为不保证还会有另一个 IRQ 到达。 */
        atomic_store_explicit(&g_e1000_poll_retry, true, memory_order_release);
        return false;
    }
    return true;
}

void e1000_retry_deferred_poll(void) {
    if (atomic_exchange_explicit(&g_e1000_poll_retry, false,
                                 memory_order_acq_rel)) {
        (void)e1000_schedule_deferred_poll();
    }
}

void e1000_deferred_poll(void *argument) {
    (void)argument;
    e1000_record_poll_wait();
    (void)e1000_poll();
    atomic_store_explicit(&g_e1000_poll_queued, false, memory_order_release);

    /* 此工作项运行期间可能到达 RX 中断，此时再次入队会看到 g_e1000_poll_queued 已设置并合并请求。如果
     * 数据包恰好在 RX 循环变空后到达，中断已确认且不保证再有新中断。清除排队标志后补查，以关闭此间隙。 */
    if (!e1000_lifecycle_try_lock()) {
        /* 可抢占的链路查询可能在此 CPU 上拥有设备。不要在其前方递归重新排入 RX；即使没有新中断到达，
         * 管理器节拍也会重试这个已确认的请求。 */
        atomic_store_explicit(&g_e1000_poll_retry, true, memory_order_release);
        return;
    }
    bool pending = false;
    if (g_e1000.initialized && g_e1000_runtime_ready &&
        g_e1000.backend == NET_BACKEND_E1000 && g_e1000.rx_ring != 0) {
        dma_sync_for_cpu(&g_e1000.rx_ring_dma);
        pending = (g_e1000.rx_ring[g_e1000.rx_clean].status &
                   E1000_RX_STATUS_DD) != 0U;
    } else if (g_e1000.initialized && g_e1000_runtime_ready &&
               g_e1000.backend == NET_BACKEND_RTL8126 &&
               g_e1000.rtl_rx_ring != 0) {
        dma_sync_for_cpu(&g_e1000.rtl_rx_ring_dma);
        /* RTL 使用反向所有权语义：OWN 位清零表示归我们所有。检查下一个描述符而非槽位 0，并包含环形回绕情况。 */
        pending = (g_e1000.rtl_rx_ring[g_e1000.rtl_rx_next].options1 &
                   (1U << 31)) == 0U;
        /* 重新启用 MSI 前先发布空闲状态。待处理的 TX/链路/RX 事件必须能够排入后继工作项，而非合并到当前
         * 工作线程。需要继续处理 RX 预算时保持 IRQ 屏蔽。 */
        if (!pending) rtl8126_rearm_interrupts(&g_e1000);
    }
    e1000_lifecycle_unlock();
    if (pending) (void)e1000_schedule_deferred_poll();
}

uint32_t e1000_last_error(void) {
    return g_e1000_error;
}
