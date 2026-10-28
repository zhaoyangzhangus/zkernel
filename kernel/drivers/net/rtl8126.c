#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/dma.h>
#include <kernel/e1000.h>
#include <kernel/irq.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/pci.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/console.h>

#include "core_internal.h"

#define RTL8126_VENDOR_ID 0x10ECU
#define RTL8126_DEVICE_ID 0x8126U
#define RTL8126_MMIO_VA (X86_64_MMIO_BASE + 0x18000000ULL)
#define RTL8126_MMIO_LIMIT 0x20000ULL

#define RTL8126_REG_MAC0          0x0000U
#define RTL8126_REG_TX_DESC_LO    0x0020U
#define RTL8126_REG_INT_CFG       0x0034U
#define RTL8126_REG_CHIP_CMD      0x0037U
#define RTL8126_REG_INT_MASK      0x0038U
#define RTL8126_REG_INT_STATUS    0x003CU
#define RTL8126_REG_INT_MASK_V2_CLEAR 0x0D00U
#define RTL8126_REG_INT_STATUS_V2     0x0D04U
#define RTL8126_REG_INT_MASK_V2_SET   0x0D0CU
#define RTL8126_REG_TX_CONFIG     0x0040U
#define RTL8126_REG_RX_CONFIG     0x0044U
#define RTL8126_REG_CFG9346       0x0050U
#define RTL8126_REG_PHY_STATUS    0x006CU
#define RTL8126_REG_TX_POLL       0x0090U
#define RTL8126_REG_RX_MAX_SIZE   0x00DAU
#define RTL8126_REG_CPLUS_CMD     0x00E0U
#define RTL8126_REG_RX_DESC_LO    0x00E4U
#define RTL8126_REG_MAC0_BACKUP   0x19E0U
#define RTL8126_REG_CSIDR         0x0064U
#define RTL8126_REG_CSIAR         0x0068U
#define RTL8126_REG_OCPDR         0x00B0U
#define RTL8126_REG_GPHY_OCP      0x00B8U
#define RTL8126_REG_CONFIG1       0x0052U
#define RTL8126_REG_CONFIG3       0x0054U
#define RTL8126_REG_CONFIG5       0x0056U
#define RTL8126_REG_MAR0          0x0008U
#define RTL8126_REG_RSS_CTRL      0x4500U
#define RTL8126_REG_QUEUE_CTRL    0x4800U
#define RTL8126_REG_INT_CFG1      0x007AU
#define RTL8126_REG_MISC          0x00F0U
#define RTL8126_REG_MCU           0x00D3U
#define RTL8126_REG_FIFO_STATUS   0x00E2U

#define RTL8126_CHIP_RESET  (1U << 4)
#define RTL8126_CHIP_RX_EN  (1U << 3)
#define RTL8126_CHIP_TX_EN  (1U << 2)
#define RTL8126_CHIP_STOP_REQ (1U << 7)
#define RTL8126_TX_MIN_FRAME_SIZE 60U
#define RTL8126_PHY_LINK     (1U << 1)
#define RTL8126_CONFIG3_L23   (1U << 1)
#define RTL8126_MCU_NOW_IS_OOB (1U << 7)
#define RTL8126_MCU_TX_EMPTY   (1U << 5)
#define RTL8126_MCU_RX_EMPTY   (1U << 4)
#define RTL8126_MCU_RXTX_EMPTY (RTL8126_MCU_TX_EMPTY | RTL8126_MCU_RX_EMPTY)
#define RTL8126_MCU_LINK_LIST_READY (1U << 1)
#define RTL8126_MISC_RXDV_GATED_EN (1U << 19)
#define RTL8126_INT_CFG_ENABLE (1U << 0)

#define RTL8126_CSI_FLAG       0x80000000U
#define RTL8126_CSI_WRITE      0x80000000U
#define RTL8126_CSI_BYTE_ENABLE 0x0000F000U
#define RTL8126_CSI_ADDR_MASK  0x00000FFFU
#define RTL8126_OCP_FLAG       0x80000000U
#define RTL8126_PHY_OCP_BASE   0xA400U
#define RTL8126_XID_MASK       0x7CFU
#define RTL8126_XID_2          0x649U
#define RTL8126_XID_3          0x64AU
#define RTL8126_FIRMWARE_MAX_BYTES (1024U * 1024U)

enum rtl8126_firmware_opcode {
    RTL8126_FW_PHY_READ = 0x0U,
    RTL8126_FW_DATA_OR = 0x1U,
    RTL8126_FW_DATA_AND = 0x2U,
    RTL8126_FW_BACKWARD_JUMP = 0x3U,
    RTL8126_FW_MDIO_CHANGE = 0x4U,
    RTL8126_FW_CLEAR_READ_COUNT = 0x7U,
    RTL8126_FW_PHY_WRITE = 0x8U,
    RTL8126_FW_READ_COUNT_SKIP = 0x9U,
    RTL8126_FW_COMPARE_SKIP = 0xAU,
    RTL8126_FW_COMPARE_NOT_SKIP = 0xBU,
    RTL8126_FW_WRITE_PREVIOUS = 0xCU,
    RTL8126_FW_SKIP = 0xDU,
    RTL8126_FW_DELAY_MS = 0xEU,
};

typedef struct __attribute__((packed)) rtl8126_firmware_header {
    uint32_t magic;
    char version[32];
    uint32_t action_offset;
    uint32_t action_count;
    uint8_t checksum;
} rtl8126_firmware_header_t;

_Static_assert(sizeof(rtl8126_firmware_header_t) == 45U,
               "RTL8126 firmware header ABI");

#define RTL8126_DESC_OWN     (1U << 31)
#define RTL8126_DESC_EOR     (1U << 30)
#define RTL8126_DESC_FS      (1U << 29)
#define RTL8126_DESC_LS      (1U << 28)
#define RTL8126_DESC_LENGTH  0x3FFFU
#define RTL8126_RX_ERROR_MASK ((1U << 19) | (1U << 20) | \
                               (1U << 21) | (1U << 22))

#define RTL8126_INT_RX_OK       (1U << 0)
#define RTL8126_INT_RX_ERR      (1U << 1)
#define RTL8126_INT_TX_OK       (1U << 2)
#define RTL8126_INT_TX_ERR      (1U << 3)
#define RTL8126_INT_LINK_CHANGE (1U << 5)
#define RTL8126_INT_MASK_VALUE  (RTL8126_INT_RX_OK | RTL8126_INT_RX_ERR | \
                                 RTL8126_INT_TX_OK | RTL8126_INT_TX_ERR | \
                                 RTL8126_INT_LINK_CHANGE)
#define RTL8126_INT_V2_RX_OK       (1U << 0)
#define RTL8126_INT_V2_LINK_CHANGE (1U << 21)
#define RTL8126_INT_V2_MASK        (RTL8126_INT_V2_RX_OK | \
                                    RTL8126_INT_V2_LINK_CHANGE)

#define RTL8126_RX_PAGE_COUNT \
    ((RTL8126_RING_COUNT * RTL8126_RX_BUFFER_SIZE) / PAGE_SIZE)
_Static_assert(RTL8126_RING_COUNT * sizeof(rtl8126_descriptor_t) <= PAGE_SIZE,
               "RTL8126 descriptor rings must fit their one-page DMA allocation");
#define RTL8126_IRQ_VECTOR 0x51U
#define RTL8126_ERROR_BASE 0x81260000U

const pci_device_t *rtl8126_pci_find(void) {
    const pci_host_t *host = pci_current_host();
    if (host == 0) return 0;
    return pci_find_device(host, RTL8126_VENDOR_ID, RTL8126_DEVICE_ID);
}

bool rtl8126_hardware_present(void) {
    return rtl8126_pci_find() != 0;
}

static bool rtl8126_reg_valid(const e1000_state_t *state, uint32_t offset,
                              uint32_t size) {
    return state != 0 && state->rtl_mmio != 0 &&
           (uint64_t)offset + size <= state->rtl_mmio_span;
}

static uint8_t rtl8126_read8(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint8_t))) return 0xFFU;
    return *(volatile const uint8_t *)(state->rtl_mmio + offset);
}

static uint16_t rtl8126_read16(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint16_t))) return UINT16_MAX;
    return *(volatile const uint16_t *)(state->rtl_mmio + offset);
}

static uint32_t rtl8126_read32(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return UINT32_MAX;
    return *(volatile const uint32_t *)(state->rtl_mmio + offset);
}

