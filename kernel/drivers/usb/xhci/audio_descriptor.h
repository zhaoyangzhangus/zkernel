#pragma once

/* 只发布完整匹配的播放备用接口，避免混合不同接口的格式和端点。 */
typedef struct xhci_audio_descriptor {
    uint8_t interface_number, alternate, protocol, control_interface, clock;
    uint8_t endpoint, interval, channels, bits, subslot;
    uint8_t mute_unit, volume_unit, volume_channels;
    uint16_t max_packet;
    uint32_t rate;
} xhci_audio_descriptor_t;

static bool xhci_select_audio_descriptor(const uint8_t *data, uint32_t size,
                                         xhci_audio_descriptor_t *out) {
    uint8_t terminal_clocks[256] = {0};
    uint8_t sources[256]={0}, mute[256]={0}, volume[256]={0};
    uint8_t control = 0U, protocol = 0U, terminal = 0U;
    bool audio_control = false, streaming = false, pcm = false, rate_ok = false;
    xhci_audio_descriptor_t candidate = {0};
    if (!data || !out || size < 9U) return false;
    for (uint32_t offset = 0; offset + 2U <= size;) {
        uint32_t length = data[offset];
        if (length < 2U || length > size - offset) return false;
        const uint8_t *d = data + offset;
        if (d[1] == 4U && length >= 9U) {
            audio_control = d[5] == 1U && d[6] == 1U;
            streaming = d[5] == 1U && d[6] == 2U && d[3] != 0U;
            if (audio_control) {control = d[2]; protocol = d[7];}
            candidate = (xhci_audio_descriptor_t){.interface_number=d[2],
                .alternate=d[3],.protocol=d[7],.control_interface=control};
            pcm = false; rate_ok = false; terminal = 0U;
        } else if (d[1] == 0x24U && length >= 3U) {
            if (audio_control && protocol == 0x20U && d[2] == 2U && length >= 17U)
                terminal_clocks[d[3]] = d[7];
            if (audio_control && protocol==0x20U && d[2]==4U && length>=7U && d[4])
                sources[d[3]]=d[5];
            if (audio_control && protocol==0x20U && d[2]==6U && length>=10U) {
                uint8_t id=d[3];sources[id]=d[4];
                if ((d[5]&3U)==3U) mute[id]=1U;
                uint32_t controls=(length-6U)/4U;
                for (uint32_t ch=0;ch<controls && ch<8U;++ch)
                    if ((d[5U+ch*4U]&12U)==12U) volume[id]|=(uint8_t)(1U<<ch);
            }
            if (streaming && d[2] == 1U) {
                if (candidate.protocol == 0x20U && length >= 16U) {
                    terminal = d[3];
                    pcm = d[5] == 1U && (d[6] & 1U) != 0U;
                    candidate.channels = d[10];
                } else if (candidate.protocol == 0U && length >= 7U) {
                    pcm = d[5] == 1U && d[6] == 0U;
                }
            } else if (streaming && length >= 4U && d[2] == 2U && d[3] == 1U) {
                if (candidate.protocol == 0x20U && length >= 6U) {
                    candidate.subslot = d[4]; candidate.bits = d[5];
                    candidate.clock = terminal_clocks[terminal];
                    rate_ok = candidate.clock != 0U;
                } else if (candidate.protocol == 0U && length >= 8U) {
                    candidate.channels = d[4]; candidate.subslot = d[5];
                    candidate.bits = d[6];
                    uint32_t rates = d[7];
                    if (rates == 0U && length >= 14U) {
                        uint32_t low = d[8] | ((uint32_t)d[9]<<8U) | ((uint32_t)d[10]<<16U);
                        uint32_t high = d[11] | ((uint32_t)d[12]<<8U) | ((uint32_t)d[13]<<16U);
                        rate_ok = low <= 48000U && high >= 48000U;
                    } else if (rates <= (length - 8U) / 3U) {
                        for (uint32_t i=0U; i<rates; ++i) {
                            uint32_t n=8U+i*3U;
                            if ((d[n] | ((uint32_t)d[n+1]<<8U) | ((uint32_t)d[n+2]<<16U)) == 48000U)
                                rate_ok = true;
                        }
                    }
                }
            }
        } else if (d[1] == 5U && length >= 7U && streaming &&
                   (d[2] & 0x80U) == 0U && (d[3] & 3U) == 1U &&
                   ((d[3] >> 4U) & 3U) == 0U && pcm && rate_ok &&
                   candidate.channels == 2U && candidate.subslot == 2U && candidate.bits == 16U) {
            candidate.endpoint = d[2] & 15U;
            candidate.max_packet = (d[4] | ((uint16_t)d[5]<<8U)) & 0x7FFU;
            candidate.interval = d[6]; candidate.rate = 48000U;
            if (!candidate.endpoint || !candidate.max_packet ||
                !candidate.interval || candidate.interval > 16U) return false;
            if (candidate.protocol==0x20U) {
                for (uint32_t id=1U;id<256U;++id) {
                    uint8_t source=sources[id];
                    for (uint32_t depth=0U;source && depth<256U;++depth) {
                        if (source==terminal) {
                            if (mute[id]) candidate.mute_unit=(uint8_t)id;
                            if (volume[id]) {
                                candidate.volume_unit=(uint8_t)id;
                                candidate.volume_channels=volume[id];
                            }
                            break;
                        }
                        source=sources[source];
                    }
                }
            }
            *out = candidate;
            return true;
        }
        offset += length;
    }
    return false;
}
