#pragma once
#include <stdint.h>
#include <stddef.h>

/* 有界解码；非法序列只消费一个字节，不越过字段边界或字符串结尾。 */
static inline uint32_t os_utf8_next(const char *text, size_t available, size_t *used) {
    *used = 0;
    if (!available || !text[0]) return 0;
    uint32_t first = (uint8_t)text[0], code = first;
    size_t count = first < 0x80 ? 1 : first >= 0xC2 && first <= 0xDF ? 2 :
        first >= 0xE0 && first <= 0xEF ? 3 : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
    *used = 1;
    if (!count || count > available) return 0xFFFD;
    if (count == 1) return code;
    code &= (1U << (7U - count)) - 1U;
    for (size_t i = 1; i < count; ++i) {
        uint32_t byte = (uint8_t)text[i];
        if ((byte & 0xC0U) != 0x80U) return 0xFFFD;
        code = (code << 6U) | (byte & 0x3FU);
    }
    if ((count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
        (count == 4 && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) return 0xFFFD;
    *used = count;
    return code;
}

/* 按相同的有界解码规则移动；损坏序列逐字节前进，不会卡在续字节上。 */
static inline size_t os_utf8_advance(const char *text,size_t length,size_t offset) {
    if (offset>=length) return length;
    size_t used;
    (void)os_utf8_next(text+offset,length-offset,&used);
    return offset+(used?used:1U);
}

static inline size_t os_utf8_previous(const char *text,size_t length,size_t offset) {
    if (offset>length) offset=length;
    if (!offset) return 0;
    size_t first=offset>4U?offset-4U:0;
    for (size_t candidate=first;candidate<offset;++candidate) {
        size_t used;
        (void)os_utf8_next(text+candidate,offset-candidate,&used);
        if (used && candidate+used==offset) return candidate;
    }
    return offset-1U;
}

/* 返回完整序列；非法标量统一编码为替代字符，调用方提供四字节空间。 */
static inline size_t os_utf8_encode(uint32_t code,char out[4]) {
    if (code>0x10ffffU || (code>=0xd800U && code<=0xdfffU)) code=0xfffdU;
    if (code<0x80U) {out[0]=(char)code;return 1;}
    if (code<0x800U) {
        out[0]=(char)(0xc0U|(code>>6U));out[1]=(char)(0x80U|(code&63U));return 2;
    }
    if (code<0x10000U) {
        out[0]=(char)(0xe0U|(code>>12U));out[1]=(char)(0x80U|((code>>6U)&63U));
        out[2]=(char)(0x80U|(code&63U));return 3;
    }
    out[0]=(char)(0xf0U|(code>>18U));out[1]=(char)(0x80U|((code>>12U)&63U));
    out[2]=(char)(0x80U|((code>>6U)&63U));out[3]=(char)(0x80U|(code&63U));return 4;
}