static void rtl8126_write8(const e1000_state_t *state, uint32_t offset,
                           uint8_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint8_t))) return;
    *(volatile uint8_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static void rtl8126_write16(const e1000_state_t *state, uint32_t offset,
                            uint16_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint16_t))) return;
    *(volatile uint16_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static void rtl8126_write32(const e1000_state_t *state, uint32_t offset,
                            uint32_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return;
    *(volatile uint32_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static bool rtl8126_wait_flag(const e1000_state_t *state, uint32_t offset,
                              uint32_t flag, bool set) {
    uint64_t deadline = 0U;
    uint64_t ticks;

    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return false;
    if (x86_boot_cpu_features.tsc_hz != 0U) {
        ticks = x86_timeout_ns_to_tsc(1000000ULL);
        deadline = x86_read_tsc() + ticks;
    }
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        bool value = (rtl8126_read32(state, offset) & flag) != 0U;
        if (value == set) return true;
        if (deadline != 0U && (int64_t)(x86_read_tsc() - deadline) >= 0) {
            return false;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool rtl8126_wait_mcu_flags(const e1000_state_t *state, uint8_t mask,
                                   bool set) {
    uint64_t deadline = 0U;

    if (!rtl8126_reg_valid(state, RTL8126_REG_MCU, sizeof(uint8_t))) {
        return false;
    }
    if (x86_boot_cpu_features.tsc_hz != 0U) {
        deadline = x86_read_tsc() + x86_timeout_ns_to_tsc(10000000ULL);
    }
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        bool value = (rtl8126_read8(state, RTL8126_REG_MCU) & mask) == mask;
        if (value == set) return true;
        if (deadline != 0U &&
            (int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool rtl8126_csi_read(const e1000_state_t *state, uint16_t address,
                             uint32_t *value) {
    uint32_t command;

    if (state == 0 || state->pci == 0 || value == 0) return false;
    command = (uint32_t)(address & RTL8126_CSI_ADDR_MASK) |
              ((uint32_t)state->pci->function << 16) |
              RTL8126_CSI_BYTE_ENABLE;
    rtl8126_write32(state, RTL8126_REG_CSIAR, command);
    if (!rtl8126_wait_flag(state, RTL8126_REG_CSIAR, RTL8126_CSI_FLAG, true)) {
        return false;
    }
    *value = rtl8126_read32(state, RTL8126_REG_CSIDR);
    return *value != UINT32_MAX;
}

static bool rtl8126_csi_write(const e1000_state_t *state, uint16_t address,
                              uint32_t value) {
    uint32_t command;

    if (state == 0 || state->pci == 0) return false;
    rtl8126_write32(state, RTL8126_REG_CSIDR, value);
    command = RTL8126_CSI_WRITE |
              (uint32_t)(address & RTL8126_CSI_ADDR_MASK) |
              ((uint32_t)state->pci->function << 16) |
              RTL8126_CSI_BYTE_ENABLE;
    rtl8126_write32(state, RTL8126_REG_CSIAR, command);
    return rtl8126_wait_flag(state, RTL8126_REG_CSIAR, RTL8126_CSI_FLAG,
                             false);
}

static bool rtl8126_csi_modify(const e1000_state_t *state, uint16_t address,
                               uint32_t mask, uint32_t set) {
    uint32_t value;

    if (!rtl8126_csi_read(state, address, &value)) return false;
    return rtl8126_csi_write(state, address, (value & ~mask) | set);
}

static bool rtl8126_mac_ocp_read(const e1000_state_t *state, uint32_t address,
                                 uint16_t *value) {
    uint32_t result;

    if (state == 0 || value == 0 || (address & 0xFFFF0001U) != 0U) {
        return false;
    }
    rtl8126_write32(state, RTL8126_REG_OCPDR, address << 15);
    result = rtl8126_read32(state, RTL8126_REG_OCPDR);
    if (result == UINT32_MAX) return false;
    *value = (uint16_t)result;
    return true;
}

static bool rtl8126_mac_ocp_write(const e1000_state_t *state, uint32_t address,
                                  uint16_t value) {
    if (state == 0 || (address & 0xFFFF0001U) != 0U) return false;
    rtl8126_write32(state, RTL8126_REG_OCPDR,
                    RTL8126_OCP_FLAG | (address << 15) | value);
    return true;
}

static bool rtl8126_mac_ocp_modify(const e1000_state_t *state,
                                   uint32_t address, uint16_t mask,
                                   uint16_t set) {
    uint16_t value;

    if (!rtl8126_mac_ocp_read(state, address, &value)) return false;
    return rtl8126_mac_ocp_write(state, address,
                                  (uint16_t)((value & (uint16_t)~mask) | set));
}

static bool rtl8126_gphy_ocp_read(const e1000_state_t *state, uint32_t address,
                                  uint16_t *value) {
    uint32_t result;

    if (state == 0 || value == 0 || (address & 0xFFFF0001U) != 0U) {
        return false;
    }
    rtl8126_write32(state, RTL8126_REG_GPHY_OCP, address << 15);
    if (!rtl8126_wait_flag(state, RTL8126_REG_GPHY_OCP, RTL8126_OCP_FLAG,
                           true)) return false;
    result = rtl8126_read32(state, RTL8126_REG_GPHY_OCP);
    if (result == UINT32_MAX) return false;
    *value = (uint16_t)result;
    return true;
}

static bool rtl8126_gphy_ocp_write(const e1000_state_t *state, uint32_t address,
                                   uint16_t value) {
    if (state == 0 || (address & 0xFFFF0001U) != 0U) return false;
    rtl8126_write32(state, RTL8126_REG_GPHY_OCP,
                    RTL8126_OCP_FLAG | (address << 15) | value);
    return rtl8126_wait_flag(state, RTL8126_REG_GPHY_OCP, RTL8126_OCP_FLAG,
                             false);
}

static uint32_t rtl8126_phy_register(const e1000_state_t *state,
                                     uint32_t reg) {
    uint32_t base;

    if (state == 0 || reg >= 0x20U) return UINT32_MAX;
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    if (base != RTL8126_PHY_OCP_BASE && reg >= 0x10U) reg -= 0x10U;
    return base + reg * 2U;
}

static bool rtl8126_phy_read(e1000_state_t *state, uint32_t reg,
                             uint16_t *value) {
    uint32_t address;

    if (state == 0 || value == 0) return false;
    if (reg == 0x1FU) {
        *value = state->rtl_phy_ocp_base == RTL8126_PHY_OCP_BASE ? 0U :
                 (uint16_t)(state->rtl_phy_ocp_base >> 4);
        return true;
    }
    address = rtl8126_phy_register(state, reg);
    return address != UINT32_MAX &&
           rtl8126_gphy_ocp_read(state, address, value);
}

static bool rtl8126_phy_write(e1000_state_t *state, uint32_t reg,
                              uint16_t value) {
    uint32_t address;

    if (state == 0) return false;
    if (reg == 0x1FU) {
        state->rtl_phy_ocp_base = value == 0U ? RTL8126_PHY_OCP_BASE :
                                   (uint32_t)value << 4;
        return true;
    }
    address = rtl8126_phy_register(state, reg);
    return address != UINT32_MAX && rtl8126_gphy_ocp_write(state, address,
                                                            value);
}

static bool rtl8126_mcu_read(e1000_state_t *state, uint32_t reg,
                             uint16_t *value) {
    uint32_t base;

    if (state == 0 || value == 0) return false;
    if (reg == 0x1FU) {
        *value = state->rtl_phy_ocp_base == RTL8126_PHY_OCP_BASE ? 0U :
                 (uint16_t)(state->rtl_phy_ocp_base >> 4);
        return true;
    }
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    return rtl8126_mac_ocp_read(state, base + reg, value);
}

static bool rtl8126_mcu_write(e1000_state_t *state, uint32_t reg,
                              uint16_t value) {
    uint32_t base;

    if (state == 0) return false;
    if (reg == 0x1FU) {
        state->rtl_phy_ocp_base = value == 0U ? RTL8126_PHY_OCP_BASE :
                                   (uint32_t)value << 4;
        return true;
    }
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    return rtl8126_mac_ocp_write(state, base + reg, value);
}

static bool rtl8126_firmware_format(const uint8_t *data, size_t size,
                                    const uint32_t **actions,
                                    uint32_t *action_count) {
    const rtl8126_firmware_header_t *header;
    uint32_t offset;
    uint32_t count;

    if (data == 0 || actions == 0 || action_count == 0 ||
        size < sizeof(uint32_t)) return false;
    if (*(const uint32_t *)data != 0U) {
        if (size % sizeof(uint32_t) != 0U) return false;
        *actions = (const uint32_t *)data;
        *action_count = (uint32_t)(size / sizeof(uint32_t));
        return true;
    }
    if (size < sizeof(*header)) return false;
    header = (const rtl8126_firmware_header_t *)data;
    offset = header->action_offset;
    count = header->action_count;
    if (offset > size || count > (size - offset) / sizeof(uint32_t)) {
        return false;
    }
    uint8_t checksum = 0U;
    for (size_t index = 0U; index < size; ++index) checksum += data[index];
    if (checksum != 0U) return false;
    *actions = (const uint32_t *)(data + offset);
    *action_count = count;
    return true;
}

static bool rtl8126_firmware_actions_valid(const uint32_t *actions,
                                           uint32_t count) {
    if (actions == 0 || count == 0U) return false;
    for (uint32_t index = 0U; index < count; ++index) {
        uint32_t action = actions[index];
        uint32_t opcode = action >> 28;
        uint32_t value = action & 0xFFFFU;
        uint32_t reg = (action >> 16) & 0x0FFFU;
        switch (opcode) {
            case RTL8126_FW_PHY_READ:
            case RTL8126_FW_DATA_OR:
            case RTL8126_FW_DATA_AND:
            case RTL8126_FW_CLEAR_READ_COUNT:
            case RTL8126_FW_PHY_WRITE:
            case RTL8126_FW_WRITE_PREVIOUS:
            case RTL8126_FW_DELAY_MS:
                break;
            case RTL8126_FW_MDIO_CHANGE:
                if (value > 1U) return false;
                break;
            case RTL8126_FW_BACKWARD_JUMP:
                if (reg == 0U || reg >= index) return false;
                break;
            case RTL8126_FW_READ_COUNT_SKIP:
                if (index + 1U >= count) return false;
                break;
            case RTL8126_FW_COMPARE_SKIP:
            case RTL8126_FW_COMPARE_NOT_SKIP:
            case RTL8126_FW_SKIP:
                if (reg >= count - index - 1U) return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

static void rtl8126_delay_ms(uint16_t milliseconds) {
    uint64_t frequency = x86_boot_cpu_features.tsc_hz;
    uint64_t ticks;
    uint64_t deadline;

    if (milliseconds == 0U || frequency == 0U) return;
    if (milliseconds > 1000U) milliseconds = 1000U;
    ticks = (frequency / 1000U) * milliseconds +
            ((frequency % 1000U) * milliseconds + 999U) / 1000U;
    deadline = x86_read_tsc() + ticks;
    while ((int64_t)(x86_read_tsc() - deadline) < 0) {
        __asm__ volatile ("pause");
    }
}

static bool rtl8126_firmware_run(e1000_state_t *state,
                                  const uint32_t *actions,
                                  uint32_t action_count) {
    uint16_t previous = 0U;
    uint32_t read_count = 0U;
    uint64_t steps = 0U;
    bool mcu = false;

    if (!rtl8126_firmware_actions_valid(actions, action_count)) return false;
    for (uint32_t index = 0U; index < action_count; ++index) {
        uint32_t action = actions[index];
        uint32_t opcode = action >> 28;
        uint16_t value = (uint16_t)(action & 0xFFFFU);
        uint32_t reg = (action >> 16) & 0x0FFFU;
        uint16_t current;

        if (++steps > 2000000ULL) return false;
        switch (opcode) {
            case RTL8126_FW_PHY_READ:
                if (!(mcu ? rtl8126_mcu_read(state, reg, &current) :
                              rtl8126_phy_read(state, reg, &current))) {
                    return false;
                }
                previous = current;
                ++read_count;
                break;
            case RTL8126_FW_DATA_OR:
                previous = (uint16_t)(previous | value);
                break;
            case RTL8126_FW_DATA_AND:
                previous = (uint16_t)(previous & value);
                break;
            case RTL8126_FW_BACKWARD_JUMP:
                index -= reg + 1U;
                break;
            case RTL8126_FW_MDIO_CHANGE:
                mcu = value != 0U;
                break;
            case RTL8126_FW_CLEAR_READ_COUNT:
                read_count = 0U;
                break;
            case RTL8126_FW_PHY_WRITE:
                if (!(mcu ? rtl8126_mcu_write(state, reg, value) :
                              rtl8126_phy_write(state, reg, value))) {
                    return false;
                }
                break;
            case RTL8126_FW_READ_COUNT_SKIP:
                if (read_count == value) ++index;
                break;
            case RTL8126_FW_COMPARE_SKIP:
                if (previous == value) index += reg;
                break;
            case RTL8126_FW_COMPARE_NOT_SKIP:
                if (previous != value) index += reg;
                break;
            case RTL8126_FW_WRITE_PREVIOUS:
                if (!(mcu ? rtl8126_mcu_write(state, reg, previous) :
                              rtl8126_phy_write(state, reg, previous))) {
                    return false;
                }
                break;
            case RTL8126_FW_SKIP:
                index += reg;
                break;
            case RTL8126_FW_DELAY_MS:
                rtl8126_delay_ms(value);
                break;
            default:
                return false;
        }
    }
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return true;
}

static bool rtl8126_load_firmware(e1000_state_t *state) {
    void *data = 0;
    size_t size = 0U;
    const uint32_t *actions;
    uint32_t action_count;

    if (state == 0) return false;
    const char *path = state->rtl_xid == RTL8126_XID_3 ?
        "/EFI/OS/firmware/rtl8126/rtl8126a-3.fw" :
        "/EFI/OS/firmware/rtl8126/rtl8126a-2.fw";
    (void)vfs_load_file_kernel(path, 1U, RTL8126_FIRMWARE_MAX_BYTES, &data, &size);
    if (data == 0 || !rtl8126_firmware_format(data, size, &actions,
                                               &action_count) ||
        !rtl8126_firmware_run(state, actions, action_count)) {
        if (data != 0) kfree(data);
        return false;
    }
    kfree(data);
    return true;
}

static bool rtl8126_phy_wait_reset(e1000_state_t *state) {
    /* 固件完成不代表 PHY 复位也已完成。修改 PHY 设置前，遵循 r8169 固件后的 BMCR 握手流程。
     * 等待必须有界；复位失败或 PHY 不可读时不得启动 DMA。 */
        if (state == 0) return false;
    for (uint32_t attempt = 0U; attempt <= 12U; ++attempt) {
        uint16_t bmcr;
        if (!rtl8126_gphy_ocp_read(state, RTL8126_PHY_OCP_BASE, &bmcr) ||
            bmcr == UINT16_MAX) return false;
        if ((bmcr & (1U << 15)) == 0U) return true;
        if (attempt != 12U) rtl8126_delay_ms(50U);
    }
    return false;
}

static bool rtl8126_phy_page(e1000_state_t *state, uint16_t page,
                             uint16_t reg, uint16_t mask, uint16_t set) {
    uint32_t base;
    uint16_t value;
    bool success;

    if (state == 0) return false;
    base = page == 0U ? RTL8126_PHY_OCP_BASE : (uint32_t)page << 4;
    if (!rtl8126_gphy_ocp_read(state, base +
                               (uint32_t)(reg >= 0x10U ? reg - 0x10U : reg) * 2U,
                               &value)) return false;
    success = rtl8126_gphy_ocp_write(
        state, base + (uint32_t)(reg >= 0x10U ? reg - 0x10U : reg) * 2U,
        (uint16_t)((value & (uint16_t)~mask) | set));
    return success;
}

static bool rtl8126_phy_start_autoneg(e1000_state_t *state) {
    /* Clause-22 BMCR 映射到厂商 OCP 地址 A400。固件或先前的操作系统可能保留 power-down/isolate 状态；
     * 单独复位 MAC 不会重新启动铜缆协商。保留速率通告设置。 */
        const uint16_t stopped = (1U << 11) | (1U << 10) | (1U << 14);
    const uint16_t negotiate = (1U << 12) | (1U << 9);
    return rtl8126_phy_page(state, 0U, 0U, stopped, negotiate);
}

static bool rtl8126_phy_config(e1000_state_t *state) {
    bool success = true;
    bool firmware_loaded;

    firmware_loaded = rtl8126_load_firmware(state);
    state->rtl_firmware_required = !firmware_loaded;
    if (firmware_loaded && !rtl8126_phy_wait_reset(state)) return false;
    /* RTL8126A MAC 版本 70 的 PHY 设置沿用上游 r8169 路径。 */
        success = rtl8126_phy_page(state, 0x0A44U, 0x11U, 0U,
                               (uint16_t)(1U << 11)) && success;
    success = rtl8126_phy_page(state, 0x0A43U, 0x10U, (uint16_t)(1U << 2),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A5BU, 0x12U, (uint16_t)(1U << 15),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A6DU, 0x14U, (uint16_t)(1U << 4),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A42U, 0x14U, (uint16_t)(1U << 7),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A4AU, 0x11U, (uint16_t)(1U << 9),
                               0U) && success;
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return success && rtl8126_phy_start_autoneg(state);
}

static bool rtl8126_disable_aspm(e1000_state_t *state) {
    uint8_t interrupt_config = rtl8126_read8(state, RTL8126_REG_INT_CFG);
    uint8_t config5 = rtl8126_read8(state, RTL8126_REG_CONFIG5);

    if (interrupt_config == UINT8_MAX || config5 == UINT8_MAX) return false;
    /* RTL8126 使用 INT_CFG0 的 bit 3，而非旧版 Config2 CLKREQ 位。保持芯片侧 ASPM 关闭：此驱动不管理
     * PCIe LTR 和 L1 子状态。PCIe 链路报告 ASPM 已关闭，仍不足以清除控制器继承的内部省电状态。 */
    if (!rtl8126_mac_ocp_modify(state, 0xE092U, 0x00FFU, 0U)) return false;
    rtl8126_write8(state, RTL8126_REG_INT_CFG,
                   interrupt_config & (uint8_t)~(1U << 3));
    rtl8126_write8(state, RTL8126_REG_CONFIG5,
                   config5 & (uint8_t)~1U);
    return true;
}

static bool rtl8126_hardware_start(e1000_state_t *state) {
    bool success = true;

    if (state == 0 || (state->rtl_xid != RTL8126_XID_2 &&
                       state->rtl_xid != RTL8126_XID_3)) return false;
    if (!rtl8126_disable_aspm(state)) return false;
    /* RTL8126A 硬件启动：禁用 PCIe L2/L3 就绪状态和 ZRXDC。 */
    rtl8126_write8(state, RTL8126_REG_CONFIG3,
                   rtl8126_read8(state, RTL8126_REG_CONFIG3) &
                   (uint8_t)~RTL8126_CONFIG3_L23);
    success = rtl8126_csi_modify(state, 0x0890U, 0x00000001U, 0U) && success;
    success = rtl8126_csi_modify(state, 0x070CU, 0xFF000000U,
                                 0x27000000U) && success;

    rtl8126_write16(state, 0x0382U, 0x221BU);
    rtl8126_write32(state, RTL8126_REG_RSS_CTRL, 0U);
    rtl8126_write16(state, RTL8126_REG_QUEUE_CTRL, 0U);
    success = rtl8126_mac_ocp_modify(state, 0xD40AU, 0x0010U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA84U, 0U, 0x0003U) && success;
    rtl8126_write8(state, RTL8126_REG_CONFIG1,
                   rtl8126_read8(state, RTL8126_REG_CONFIG1) & 0xEFU);
    success = rtl8126_mac_ocp_write(state, 0xC140U, 0xFFFFU) && success;
    success = rtl8126_mac_ocp_write(state, 0xC142U, 0xFFFFU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD3E2U, 0x0FFFU, 0x03A9U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD3E4U, 0x00FFU, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE860U, 0U, 0x0080U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB58U, 0x0001U, 0U) && success;
    rtl8126_write8(state, 0x00D8U,
                   rtl8126_read8(state, 0x00D8U) & 0xFDU);
    success = rtl8126_mac_ocp_modify(state, 0xE614U, 0x0700U, 0x0400U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE63EU, 0x0C30U, 0x0020U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xC0B4U, 0U, 0x000CU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB6AU, 0x00FFU, 0x0033U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB50U, 0x03E0U, 0x0040U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE056U, 0x00F0U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE040U, 0x1000U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0003U, 0x0001U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0300U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE0C0U, 0x4F0FU, 0x4403U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE052U, 0x0080U, 0x0068U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD430U, 0x0FFFU, 0x047FU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0004U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB54U, 0U, 0x0001U) && success;
    /* MCU 复位脉冲不得被合并为连续的 PCI 写入。 */
    rtl8126_delay_ms(1U);
    success = rtl8126_mac_ocp_modify(state, 0xEB54U, 0x0001U, 0U) && success;
    rtl8126_write16(state, 0x1880U,
                    rtl8126_read16(state, 0x1880U) & (uint16_t)~0x0030U);
    success = rtl8126_mac_ocp_write(state, 0xE098U, 0xC302U) && success;
    bool mcu_ready = false;
    for (uint32_t attempt = 0U; attempt < 10U; ++attempt) {
        uint16_t value;
        if (!rtl8126_mac_ocp_read(state, 0xE00EU, &value)) return false;
        if ((value & (1U << 13)) == 0U) { mcu_ready = true; break; }
        rtl8126_delay_ms(1U);
    }
    if (!mcu_ready) return false;
    success = rtl8126_mac_ocp_modify(state, 0xC0ACU, 0U, 0x1F80U) && success;
    rtl8126_write32(state, RTL8126_REG_MISC,
                    rtl8126_read32(state, RTL8126_REG_MISC) & ~(1U << 19));
    return success;
}

static bool rtl8126_map_mmio(e1000_state_t *state) {
    uint32_t bar_index = PCI_MAX_BARS;
    uint64_t mapped = 0U;
    uint64_t span;

    if (state == 0 || state->pci == 0) return false;
    for (uint32_t index = 0U; index < PCI_MAX_BARS; ++index) {
        const pci_bar_t *bar = &state->pci->bars[index];
        if ((bar->flags & PCI_RESOURCE_MEMORY) != 0U && bar->length != 0U) {
            bar_index = index;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) return false;

    span = state->pci->bars[bar_index].length;
    if (span > RTL8126_MMIO_LIMIT) span = RTL8126_MMIO_LIMIT;
    span = (span + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (span < PAGE_SIZE || span > X86_64_MMIO_END - RTL8126_MMIO_VA + 1ULL) {
        return false;
    }
    while (mapped < span) {
        if (x86_map_page(
                x86_current_root_table(), (vaddr_t)(RTL8126_MMIO_VA + mapped),
                paddr_make(state->pci->bars[bar_index].address + mapped),
                X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
            while (mapped != 0U) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(),
                                     (vaddr_t)(RTL8126_MMIO_VA + mapped), 0);
            }
            return false;
        }
        mapped += PAGE_SIZE;
    }
    state->rtl_mmio_bar = (uint8_t)bar_index;
    state->rtl_mmio = (volatile uint8_t *)(uintptr_t)RTL8126_MMIO_VA;
    state->rtl_mmio_span = span;
    return true;
}

static void rtl8126_unmap_mmio(e1000_state_t *state) {
    if (state == 0 || state->rtl_mmio == 0) return;
    for (uint64_t offset = 0U; offset < state->rtl_mmio_span;
         offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(),
                             (vaddr_t)(RTL8126_MMIO_VA + offset), 0);
    }
    state->rtl_mmio = 0;
    state->rtl_mmio_span = 0U;
}

static bool rtl8126_identify(e1000_state_t *state) {
    uint32_t tx_config;
    uint16_t xid;

    if (state == 0) return false;
    tx_config = rtl8126_read32(state, RTL8126_REG_TX_CONFIG);
    if (tx_config == UINT32_MAX) return false;
    xid = (uint16_t)((tx_config >> 20) & RTL8126_XID_MASK);
    if (xid != RTL8126_XID_2 && xid != RTL8126_XID_3) return false;
    state->rtl_xid = xid;
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return true;
}

/*
 * RTL8126A 必须先释放共享 MCU FIFO，再复位 MAC。真实硬件上若缺少此厂商驱动初始化序列，链路可能已经
 * 建立，但 RX DMA 引擎仍处于停止状态。
 */
static bool rtl8126_wait_fifo_empty(const e1000_state_t *state) {
    uint64_t deadline = 0U;
    if (!rtl8126_wait_mcu_flags(state, RTL8126_MCU_RXTX_EMPTY, true)) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x21U);
        return false;
    }
    if (x86_boot_cpu_features.tsc_hz != 0U)
        deadline = x86_read_tsc() + x86_timeout_ns_to_tsc(10000000ULL);
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        uint16_t status = rtl8126_read16(state, RTL8126_REG_FIFO_STATUS);
        if (status == UINT16_MAX) {
            e1000_record_error(RTL8126_ERROR_BASE | 0x22U);
            return false;
        }
        if ((status & 0x0103U) == 0x0103U) return true;
        if (deadline != 0U &&
            (int64_t)(x86_read_tsc() - deadline) >= 0) break;
        __asm__ volatile ("pause");
    }
    e1000_record_error(RTL8126_ERROR_BASE | 0x23U);
    return false;
}

static bool rtl8126_hardware_initialize(e1000_state_t *state) {
    uint8_t command;
    uint32_t misc;

    if (state == 0) return false;
    misc = rtl8126_read32(state, RTL8126_REG_MISC);
    if (misc == UINT32_MAX) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x24U);
        return false;
    }
    rtl8126_write32(state, RTL8126_REG_MISC,
                    misc | RTL8126_MISC_RXDV_GATED_EN);
    rtl8126_delay_ms(2U);
    command = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    if (command == UINT8_MAX) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x25U);
        return false;
    }
    /* 只有在发出 StopReq 后，TXFIFO_EMPTY 状态才有意义。查询任一空状态前，先等待设备要求的 200 微秒静默期。 */
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                   command | RTL8126_CHIP_STOP_REQ);
    rtl8126_delay_ms(1U);
    if (!rtl8126_wait_fifo_empty(state)) return false;
    command = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    if (command == UINT8_MAX) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x25U);
        return false;
    }
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                   command & (uint8_t)~(RTL8126_CHIP_RX_EN |
                                        RTL8126_CHIP_TX_EN));
    rtl8126_delay_ms(1U);
    rtl8126_write8(state, RTL8126_REG_MCU,
                   rtl8126_read8(state, RTL8126_REG_MCU) &
                   (uint8_t)~RTL8126_MCU_NOW_IS_OOB);
    /* 保留第一个失败步骤，不在握手临界区打印日志。 */
    if (!rtl8126_mac_ocp_modify(state, 0xE8DEU, (uint16_t)(1U << 14), 0U)) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x26U); return false;
    }
    if (!rtl8126_wait_mcu_flags(state, RTL8126_MCU_LINK_LIST_READY, true)) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x27U); return false;
    }
    if (!rtl8126_mac_ocp_write(state, 0xC0AAU, 0x07D0U) ||
        !rtl8126_mac_ocp_write(state, 0xC0A6U, 0x0150U) ||
        !rtl8126_mac_ocp_write(state, 0xC01EU, 0x5555U)) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x28U); return false;
    }
    if (!rtl8126_wait_mcu_flags(state, RTL8126_MCU_LINK_LIST_READY, true)) {
        e1000_record_error(RTL8126_ERROR_BASE | 0x29U); return false;
    }
    return true;
}

