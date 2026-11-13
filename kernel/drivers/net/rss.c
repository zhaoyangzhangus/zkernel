#include "internal.h"

/*
 * REFACTOR_P8_E1000_RSS_OWNER：流哈希和软件队列选择与控制器的 MMIO、DMA 及协议状态
 * 相互独立。
 */

static uint32_t e1000_hash_bytes(uint32_t hash, const uint8_t *bytes,
                                 size_t length) {
    while (length-- != 0U) {
        hash ^= *bytes++;
        hash *= 16777619U;
    }
    return hash;
}

static uint32_t e1000_hash_u16(uint32_t hash, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return e1000_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint32_t e1000_hash_u32(uint32_t hash, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                        (uint8_t)(value >> 8), (uint8_t)value};
    return e1000_hash_bytes(hash, bytes, sizeof(bytes));
}

uint32_t e1000_flow_hash_ipv4(uint32_t source_address,
                              uint32_t destination_address,
                              uint16_t source_port,
                              uint16_t destination_port,
                              uint8_t protocol) {
    uint32_t hash = 2166136261U;
    hash = e1000_hash_u32(hash, source_address);
    hash = e1000_hash_u32(hash, destination_address);
    hash = e1000_hash_u16(hash, source_port);
    hash = e1000_hash_u16(hash, destination_port);
    return e1000_hash_bytes(hash, &protocol, 1U);
}

uint32_t e1000_flow_hash_ipv6(const uint8_t source_address[16],
                              const uint8_t destination_address[16],
                              uint16_t source_port,
                              uint16_t destination_port) {
    uint32_t hash = 2166136261U;
    hash = e1000_hash_bytes(hash, source_address, 16U);
    hash = e1000_hash_bytes(hash, destination_address, 16U);
    hash = e1000_hash_u16(hash, source_port);
    hash = e1000_hash_u16(hash, destination_port);
    return e1000_hash_bytes(hash, (const uint8_t[]){17U}, 1U);
}

uint32_t e1000_select_software_queue(uint32_t software_queue_count,
                                     uint32_t flow_hash) {
    if (software_queue_count == 0U) return 0U;
    return flow_hash % software_queue_count;
}

bool e1000_rss_self_test_state(uint32_t software_queue_count) {
    bool seen[E1000_SOFTWARE_QUEUE_COUNT] = {false};
    uint32_t required;
    if (software_queue_count == 0U ||
        software_queue_count > E1000_SOFTWARE_QUEUE_COUNT) return false;
    required = software_queue_count;
    for (uint32_t flow = 0; flow < 256U && required != 0U; ++flow) {
        uint32_t source = 0x0A000001U + flow;
        uint32_t destination = 0x0A000100U + (flow & 31U);
        uint32_t queue = e1000_select_software_queue(
            software_queue_count,
            e1000_flow_hash_ipv4(source, destination,
                                 (uint16_t)(1000U + flow), 2000U,
                                 SOCKET_PROTOCOL_UDP));
        if (!seen[queue]) {
            seen[queue] = true;
            --required;
        }
    }
    return required == 0U;
}
