#include <kernel/net_core.h>
#include <kernel/sched.h>
#include <uapi/socket.h>
#include "internal.h"

/* REFACTOR_P8_NET_FIREWALL_OWNER：防火墙规则状态和数据包策略。 */

#define NET_FIREWALL_PROTOCOL_UDP 17U

static void net_firewall_lock(net_firewall_t *firewall) {
    sched_preempt_disable();
    spinlock_lock(&firewall->lock);
}

static void net_firewall_unlock(net_firewall_t *firewall) {
    spinlock_unlock(&firewall->lock);
    sched_preempt_enable();
}

static bool net_firewall_prefix_matches(const uint8_t left[16],
                                        const uint8_t right[16],
                                        uint8_t prefix_length,
                                        uint8_t maximum_length) {
    uint8_t full_bytes;
    uint8_t remainder;
    if (prefix_length > maximum_length) return false;
    full_bytes = (uint8_t)(prefix_length / 8U);
    remainder = (uint8_t)(prefix_length & 7U);
    for (uint8_t index = 0; index < full_bytes; ++index) {
        if (left[index] != right[index]) return false;
    }
    if (remainder != 0U) {
        uint8_t mask = (uint8_t)(0xFFU << (8U - remainder));
        if ((left[full_bytes] & mask) != (right[full_bytes] & mask)) return false;
    }
    return true;
}

static bool net_firewall_rule_matches(const net_firewall_rule_t *rule,
                                      const net_firewall_packet_t *packet) {
    uint8_t maximum_prefix = packet->family == OS_AF_INET4 ? 32U : 128U;
    if (!rule->enabled || rule->family != packet->family ||
        (rule->direction != 0U && rule->direction != packet->direction) ||
        (rule->protocol != 0U && rule->protocol != packet->protocol) ||
        (rule->source_port != 0U && rule->source_port != packet->source_port) ||
        (rule->destination_port != 0U &&
         rule->destination_port != packet->destination_port) ||
        !net_firewall_prefix_matches(rule->source_address, packet->source_address,
                                     rule->source_prefix_length, maximum_prefix) ||
        !net_firewall_prefix_matches(rule->destination_address,
                                     packet->destination_address,
                                     rule->destination_prefix_length, maximum_prefix)) {
        return false;
    }
    return true;
}

void net_firewall_init(net_firewall_t *firewall, uint8_t default_action) {
    if (firewall == 0) return;
    net_zero(firewall, sizeof(*firewall));
    spinlock_init(&firewall->lock);
    firewall->default_action = default_action == NET_FIREWALL_ALLOW ?
                               NET_FIREWALL_ALLOW : NET_FIREWALL_DENY;
}

kstatus_t net_firewall_add(net_firewall_t *firewall,
                           const net_firewall_rule_t *rule,
                           uint32_t *rule_id) {
    if (firewall == 0 || rule == 0 || rule_id == 0 || !rule->enabled ||
        (rule->action != NET_FIREWALL_ALLOW && rule->action != NET_FIREWALL_DENY) ||
        (rule->family != OS_AF_INET4 && rule->family != OS_AF_INET6) ||
        rule->direction > NET_FIREWALL_EGRESS || rule->source_prefix_length > 128U ||
        rule->destination_prefix_length > 128U) return K_EINVAL;
    if (rule->family == OS_AF_INET4 &&
        (rule->source_prefix_length > 32U || rule->destination_prefix_length > 32U)) {
        return K_EINVAL;
    }
    net_firewall_lock(firewall);
    for (uint32_t index = 0; index < NET_FIREWALL_MAX_RULES; ++index) {
        if (firewall->rules[index].enabled) continue;
        firewall->rules[index] = *rule;
        firewall->rules[index].enabled = true;
        *rule_id = index;
        net_firewall_unlock(firewall);
        return K_OK;
    }
    net_firewall_unlock(firewall);
    return K_ENOMEM;
}

kstatus_t net_firewall_remove(net_firewall_t *firewall, uint32_t rule_id) {
    if (firewall == 0 || rule_id >= NET_FIREWALL_MAX_RULES) return K_EINVAL;
    net_firewall_lock(firewall);
    if (!firewall->rules[rule_id].enabled) {
        net_firewall_unlock(firewall);
        return K_ENOENT;
    }
    firewall->rules[rule_id].enabled = false;
    net_firewall_unlock(firewall);
    return K_OK;
}

bool net_firewall_check(net_firewall_t *firewall,
                        const net_firewall_packet_t *packet) {
    bool allowed;
    if (firewall == 0 || packet == 0 ||
        (packet->family != OS_AF_INET4 && packet->family != OS_AF_INET6) ||
        (packet->direction != NET_FIREWALL_INGRESS &&
         packet->direction != NET_FIREWALL_EGRESS)) return false;
    net_firewall_lock(firewall);
    allowed = firewall->default_action == NET_FIREWALL_ALLOW;
    for (uint32_t index = 0; index < NET_FIREWALL_MAX_RULES; ++index) {
        if (!net_firewall_rule_matches(&firewall->rules[index], packet)) continue;
        allowed = firewall->rules[index].action == NET_FIREWALL_ALLOW;
        break;
    }
    net_firewall_unlock(firewall);
    return allowed;
}

bool net_firewall_self_test(void) {
    net_firewall_t firewall;
    net_firewall_rule_t rule = {0};
    net_firewall_packet_t packet = {0};
    uint32_t rule_id = UINT32_MAX;
    net_firewall_init(&firewall, NET_FIREWALL_DENY);
    rule.enabled = true;
    rule.direction = NET_FIREWALL_INGRESS;
    rule.protocol = NET_FIREWALL_PROTOCOL_UDP;
    rule.family = OS_AF_INET4;
    rule.destination_port = 53U;
    rule.action = NET_FIREWALL_ALLOW;
    if (net_firewall_add(&firewall, &rule, &rule_id) != K_OK ||
        rule_id >= NET_FIREWALL_MAX_RULES) return false;
    packet.family = OS_AF_INET4;
    packet.direction = NET_FIREWALL_INGRESS;
    packet.protocol = NET_FIREWALL_PROTOCOL_UDP;
    packet.destination_port = 53U;
    if (!net_firewall_check(&firewall, &packet)) return false;
    packet.destination_port = 54U;
    if (net_firewall_check(&firewall, &packet) ||
        net_firewall_remove(&firewall, rule_id) != K_OK ||
        net_firewall_remove(&firewall, rule_id) != K_ENOENT) return false;
    return !net_firewall_check(&firewall, &packet);
}