static uint8_t rtl8126_order_for_pages(uint32_t page_count) {
    uint8_t order = 0U;
    uint32_t allocated = 1U;
    while (allocated < page_count && order < BUDDY_MAX_ORDER) {
        ++order;
        allocated <<= 1U;
    }
    return allocated >= page_count ? order : (uint8_t)(BUDDY_MAX_ORDER + 1U);
}

static bool rtl8126_alloc_region(const pci_device_t *pci, uint32_t page_count,
                                 enum dma_direction direction, page_t **head,
                                 dma_mapping_t *mapping, void **cpu) {
    page_t *pages[RTL8126_RX_PAGE_COUNT];
    uint8_t order;
    paddr_t physical;

    if (pci == 0 || page_count == 0U || page_count > RTL8126_RX_PAGE_COUNT ||
        head == 0 || mapping == 0 || cpu == 0) return false;
    order = rtl8126_order_for_pages(page_count);
    if (order > BUDDY_MAX_ORDER) return false;
    *head = page_alloc(order, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (*head == 0) return false;
    physical = page_to_phys(*head);
    for (uint32_t index = 0U; index < page_count; ++index) {
        pages[index] = phys_to_page(
            paddr_make(physical.value + (uint64_t)index * PAGE_SIZE));
        if (pages[index] == 0) {
            page_free(*head);
            *head = 0;
            return false;
        }
    }
    if (dma_map_pages((device_t *)&pci->device, pages, page_count, direction,
                      mapping) != K_OK || mapping->segment_count != 1U ||
        mapping->segments[0].length < (uint64_t)page_count * PAGE_SIZE) {
        if (dma_mapping_active(mapping) && dma_unmap_checked(mapping) != K_OK) {
            return false;
        }
        page_free(*head);
        *head = 0;
        return false;
    }
    *cpu = phys_to_direct(physical);
    if (*cpu == 0) {
        if (dma_unmap_checked(mapping) != K_OK) return false;
        page_free(*head);
        *head = 0;
        return false;
    }
    return true;
}

static bool rtl8126_free_region(page_t **head, dma_mapping_t *mapping) {
    if (head == 0 || mapping == 0) return false;
    if (dma_mapping_active(mapping) && dma_unmap_checked(mapping) != K_OK) {
        return false;
    }
    if (*head != 0) page_free(*head);
    *head = 0;
    *mapping = (dma_mapping_t){0};
    return true;
}

static uint64_t rtl8126_dma_address(const dma_mapping_t *mapping) {
    return mapping != 0 && mapping->segment_count == 1U ?
           mapping->segments[0].addr.value : 0U;
}

static bool rtl8126_free_dma(e1000_state_t *state);

static bool rtl8126_alloc_dma(e1000_state_t *state) {
    uint64_t tx_address;
    uint64_t rx_address;
    if (state == 0 || state->pci == 0) return false;
    if (!rtl8126_alloc_region(state->pci, 1U, DMA_BIDIRECTIONAL,
                              &state->rtl_tx_ring_page,
                              &state->rtl_tx_ring_dma,
                              (void **)&state->rtl_tx_ring) ||
        !rtl8126_alloc_region(state->pci, 1U, DMA_BIDIRECTIONAL,
                              &state->rtl_rx_ring_page,
                              &state->rtl_rx_ring_dma,
                              (void **)&state->rtl_rx_ring) ||
        !rtl8126_alloc_region(
            state->pci,
            (RTL8126_RING_COUNT * RTL8126_TX_BUFFER_SIZE) / PAGE_SIZE,
            DMA_TO_DEVICE, &state->rtl_tx_buffer_page,
            &state->rtl_tx_buffer_dma, (void **)&state->rtl_tx_buffers) ||
        !rtl8126_alloc_region(
            state->pci, RTL8126_RX_PAGE_COUNT, DMA_FROM_DEVICE,
            &state->rtl_rx_buffer_page, &state->rtl_rx_buffer_dma,
            (void **)&state->rtl_rx_buffers)) {
        /* 生命周期回滚负责处理部分分配结果，包括释放失败的情况。 */
        return false;
    }

    tx_address = rtl8126_dma_address(&state->rtl_tx_buffer_dma);
    rx_address = rtl8126_dma_address(&state->rtl_rx_buffer_dma);
    if (tx_address == 0U || rx_address == 0U) {
        return false;
    }
    for (uint32_t index = 0U; index < RTL8126_RING_COUNT; ++index) {
        rtl8126_descriptor_t *tx = &state->rtl_tx_ring[index];
        rtl8126_descriptor_t *rx = &state->rtl_rx_ring[index];
        tx->options1 = index + 1U == RTL8126_RING_COUNT ? RTL8126_DESC_EOR : 0U;
        tx->options2 = 0U;
        tx->address = tx_address + (uint64_t)index * RTL8126_TX_BUFFER_SIZE;
        rx->options1 = RTL8126_DESC_OWN | RTL8126_RX_DESCRIPTOR_LENGTH |
                       (index + 1U == RTL8126_RING_COUNT ? RTL8126_DESC_EOR : 0U);
        rx->options2 = 0U;
        rx->address = rx_address + (uint64_t)index * RTL8126_RX_BUFFER_SIZE;
    }
    state->rtl_tx_next = 0U;
    state->rtl_tx_clean = 0U;
    state->rtl_tx_pending = 0U;
    state->rtl_rx_next = 0U;
    dma_sync_for_device(&state->rtl_tx_ring_dma);
    dma_sync_for_device(&state->rtl_rx_ring_dma);
    dma_sync_for_device(&state->rtl_tx_buffer_dma);
    dma_sync_for_device(&state->rtl_rx_buffer_dma);
    dma_wmb();
    return true;
}

static bool rtl8126_free_dma(e1000_state_t *state) {
    bool success = true;
    bool released;
    if (state == 0) return false;
    released = rtl8126_free_region(&state->rtl_rx_buffer_page,
                                   &state->rtl_rx_buffer_dma);
    if (released) state->rtl_rx_buffers = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_tx_buffer_page,
                                   &state->rtl_tx_buffer_dma);
    if (released) state->rtl_tx_buffers = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_rx_ring_page,
                                   &state->rtl_rx_ring_dma);
    if (released) state->rtl_rx_ring = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_tx_ring_page,
                                   &state->rtl_tx_ring_dma);
    if (released) state->rtl_tx_ring = 0;
    else success = false;
    return success;
}

