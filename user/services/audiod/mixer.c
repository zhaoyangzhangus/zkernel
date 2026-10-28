#include "mixer.h"

static uint32_t audiod_sample_bytes(uint16_t format) {
    switch (format) {
        case OS_AUDIO_SAMPLE_S16_LE: return 2U;
        case OS_AUDIO_SAMPLE_S24_LE: return 3U;
        case OS_AUDIO_SAMPLE_S32_LE: return 4U;
        default: return 0U;
    }
}

static bool audiod_header_valid(const os_versioned_header_t *header,
                                size_t size) {
    return header != 0 && header->size >= size && header->version == 1U &&
           header->flags == 0U;
}

static bool audiod_config_valid(const os_audio_stream_config_t *config) {
    return config != 0 && audiod_header_valid(&config->hdr, sizeof(*config)) &&
           config->direction == OS_AUDIO_PLAYBACK &&
           config->sample_rate >= OS_AUDIO_MIN_RATE &&
           config->sample_rate <= OS_AUDIO_MAX_RATE && config->channels != 0U &&
           config->channels <= OS_AUDIO_MAX_CHANNELS &&
           audiod_sample_bytes(config->sample_format) != 0U &&
           config->period_frames >= OS_AUDIO_MIN_PERIOD_FRAMES &&
           config->period_frames <= OS_AUDIO_MAX_PERIOD_FRAMES &&
           config->period_count >= 2U &&
           config->period_count <= OS_AUDIO_MAX_PERIOD_COUNT;
}

static bool audiod_config_equal(const os_audio_stream_config_t *left,
                                const os_audio_stream_config_t *right) {
    return left->direction == right->direction &&
           left->sample_rate == right->sample_rate &&
           left->channels == right->channels &&
           left->sample_format == right->sample_format &&
           left->period_frames == right->period_frames &&
           left->period_count == right->period_count;
}

static int32_t audiod_saturate_i64(int64_t value) {
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static int32_t audiod_read_sample(const uint8_t *source, uint16_t format) {
    if (format == OS_AUDIO_SAMPLE_S16_LE) {
        int16_t value = (int16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8));
        return (int32_t)value * 65536;
    }
    if (format == OS_AUDIO_SAMPLE_S24_LE) {
        int32_t value = (int32_t)source[0] | ((int32_t)source[1] << 8) |
                        ((int32_t)source[2] << 16);
        if ((value & 0x00800000) != 0) value |= (int32_t)0xFF000000U;
        return value * 256;
    }
    return (int32_t)((uint32_t)source[0] | ((uint32_t)source[1] << 8) |
                     ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24));
}

static void audiod_write_sample(uint8_t *destination, uint16_t format,
                                int32_t value) {
    if (format == OS_AUDIO_SAMPLE_S16_LE) {
        int32_t sample = value >> 16;
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        destination[0] = (uint8_t)(uint16_t)sample;
        destination[1] = (uint8_t)((uint16_t)sample >> 8);
    } else if (format == OS_AUDIO_SAMPLE_S24_LE) {
        int32_t sample = value >> 8;
        if (sample > 0x7FFFFF) sample = 0x7FFFFF;
        if (sample < -0x800000) sample = -0x800000;
        destination[0] = (uint8_t)sample;
        destination[1] = (uint8_t)(sample >> 8);
        destination[2] = (uint8_t)(sample >> 16);
    } else {
        destination[0] = (uint8_t)(uint32_t)value;
        destination[1] = (uint8_t)((uint32_t)value >> 8);
        destination[2] = (uint8_t)((uint32_t)value >> 16);
        destination[3] = (uint8_t)((uint32_t)value >> 24);
    }
}

static bool audiod_client_valid(const audiod_server_t *server, uint32_t client_id) {
    return server != 0 && client_id < AUDIOD_MAX_CLIENTS &&
           server->clients[client_id].used;
}

int audiod_server_init(audiod_server_t *server) {
    if (server == 0) return AUDIOD_EINVAL;
    for (size_t i = 0U; i < sizeof(*server); ++i) ((uint8_t *)server)[i] = 0U;
    server->initialized = true;
    server->state = OS_AUDIO_STATE_READY;
    server->device_generation = 1U;
    return AUDIOD_OK;
}

int audiod_client_open(audiod_server_t *server,
                       const os_audio_stream_config_t *config,
                       uint32_t *client_id) {
    if (server == 0 || !server->initialized || config == 0 || client_id == 0 ||
        !audiod_config_valid(config)) return AUDIOD_EINVAL;
    if (server->state == OS_AUDIO_STATE_DISCONNECTED) return AUDIOD_EDEVREMOVED;
    if (server->config.hdr.size != 0U && !audiod_config_equal(&server->config, config)) {
        return AUDIOD_EBUSY;
    }
    for (uint32_t i = 0U; i < AUDIOD_MAX_CLIENTS; ++i) {
        if (server->clients[i].used) continue;
        audiod_client_t *client = &server->clients[i];
        client->used = true;
        client->config = *config;
        if (server->config.hdr.size == 0U) server->config = *config;
        *client_id = i;
        return AUDIOD_OK;
    }
    return AUDIOD_ENOMEM;
}

int audiod_client_close(audiod_server_t *server, uint32_t client_id) {
    if (!audiod_client_valid(server, client_id)) return AUDIOD_EINVAL;
    server->clients[client_id] = (audiod_client_t){0};
    bool any_client = false;
    for (uint32_t i = 0U; i < AUDIOD_MAX_CLIENTS; ++i) {
        if (server->clients[i].used) {
            any_client = true;
            break;
        }
    }
    if (!any_client) server->config = (os_audio_stream_config_t){0};
    return AUDIOD_OK;
}

