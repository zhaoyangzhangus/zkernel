#pragma once

/* 在所属 socket 锁保护下、裁掉已交付前缀后调用。stream_write 之后的环槽位
 * 在数据连续前不可见。绝不分配内存或休眠，也不确认超出自有环形容量的数据。 */
static kstatus_t socket_tcp_reassemble_locked(socket_private_t *private,
                                               uint32_t sequence,
                                               uint32_t expected,
                                               uint8_t flags,
                                               const void *buffer,
                                               size_t length,
                                               bool *notify) {
    uint32_t count = atomic_load_explicit(&private->stream_count, memory_order_relaxed);
    uint32_t capacity = socket_stream_capacity(private);
    uint8_t *data = socket_stream_data(private);
    uint32_t distance = sequence - expected;
    uint32_t available = capacity - count;
    bool fin = (flags & NET_TCP_FLAG_FIN) != 0U;
    bool discard = atomic_load_explicit(&private->receive_shutdown, memory_order_acquire);
    *notify = false;
    if (atomic_load_explicit(&private->peer_closed, memory_order_acquire)) return K_OK;
    if (distance > available) return K_EAGAIN;
    uint32_t room = available - distance;
    if (room == 0U && (length != 0U || fin)) return K_EAGAIN;
    /* 接受通告窗口覆盖的部分，而非全有或全无。FIN 也占用一个序列号：
     * 位于右边界或其外的 FIN 在对端重传至窗口内前不会被确认。 */
    if (length > room) {
        length = room;
        fin = false;
    } else if (length == room) {
        fin = false;
    }

    if (discard || private->tcp_rx_present == 0 ||
        (distance == 0U && private->tcp_rx_future_count == 0U &&
         !private->tcp_rx_fin_pending)) {
        if (distance != 0U) return K_EAGAIN;
        if (!discard) {
            private->stream_write = socket_stream_write_locked(private, buffer, length);
            atomic_store_explicit(&private->stream_count, count + (uint32_t)length,
                                  memory_order_release);
        }
        private->tcp_sequence_valid = true;
        private->tcp_next_sequence = sequence + (uint32_t)length + (fin ? 1U : 0U);
        if (fin) atomic_store_explicit(&private->peer_closed, true, memory_order_release);
        *notify = (!discard && length != 0U) || fin;
        return K_OK;
    }

    /* 首个被接受的 FIN 确定流的结束位置。后续冲突的 FIN 不得改变该位置，
     * 边界之后的数据也不能变为可见。 */
    if (private->tcp_rx_fin_pending) {
        uint32_t end = private->tcp_rx_fin_sequence - expected;
        if (distance > end) return K_EAGAIN;
        if (length > end - distance) length = end - distance;
    } else if (fin) {
        private->tcp_rx_fin_sequence = sequence + (uint32_t)length;
        private->tcp_rx_fin_pending = true;
    }
    const uint8_t *input = buffer;
    uint32_t slot = (private->stream_write + distance) % capacity;
    size_t index = 0U;
    while (index < length) {
        uint8_t mask = (uint8_t)(1U << (slot & 7U));
        uint8_t *bits = &private->tcp_rx_present[slot >> 3U];
        /* 空/满的所有权字节可以按组插入或跳过。混合字节仍遵循下方的先到先得语义。
         * 复制范围同时受数据包和环尾限制，也要处理未对齐的尾部。 */
        if ((slot & 7U) == 0U && length - index >= 8U &&
            capacity - slot >= 8U &&
            (*bits == 0U || *bits == UINT8_MAX)) {
            /* 合并相同所有权的连续字节，一段负载只复制一次。
             * 不能跨过混合所有权、包尾或环尾，重复数据仍保持先到先得。 */
            size_t groups = (length - index) / 8U;
            size_t ring_groups = (capacity - slot) / 8U;
            if (groups > ring_groups) groups = ring_groups;
            size_t run = 1U;
            while (run < groups && bits[run] == *bits) ++run;
            size_t bytes = run * 8U;
            if (*bits == 0U) {
                socket_copy(data + slot, input + index, bytes);
                for (size_t group = 0U; group < run; ++group)
                    bits[group] = UINT8_MAX;
                private->tcp_rx_future_count += (uint32_t)bytes;
            }
            index += bytes;
            slot += (uint32_t)bytes;
            if (slot == capacity) slot = 0U;
            continue;
        }
        if ((*bits & mask) == 0U) {
            data[slot] = input[index];
            *bits |= mask;
            ++private->tcp_rx_future_count;
        }
        ++index;
        if (++slot == capacity) slot = 0U;
    }
    uint32_t contiguous = 0U;
    slot = private->stream_write;
    while (contiguous < available) {
        if (private->tcp_rx_fin_pending &&
            expected + contiguous == private->tcp_rx_fin_sequence) break;
        uint8_t mask = (uint8_t)(1U << (slot & 7U));
        uint8_t *bits = &private->tcp_rx_present[slot >> 3U];
        if ((*bits & mask) == 0U) break;
        /* 一个完整的所有权字节表示八个连续数据字节。不能越过接收窗口、环尾
         * 或已保留的 FIN。 */
        if ((slot & 7U) == 0U && *bits == UINT8_MAX &&
            available - contiguous >= 8U &&
            capacity - slot >= 8U &&
            (!private->tcp_rx_fin_pending ||
             private->tcp_rx_fin_sequence - expected - contiguous >= 8U)) {
            *bits = 0U;
            private->tcp_rx_future_count -= 8U;
            contiguous += 8U;
            slot += 8U;
            if (slot == capacity) slot = 0U;
            continue;
        }
        *bits &= (uint8_t)~mask;
        --private->tcp_rx_future_count;
        ++contiguous;
        if (++slot == capacity) slot = 0U;
    }
    private->stream_write = slot;
    private->tcp_sequence_valid = true;
    private->tcp_next_sequence = expected + contiguous;
    atomic_store_explicit(&private->stream_count, count + contiguous, memory_order_release);
    bool eof = private->tcp_rx_fin_pending &&
               private->tcp_next_sequence == private->tcp_rx_fin_sequence;
    if (eof) {
        ++private->tcp_next_sequence;
        private->tcp_rx_fin_pending = false;
        atomic_store_explicit(&private->peer_closed, true, memory_order_release);
    }
    *notify = contiguous != 0U || eof;
    return K_OK;
}