static bool rtl8126_mac_valid(const uint8_t mac[6]) {
    bool all_zero = true;
    bool all_ff = true;
    for (uint32_t index = 0U; index < 6U; ++index) {
        if (mac[index] != 0U) all_zero = false;
        if (mac[index] != 0xFFU) all_ff = false;
    }
    return !all_zero && !all_ff && (mac[0] & 1U) == 0U;
}

static void rtl8126_read_mac(e1000_state_t *state) {
    uint32_t low = rtl8126_read32(state, RTL8126_REG_MAC0_BACKUP);
    uint32_t high = rtl8126_read32(state, RTL8126_REG_MAC0_BACKUP + 4U);
    state->mac[0] = (uint8_t)low;
    state->mac[1] = (uint8_t)(low >> 8);
    state->mac[2] = (uint8_t)(low >> 16);
    state->mac[3] = (uint8_t)(low >> 24);
    state->mac[4] = (uint8_t)high;
    state->mac[5] = (uint8_t)(high >> 8);
    if (rtl8126_mac_valid(state->mac)) return;
    low = rtl8126_read32(state, RTL8126_REG_MAC0);
    high = rtl8126_read32(state, RTL8126_REG_MAC0 + 4U);
    state->mac[0] = (uint8_t)low;
    state->mac[1] = (uint8_t)(low >> 8);
    state->mac[2] = (uint8_t)(low >> 16);
    state->mac[3] = (uint8_t)(low >> 24);
    state->mac[4] = (uint8_t)high;
    state->mac[5] = (uint8_t)(high >> 8);
}