int audiod_client_submit(audiod_server_t *server, uint32_t client_id,
                         const void *samples, uint32_t frames) {
    audiod_client_t *client;
    uint32_t channels;
    uint32_t sample_bytes;
    if (!audiod_client_valid(server, client_id) || samples == 0 || frames == 0U) {
        return AUDIOD_EINVAL;
    }
    if (server->state == OS_AUDIO_STATE_DISCONNECTED) return AUDIOD_EDEVREMOVED;
    client = &server->clients[client_id];
    channels = client->config.channels;
    sample_bytes = audiod_sample_bytes(client->config.sample_format);
    if (frames > AUDIOD_RING_FRAMES - client->queued_frames) {
        ++client->overruns;
        return AUDIOD_EBUSY;
    }
    const uint8_t *input = (const uint8_t *)samples;
    for (uint32_t frame = 0U; frame < frames; ++frame) {
        uint32_t destination_frame =
            (client->write_frame + frame) % AUDIOD_RING_FRAMES;
        for (uint32_t channel = 0U; channel < channels; ++channel) {
            client->samples[destination_frame * OS_AUDIO_MAX_CHANNELS + channel] =
                audiod_read_sample(input +
                    ((size_t)frame * channels + channel) * sample_bytes,
                    client->config.sample_format);
        }
    }
    client->write_frame = (client->write_frame + frames) % AUDIOD_RING_FRAMES;
    client->queued_frames += frames;
    client->submitted_frames += frames;
    return AUDIOD_OK;
}

int audiod_mix(audiod_server_t *server, void *output, size_t output_bytes,
               uint32_t frames) {
    uint32_t channels;
    uint32_t sample_bytes;
    size_t required;
    if (server == 0 || !server->initialized || output == 0 || frames == 0U ||
        server->config.hdr.size == 0U || server->state == OS_AUDIO_STATE_DISCONNECTED) {
        return server != 0 && server->state == OS_AUDIO_STATE_DISCONNECTED ?
            AUDIOD_EDEVREMOVED : AUDIOD_EINVAL;
    }
    channels = server->config.channels;
    sample_bytes = audiod_sample_bytes(server->config.sample_format);
    if ((size_t)frames > SIZE_MAX / channels / sample_bytes) return AUDIOD_EINVAL;
    required = (size_t)frames * channels * sample_bytes;
    if (output_bytes < required) return AUDIOD_EINVAL;
    /* 客户端集合在同一批混音期间不变，只收集一次，避免逐帧扫描空槽。 */
    audiod_client_t *clients[AUDIOD_MAX_CLIENTS];
    uint32_t client_count = 0U;
    for (uint32_t i = 0U; i < AUDIOD_MAX_CLIENTS; ++i) {
        if (server->clients[i].used) clients[client_count++] = &server->clients[i];
    }
    uint8_t *destination = (uint8_t *)output;
    for (uint32_t frame = 0U; frame < frames; ++frame) {
        for (uint32_t i = 0U; i < client_count; ++i) {
            audiod_client_t *client = clients[i];
            if (client->queued_frames == 0U) ++client->underruns;
        }
        for (uint32_t channel = 0U; channel < channels; ++channel) {
            int64_t sum = 0;
            for (uint32_t i = 0U; i < client_count; ++i) {
                audiod_client_t *client = clients[i];
                if (client->queued_frames == 0U) continue;
                sum += client->samples[client->read_frame * OS_AUDIO_MAX_CHANNELS + channel];
            }
            audiod_write_sample(destination +
                ((size_t)frame * channels + channel) * sample_bytes,
                server->config.sample_format, audiod_saturate_i64(sum));
        }
        for (uint32_t i = 0U; i < client_count; ++i) {
            audiod_client_t *client = clients[i];
            if (client->queued_frames == 0U) continue;
            client->read_frame = (client->read_frame + 1U) % AUDIOD_RING_FRAMES;
            --client->queued_frames;
        }
    }
    server->state = OS_AUDIO_STATE_RUNNING;
    server->mixed_frames += frames;
    return AUDIOD_OK;
}

int audiod_device_disconnect(audiod_server_t *server) {
    if (server == 0 || !server->initialized) return AUDIOD_EINVAL;
    server->state = OS_AUDIO_STATE_DISCONNECTED;
    ++server->device_generation;
    return AUDIOD_OK;
}

int audiod_device_recover(audiod_server_t *server) {
    if (server == 0 || !server->initialized) return AUDIOD_EINVAL;
    for (uint32_t i = 0U; i < AUDIOD_MAX_CLIENTS; ++i) {
        server->clients[i].read_frame = 0U;
        server->clients[i].write_frame = 0U;
        server->clients[i].queued_frames = 0U;
    }
    server->state = OS_AUDIO_STATE_READY;
    ++server->device_generation;
    return AUDIOD_OK;
}

int audiod_client_get_stats(const audiod_server_t *server, uint32_t client_id,
                            os_audio_stream_stats_t *stats) {
    const audiod_client_t *client;
    if (!audiod_client_valid(server, client_id) || stats == 0 ||
        !audiod_header_valid(&stats->hdr, sizeof(*stats))) return AUDIOD_EINVAL;
    client = &server->clients[client_id];
    stats->queued_frames = client->queued_frames;
    stats->mixed_frames = server->mixed_frames;
    stats->underruns = client->underruns;
    stats->overruns = client->overruns;
    stats->device_generation = server->device_generation;
    stats->state = server->state;
    stats->reserved = 0U;
    return AUDIOD_OK;
}