static void rtl8126_program_mac(const e1000_state_t *state) {
    uint32_t low;
    uint32_t high;

    if (state == 0) return;
    low = (uint32_t)state->mac[0] |
          ((uint32_t)state->mac[1] << 8U) |
          ((uint32_t)state->mac[2] << 16U) |
          ((uint32_t)state->mac[3] << 24U);
    high = (uint32_t)state->mac[4] | ((uint32_t)state->mac[5] << 8U);
    rtl8126_write32(state, RTL8126_REG_MAC0 + 4U, high);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    rtl8126_write32(state, RTL8126_REG_MAC0, low);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
}

/* 仅由常规管理器上下文调用，ISR/RX 工作线程绝不调用。每次启动最多写入八条小记录，即使没有连接串口线，
 * 也能将真实机器诊断信息保存在启动卷。数值均以十六进制表示。 */
static void rtl8126_persist_diagnostic(uint32_t report, const uint32_t values[27]) {
    static const char *const labels[27] = {
        " IRQ=", " IRQSTATUS=", " RXDESC=", " RXFRAME=", " TX=",
        " RXNEXT=", " TXLAST=", " INT=", " INTV2=", " CMD=", " TXCFG=",
        " RXCFG=", " PCICMD=", " MISC=", " TXBASELO=", " TXBASEHI=",
        " RXBASELO=", " RXBASEHI=", " TXOWN=", " TXFIRST=",
        " CMDSTART=", " CMDPREIRQ=", " CMDPOSTIRQ=", " CMDPRE=", " CMDPOST=",
        " ENABLES=", " ENABLEFAIL="
    };
    static const char hex[] = "0123456789abcdef";
    char line[512];
    size_t used = 0U;
    const char *prefix = "OS_RTL8126_DIAG";
    file_t *file = 0;
    while (*prefix != '\0') line[used++] = *prefix++;
    for (unsigned i = 0U; i < 27U; ++i) {
        const char *label = labels[i];
        while (*label != '\0') line[used++] = *label++;
        for (unsigned digit = 0U; digit < 8U; ++digit)
            line[used++] = hex[(values[i] >> (28U - 4U * digit)) & 15U];
    }
    line[used++] = '\n';
    if (vfs_open_kernel("/log/rtl8126.log", VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                        (report == 0U ? VFS_OPEN_TRUNCATE : VFS_OPEN_APPEND),
                        0644U, &file) != K_OK || file == 0) return;
    for (size_t offset = 0U; offset < used;) {
        uint64_t written = 0U;
        if (vfs_write_kernel(file, line + offset, used - offset, &written) != K_OK ||
            written == 0U || written > used - offset) break;
        offset += (size_t)written;
    }
    (void)vfs_fsync(file);
    vfs_close(file);
}

void rtl8126_emit_diagnostic(void) {
    e1000_state_t *state = e1000_controller_state();
    uint32_t report;
    uint64_t now;
    uint64_t interval;
    uint32_t rx_descriptor = 0U;
    uint32_t rx_frame = 0U;
    uint32_t irq = 0U;
    uint32_t irq_status = 0U;
    uint32_t tx = 0U;
    uint32_t rx0 = 0U;
    uint32_t irq_status_v2 = 0U;
    uint64_t msix_address = 0U;
    uint32_t msix_data = 0U;
    uint32_t msix_control = 0U;
    uint32_t pci_command = 0U;
    uint32_t msi_capability = 0U;
    uint32_t msix_capability = 0U;

    if (state == 0 || state->backend != NET_BACKEND_RTL8126 ||
        !state->initialized || !state->link_up) return;
    report = __atomic_load_n(&state->rtl_diagnostic_reports, __ATOMIC_RELAXED);
    if (report >= 8U) return;
    now = x86_read_tsc();
    interval = x86_timeout_ns_to_tsc(2000000000ULL);
    if (report != 0U && interval != 0U &&
        (int64_t)(now - state->rtl_diagnostic_tsc) < (int64_t)interval) {
        return;
    }
    if (!__atomic_compare_exchange_n(&state->rtl_diagnostic_reports, &report,
                                     report + 1U, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return;
    }
    state->rtl_diagnostic_tsc = now;
    irq = __atomic_load_n(&state->rtl_irq_count, __ATOMIC_RELAXED);
    irq_status = __atomic_load_n(&state->rtl_irq_status_count,
                                 __ATOMIC_RELAXED);
    rx_descriptor = __atomic_load_n(&state->rtl_rx_descriptor_count,
                                    __ATOMIC_RELAXED);
    rx_frame = __atomic_load_n(&state->rtl_rx_frame_count, __ATOMIC_RELAXED);
    tx = __atomic_load_n(&state->rtl_tx_count, __ATOMIC_RELAXED);
    if (state->rtl_rx_ring != 0) {
        dma_sync_for_cpu(&state->rtl_rx_ring_dma);
        rx0 = state->rtl_rx_ring[0].options1;
    }
    irq_status_v2 = rtl8126_read32(state, RTL8126_REG_INT_STATUS_V2);
    (void)pci_read_config32(state->pci, 4U, &pci_command);
    if (state->pci->msi_capability != 0U) {
        (void)pci_read_config32(state->pci, state->pci->msi_capability,
                                &msi_capability);
    }
    if (state->pci->msix_capability != 0U) {
        (void)pci_read_config32(state->pci, state->pci->msix_capability,
                                &msix_capability);
        (void)pci_msix_read_entry(state->pci, 0U, &msix_address, &msix_data,
                                  &msix_control);
    }
    kernel_serial_printf_serial_only(
        "OS_RTL8126_DIAG IRQ=%u STATUS=%u RXDESC=%u RXFRAME=%u TX=%u "
        "CMD=%x INTCFG=%x RXCFG=%x RX0=%x INT=%x MASK=%x INTV2=%x "
        "PCICMD=%x MSI=%x MSIX=%x MSIXAL=%x MSIXAH=%x MSIXD=%x MSIXC=%x\r\n",
        irq, irq_status, rx_descriptor, rx_frame, tx,
        (uint32_t)rtl8126_read8(state, RTL8126_REG_CHIP_CMD),
        (uint32_t)rtl8126_read8(state, RTL8126_REG_INT_CFG),
        rtl8126_read32(state, RTL8126_REG_RX_CONFIG), rx0,
        rtl8126_read32(state, RTL8126_REG_INT_STATUS),
        rtl8126_read32(state, RTL8126_REG_INT_MASK),
        irq_status_v2,
        pci_command, msi_capability, msix_capability,
        (uint32_t)msix_address, (uint32_t)(msix_address >> 32), msix_data,
        msix_control);
    uint32_t rx_next = 0U;
    uint32_t tx_last = 0U;
    uint32_t tx_owned = 0U;
    uint32_t tx_first = 0U;
    if (state->rtl_rx_ring != 0)
        rx_next = state->rtl_rx_ring[state->rtl_rx_next].options1;
    if (state->rtl_tx_ring != 0) {
        dma_sync_for_cpu(&state->rtl_tx_ring_dma);
        tx_last = state->rtl_tx_ring[((state->rtl_tx_next + RTL8126_RING_COUNT - 1U) %
                                    RTL8126_RING_COUNT)].options1;
        tx_first = state->rtl_tx_ring[0].options1;
        for (uint32_t i = 0U; i < RTL8126_RING_COUNT; ++i)
            if ((state->rtl_tx_ring[i].options1 & RTL8126_DESC_OWN) != 0U) ++tx_owned;
    }
    const uint32_t values[27] = {irq, irq_status, rx_descriptor, rx_frame, tx,
        rx_next, tx_last, rtl8126_read32(state, RTL8126_REG_INT_STATUS), irq_status_v2,
        rtl8126_read8(state, RTL8126_REG_CHIP_CMD),
        rtl8126_read32(state, RTL8126_REG_TX_CONFIG),
        rtl8126_read32(state, RTL8126_REG_RX_CONFIG), pci_command,
        rtl8126_read32(state, RTL8126_REG_MISC),
        rtl8126_read32(state, RTL8126_REG_TX_DESC_LO),
        rtl8126_read32(state, RTL8126_REG_TX_DESC_LO + 4U),
        rtl8126_read32(state, RTL8126_REG_RX_DESC_LO),
        rtl8126_read32(state, RTL8126_REG_RX_DESC_LO + 4U), tx_owned, tx_first,
        state->rtl_cmd_after_start, state->rtl_cmd_before_irq, state->rtl_cmd_after_irq,
        state->rtl_cmd_last_before_enable, state->rtl_cmd_last_after_enable,
        state->rtl_enable_attempts, state->rtl_enable_failures};
    rtl8126_persist_diagnostic(report, values);
}

static void rtl8126_clear_interrupts(e1000_state_t *state) {
    if (state == 0) return;
    rtl8126_write32(state, RTL8126_REG_INT_MASK, 0U);
    rtl8126_write32(state, RTL8126_REG_INT_STATUS, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_INT_MASK_V2_CLEAR, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_INT_STATUS_V2, UINT32_MAX);
}

static bool rtl8126_reset_controller(e1000_state_t *state) {
    if (!rtl8126_reg_valid(state, RTL8126_REG_CHIP_CMD, sizeof(uint8_t))) {
        return false;
    }
    rtl8126_clear_interrupts(state);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD, RTL8126_CHIP_RESET);
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        if ((rtl8126_read8(state, RTL8126_REG_CHIP_CMD) & RTL8126_CHIP_RESET) == 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool rtl8126_set_busmaster(e1000_state_t *state, bool enabled) {
    uint32_t value;
    if (state == 0 || state->pci == 0 ||
        pci_read_config32(state->pci, 4U, &value) != K_OK ||
        value == UINT32_MAX) return false;
    uint16_t command = (uint16_t)value;
    command = (uint16_t)((command | 2U) & (uint16_t)~4U);
    if (enabled) command |= 4U;
    /* PCI Status 是 W1C 寄存器：高半部分写零，不得执行读-改-写。 */
    if (pci_write_config32(state->pci, 4U, command) != K_OK ||
        pci_read_config32(state->pci, 4U, &value) != K_OK ||
        value == UINT32_MAX || (value & 6U) != (command & 6U)) return false;
    ((pci_device_t *)state->pci)->command = (uint16_t)value;
    return true;
}

static bool rtl8126_quiesce_dma(e1000_state_t *state) {
    rtl8126_clear_interrupts(state);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD, 0U);
    /* 复位完成后才能释放设备所有的描述符或页面。即使复位失败也要尝试关闭 PCI 门控；任一操作出错时都
     * 保留资源。 */
    bool reset = rtl8126_reset_controller(state);
    bool gated = rtl8126_set_busmaster(state, false);
    return reset && gated;
}

static void rtl8126_write_dma_base(const e1000_state_t *state,
                                   uint32_t low_offset, uint64_t address) {
    /* Realtek 文档规定先写高双字，作为 64 位更新的第一步。 */
    rtl8126_write32(state, low_offset + 4U, (uint32_t)(address >> 32));
    rtl8126_write32(state, low_offset, (uint32_t)address);
}

/* 调用方负责控制器生命周期。此处绝不复位 DMA 环：硬件可能仍拥有其中的描述符。仅重新启动已配置但处于
 * 停止状态的 MAC。读回失败必须报告错误，不能当作报文提交成功。 */
static bool rtl8126_ensure_mac_enabled(e1000_state_t *state) {
    const uint8_t enabled = RTL8126_CHIP_RX_EN | RTL8126_CHIP_TX_EN;
    if (state == 0) return false;
    uint8_t command = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    if (command == UINT8_MAX || (command & RTL8126_CHIP_RESET) != 0U) return false;
    if ((command & (enabled | RTL8126_CHIP_STOP_REQ)) == enabled) return true;
    state->rtl_cmd_last_before_enable = command;
    ++state->rtl_enable_attempts;
    /* 短暂稳定间隔后重试一次；即使硬件故障，操作也保持有界。 */
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                       (uint8_t)((command & (uint8_t)~RTL8126_CHIP_STOP_REQ) | enabled));
        command = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
        state->rtl_cmd_last_after_enable = command;
        if (command == UINT8_MAX || (command & RTL8126_CHIP_RESET) != 0U) break;
        if ((command & (enabled | RTL8126_CHIP_STOP_REQ)) == enabled) return true;
        if (attempt == 0U) rtl8126_delay_ms(1U);
    }
    ++state->rtl_enable_failures;
    return false;
}

static bool rtl8126_start_controller(e1000_state_t *state) {
    uint64_t tx_ring;
    uint64_t rx_ring;
    uint32_t rx_config;
    uint32_t tx_config;
    if (state == 0) return false;
    tx_ring = rtl8126_dma_address(&state->rtl_tx_ring_dma);
    rx_ring = rtl8126_dma_address(&state->rtl_rx_ring_dma);
    if (tx_ring == 0U || rx_ring == 0U) {
        return false;
    }

    rtl8126_write8(state, RTL8126_REG_CFG9346, 0xC0U);
    if (!rtl8126_hardware_start(state)) {
        rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
        return false;
    }
    rtl8126_program_mac(state);
    rtl8126_write8(state, RTL8126_REG_INT_CFG, 0U);
    rtl8126_write16(state, RTL8126_REG_INT_CFG1, 0U);
    /* 0xE2 在 RTL8126 上是 FIFO 状态寄存器，而非旧版 IntrMitigate 寄存器。仅通过逐向量寄存器关闭合并。 */
    for (uint32_t offset = 0x0A00U; offset < 0x0A80U; offset += 4U) {
        rtl8126_write32(state, offset, 0U);
    }
    rtl8126_clear_interrupts(state);
    rtl8126_write_dma_base(state, RTL8126_REG_TX_DESC_LO, tx_ring);
    rtl8126_write_dma_base(state, RTL8126_REG_RX_DESC_LO, rx_ring);
    rtl8126_write16(state, RTL8126_REG_RX_MAX_SIZE,
                    (uint16_t)RTL8126_RX_BUFFER_SIZE);
    rtl8126_write16(state, RTL8126_REG_CPLUS_CMD, 0U);
    rtl8126_write32(state, RTL8126_REG_MAR0 + 4U, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_MAR0, UINT32_MAX);

    /* 提交描述符并启用 MAC，然后配置 RX/TX。TXCFG_AUTO_FIFO（bit 7）属于旧款 8111E-VL MAC，不适用于
     * RTL8126。r8169 的 rtl_is_8168evl_up() 明确仅适用于 MAC 版本不高于 52 的设备。 */
    rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    /* 环形队列和 DMA 映射已准备完成。启动 MAC 前先打开 PCI DMA 门控：在 busmaster 关闭时启用 MAC，可能
     * 导致 RTL8126 TX 在后续 MAC 复位或驱动交接后仍停滞。 */
    if (!rtl8126_set_busmaster(state, true)) return false;
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                   RTL8126_CHIP_RX_EN | RTL8126_CHIP_TX_EN);
    rx_config = (8U << 27) | (7U << 8) | (1U << 11) | 0x0EU;
    rtl8126_write32(state, RTL8126_REG_RX_CONFIG, rx_config);
    tx_config = (7U << 8) | (3U << 24);
    rtl8126_write32(state, RTL8126_REG_TX_CONFIG, tx_config);
    state->rtl_cmd_after_start = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    return true;
}

static void rtl8126_irq_handler(uint8_t vector, struct arch_trap_frame *frame,
                                void *argument) {
    e1000_state_t *state = (e1000_state_t *)argument;
    uint32_t status;
    (void)vector;
    (void)frame;
    if (state == 0 || !state->initialized || !state->rtl_irq_bound) return;
    __atomic_add_fetch(&state->rtl_irq_count, 1U, __ATOMIC_RELAXED);
    status = rtl8126_read32(state, RTL8126_REG_INT_STATUS);
    if (status == 0U || status == UINT32_MAX) return;
    __atomic_add_fetch(&state->rtl_irq_status_count, 1U, __ATOMIC_RELAXED);
    /* 排空 RX 后必须重新布置 MSI。数据包突发时，若状态更新与描述符回收竞争，不得让 MSI 一直保持启用。 */
    if ((status & RTL8126_INT_MASK_VALUE) != 0U) {
        rtl8126_write32(state, RTL8126_REG_INT_MASK, 0U);
        /* 屏蔽操作是已投递的 PCIe 写入。其他 CPU 执行延迟批次前必须先排空写入；仅使用 CPU 栅栏无法确保
         * 写入到达网卡。 */
        (void)rtl8126_read32(state, RTL8126_REG_INT_MASK);
    }
    rtl8126_write32(state, RTL8126_REG_INT_STATUS, status);
    if ((status & RTL8126_INT_MASK_VALUE) != 0U) {
        (void)e1000_schedule_deferred_poll();
    }
}

bool rtl8126_bind_interrupt_state(e1000_state_t *state) {
    paddr_t table;
    uint16_t entries;
    bool use_msix;
    kstatus_t status;

    if (state == 0 || state->pci == 0 || !state->initialized ||
        state->rtl_irq_bound || RTL8126_IRQ_VECTOR > IRQ_VECTOR_LAST) {
        return false;
    }
    /* RTL8126 V2 将 RX/链路事件映射到不同的 MSI-X 消息 ID。此驱动仅拥有一个 IRQ 槽位，因此使用文档规定的
     * 单向量 MSI 回退，而不是只配置 MSI-X 消息 0。 */
    use_msix = false;
    table = paddr_make(0U);
    entries = 0U;
    (void)pci_msix_table(state->pci, &table, &entries);
    if (state->pci->msi_capability == 0U) return false;
    state->rtl_cmd_before_irq = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    kernel_serial_printf_serial_only(
        "OS_RTL8126_IRQ_SETUP MODE=%u MSI=%u CAP=%u ENTRIES=%u BAR=%u "
        "OFF=%x TABLE=%x APIC=%u VECTOR=%u\r\n",
        use_msix ? 1U : 0U, state->pci->msi_capability != 0U ? 1U : 0U,
        state->pci->msix_capability, entries, state->pci->msix_table_bar,
        state->pci->msix_table_offset, (uint32_t)table.value,
        x86_current_apic_id(), RTL8126_IRQ_VECTOR);
    (void)rtl8126_read32(state, RTL8126_REG_INT_STATUS);
    if (irq_register(RTL8126_IRQ_VECTOR, rtl8126_irq_handler, state) != K_OK) {
        return false;
    }
    status = use_msix ?
        pci_msix_configure((pci_device_t *)state->pci, 0U,
                           x86_current_apic_id(), RTL8126_IRQ_VECTOR) :
        pci_msi_configure((pci_device_t *)state->pci,
                          x86_current_apic_id(), RTL8126_IRQ_VECTOR);
    if (status != K_OK) {
        (void)irq_unregister(RTL8126_IRQ_VECTOR, rtl8126_irq_handler, state);
        return false;
    }
    state->rtl_irq_vector = RTL8126_IRQ_VECTOR;
    state->rtl_irq_msix = use_msix;
    state->rtl_irq_bound = true;
    if (use_msix &&
        pci_msix_mask((pci_device_t *)state->pci, 0U, false) != K_OK) {
        rtl8126_unbind_interrupt_state(state);
        return false;
    }
    rtl8126_write8(state, RTL8126_REG_INT_CFG,
                   rtl8126_read8(state, RTL8126_REG_INT_CFG) &
                   (uint8_t)~RTL8126_INT_CFG_ENABLE);
    rtl8126_clear_interrupts(state);
    rtl8126_write32(state, RTL8126_REG_INT_MASK, RTL8126_INT_MASK_VALUE);
    state->rtl_cmd_after_irq = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    kernel_serial_printf_serial_only(
        "OS_RTL8126_IRQ_ARMED MODE=%u INTCFG=%x INT=%x MASK=%x "
        "INTV2=%x MASKV2=%x\r\n",
        use_msix ? 1U : 0U,
        (uint32_t)rtl8126_read8(state, RTL8126_REG_INT_CFG),
        rtl8126_read32(state, RTL8126_REG_INT_STATUS),
        rtl8126_read32(state, RTL8126_REG_INT_MASK),
        rtl8126_read32(state, RTL8126_REG_INT_STATUS_V2),
        rtl8126_read32(state, RTL8126_REG_INT_MASK_V2_SET));
    return true;
}

void rtl8126_unbind_interrupt_state(e1000_state_t *state) {
    if (state == 0 || !state->rtl_irq_bound) return;
    rtl8126_clear_interrupts(state);
    if (state->rtl_irq_msix) {
        (void)pci_msix_mask((pci_device_t *)state->pci, 0U, true);
        (void)pci_msix_disable((pci_device_t *)state->pci);
    } else {
        (void)pci_msi_disable((pci_device_t *)state->pci);
    }
    (void)irq_unregister(state->rtl_irq_vector, rtl8126_irq_handler, state);
    state->rtl_irq_vector = 0U;
    state->rtl_irq_msix = false;
    state->rtl_irq_bound = false;
}

bool rtl8126_initialize_state(e1000_state_t *state, const pci_device_t *pci) {
    if (state == 0 || pci == 0 || state->initialized || state->rtl_dma_quarantined) {
        e1000_record_error(RTL8126_ERROR_BASE | 1U);
        return false;
    }
    e1000_state_prepare(state, pci, NET_BACKEND_RTL8126);
    if (!rtl8126_set_busmaster(state, false) ||
        !rtl8126_map_mmio(state) || !rtl8126_identify(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 2U);
        rtl8126_unmap_mmio(state);
        return false;
    }
    state->rtl_dma_quarantined = true;
    if (!rtl8126_hardware_initialize(state) || !rtl8126_reset_controller(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 2U);
        (void)rtl8126_destroy_state(state);
        return false;
    }
    rtl8126_read_mac(state);
    if (!rtl8126_mac_valid(state->mac) || !rtl8126_phy_config(state) ||
        !rtl8126_alloc_dma(state) ||
        !rtl8126_start_controller(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 3U);
        (void)rtl8126_destroy_state(state);
        return false;
    }
    e1000_configure_link_local(state);
    net_device_init(&state->device, "rtl8126", state->mac, 1500U,
                    e1000_device_transmit, state);
    state->link_up = (rtl8126_read8(state, RTL8126_REG_PHY_STATUS) &
                      RTL8126_PHY_LINK) != 0U;
    state->device.link_up = state->link_up;
    state->initialized = true;
    state->rtl_dma_quarantined = false;
    e1000_install_socket_outputs(state);
    return true;
}

bool rtl8126_destroy_state(e1000_state_t *state) {
    if (state == 0 || (!state->initialized && !state->rtl_dma_quarantined)) return true;
    e1000_runtime_set_ready(false);
    e1000_remove_socket_outputs();
    rtl8126_unbind_interrupt_state(state);
    state->initialized = false;
    state->rtl_dma_quarantined = true;
    if (!rtl8126_quiesce_dma(state)) return false;
    rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
    if (!rtl8126_free_dma(state)) return false;
    rtl8126_unmap_mmio(state);
    state->rtl_dma_quarantined = false;
    state->backend = NET_BACKEND_NONE;
    return true;
}

/* 调用方持有 tx_lock。仅按环形顺序回收已提交描述符：未使用槽位和乱序清除的 OWN 位都不代表 TX 有进展。 */
static uint16_t rtl8126_reclaim_tx_locked(e1000_state_t *state) {
    uint16_t completed = 0U;
    dma_sync_for_cpu(&state->rtl_tx_ring_dma);
    while (state->rtl_tx_pending != 0U) {
        if ((state->rtl_tx_ring[state->rtl_tx_clean].options1 &
             RTL8126_DESC_OWN) != 0U) break;
        state->rtl_tx_clean = (uint16_t)((state->rtl_tx_clean + 1U) %
                                          RTL8126_RING_COUNT);
        --state->rtl_tx_pending;
        ++completed;
    }
    /* 完成进度更新和队列通知是一个整体操作。生产者也会回收描述符；若将通知延迟到工作线程，生产者先
     * 消费进度时就会丢失通知。 */
    if (completed != 0U && state->rtl_tx_pending != 0U) {
        dma_wmb();
        rtl8126_write16(state, RTL8126_REG_TX_POLL, 1U);
    }
    return completed;
}

kstatus_t rtl8126_transmit_status(e1000_state_t *state, const uint8_t *frame,
                                  size_t length) {
    rtl8126_descriptor_t *descriptor;
    uint32_t options;
    size_t wire_length;
    if (state == 0 || frame == 0 || length == 0U ||
        length > RTL8126_TX_BUFFER_SIZE) {
        return K_EINVAL;
    }
    if (!state->initialized || state->rtl_tx_ring == 0 ||
        state->rtl_tx_buffers == 0) return K_EIO;
    (void)rtl8126_reclaim_tx_locked(state);
    if (state->rtl_tx_pending == RTL8126_RING_COUNT) return K_EAGAIN;
    descriptor = &state->rtl_tx_ring[state->rtl_tx_next];
    if ((descriptor->options1 & RTL8126_DESC_OWN) != 0U) return K_EAGAIN;
    if (!rtl8126_ensure_mac_enabled(state)) return K_EIO;
    memcpy(state->rtl_tx_buffers +
                     (size_t)state->rtl_tx_next * RTL8126_TX_BUFFER_SIZE,
                 frame, length);
    /* RTL8126 需要软件填充（与 r8169 的 ETH_ZLEN 特性相同）。发布实际 DMA 长度，并将填充区清零，
     * 不得读取调用方报文末尾之外的数据，也不得泄露此前发送的负载。 */
    wire_length = length < RTL8126_TX_MIN_FRAME_SIZE ?
                  RTL8126_TX_MIN_FRAME_SIZE : length;
    if (wire_length != length) {
        memset(state->rtl_tx_buffers +
                   (size_t)state->rtl_tx_next * RTL8126_TX_BUFFER_SIZE + length,
               0, wire_length - length);
    }
    descriptor->options2 = 0U;
    options = (uint32_t)wire_length | RTL8126_DESC_OWN | RTL8126_DESC_FS |
              RTL8126_DESC_LS;
    if (state->rtl_tx_next + 1U == RTL8126_RING_COUNT) options |= RTL8126_DESC_EOR;
    dma_sync_for_device(&state->rtl_tx_buffer_dma);
    dma_wmb();
    descriptor->options1 = options;
    dma_sync_for_device(&state->rtl_tx_ring_dma);
    dma_wmb();
    state->rtl_tx_next = (uint16_t)((state->rtl_tx_next + 1U) %
                                    RTL8126_RING_COUNT);
    ++state->rtl_tx_pending;
    rtl8126_write16(state, RTL8126_REG_TX_POLL, 1U);
    __atomic_add_fetch(&state->rtl_tx_count, 1U, __ATOMIC_RELAXED);
    return K_OK;
}

bool rtl8126_link_up_state(e1000_state_t *state) {
    if (state == 0 || !state->initialized) return false;
    state->link_up = (rtl8126_read8(state, RTL8126_REG_PHY_STATUS) &
                      RTL8126_PHY_LINK) != 0U;
    state->device.link_up = state->link_up;
    if (state->link_up) (void)rtl8126_ensure_mac_enabled(state);
    return state->link_up;
}

static void rtl8126_complete_tx(e1000_state_t *state) {
    if (state == 0 || !state->initialized || state->rtl_tx_ring == 0) return;
    sched_preempt_disable();
    spinlock_lock(&state->tx_lock);
    if (state->rtl_tx_pending != 0U)
        (void)rtl8126_reclaim_tx_locked(state);
    spinlock_unlock(&state->tx_lock);
    sched_preempt_enable();
}

void rtl8126_rearm_interrupts(e1000_state_t *state) {
    if (!state->rtl_irq_bound) return;
    uint32_t status = rtl8126_read32(state, RTL8126_REG_INT_STATUS);
    if (status == UINT32_MAX) return;
    /* 仅 ISR 可以确认事件。在此处清除会丢弃工作线程获取快照后到达的 TX 或链路事件。 */
    rtl8126_write32(state, RTL8126_REG_INT_MASK, RTL8126_INT_MASK_VALUE);
    (void)rtl8126_read32(state, RTL8126_REG_INT_MASK);
}

bool rtl8126_poll_receive_state(e1000_state_t *state) {
    bool received = false;
    uint32_t processed = 0U;
    if (state == 0 || !state->initialized || state->rtl_rx_ring == 0) return false;
    if (state->rtl_irq_bound) {
        rtl8126_write32(state, RTL8126_REG_INT_MASK, 0U);
        (void)rtl8126_read32(state, RTL8126_REG_INT_MASK);
    }
    dma_sync_for_cpu(&state->rtl_rx_ring_dma);
    dma_sync_for_cpu(&state->rtl_rx_buffer_dma);
    while (processed < E1000_RX_BUDGET) {
        rtl8126_descriptor_t *descriptor = &state->rtl_rx_ring[state->rtl_rx_next];
        uint32_t options = descriptor->options1;
        uint16_t length;
        if ((options & RTL8126_DESC_OWN) != 0U) break;
        __atomic_add_fetch(&state->rtl_rx_descriptor_count, 1U,
                           __ATOMIC_RELAXED);
        length = (uint16_t)(options & RTL8126_DESC_LENGTH);
        if ((options & (RTL8126_DESC_FS | RTL8126_DESC_LS)) !=
                (RTL8126_DESC_FS | RTL8126_DESC_LS) ||
            (options & RTL8126_RX_ERROR_MASK) != 0U) {
            length = 0U;
        } else if (length >= 4U) {
            length = (uint16_t)(length - 4U);
        } else {
            length = 0U;
        }
        if (length != 0U && length <= RTL8126_RX_BUFFER_SIZE) {
            __atomic_add_fetch(&state->rtl_rx_frame_count, 1U,
                               __ATOMIC_RELAXED);
            received = e1000_process_rx_frame(
                state,
                state->rtl_rx_buffers +
                    (size_t)state->rtl_rx_next * RTL8126_RX_BUFFER_SIZE,
                length) || received;
        }
        descriptor->options2 = 0U;
        descriptor->options1 = RTL8126_DESC_OWN | RTL8126_RX_DESCRIPTOR_LENGTH |
                               (state->rtl_rx_next + 1U == RTL8126_RING_COUNT ?
                                RTL8126_DESC_EOR : 0U);
        state->rtl_rx_next = (uint16_t)((state->rtl_rx_next + 1U) %
                                        RTL8126_RING_COUNT);
        ++processed;
    }
    dma_sync_for_device(&state->rtl_rx_buffer_dma);
    dma_sync_for_device(&state->rtl_rx_ring_dma);
    dma_wmb();
    bool dispatched = e1000_dispatch_software_queues(state);
    socket_tcp_poll(x86_read_tsc());
    rtl8126_complete_tx(state);
    /* 完成路径负责重新布置中断，并在释放排队标志后执行。若在此处提前启用，新 MSI 可能会合并到正在退出的
     * 工作线程。 */
    return received || dispatched;
}

bool rtl8126_interrupt_ready(void) {
    e1000_state_t *state = e1000_controller_state();
    return state != 0 && state->backend == NET_BACKEND_RTL8126 &&
           state->initialized && state->rtl_irq_bound;
}

bool rtl8126_firmware_required(void) {
    e1000_state_t *state = e1000_controller_state();
    if (state != 0 && state->backend == NET_BACKEND_RTL8126) {
        return state->rtl_firmware_required;
    }
    return rtl8126_hardware_present();
}

bool rtl8126_self_test(void) {
    e1000_state_t *state = e1000_controller_state();
    const pci_device_t *pci = rtl8126_pci_find();
    if (pci == 0) return true;
    e1000_self_test_begin();
    if (!rtl8126_initialize_state(state, pci) ||
        !rtl8126_bind_interrupt_state(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 4U);
        if (state->initialized) (void)rtl8126_destroy_state(state);
        return false;
    }
    e1000_runtime_set_ready(true);
    return true;
}
