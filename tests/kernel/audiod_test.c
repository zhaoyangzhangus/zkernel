#include "mixer.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

static os_audio_stream_config_t test_config(uint16_t sample_format) {
    os_audio_stream_config_t config = {0};
    config.hdr.size = sizeof(config);
    config.hdr.version = 1U;
    config.direction = OS_AUDIO_PLAYBACK;
    config.sample_rate = 48000U;
    config.channels = 2U;
    config.sample_format = sample_format;
    config.period_frames = 128U;
    config.period_count = 4U;
    return config;
}

/* 独立按样本位宽累加，覆盖负数、饱和、空客户端和环回绕。 */
static void test_formats(void) {
    const uint16_t formats[] = {OS_AUDIO_SAMPLE_S16_LE, OS_AUDIO_SAMPLE_S24_LE,
                                OS_AUDIO_SAMPLE_S32_LE};
    for (unsigned f = 0; f < 3; ++f) {
        audiod_server_t server;
        os_audio_stream_config_t config = test_config(formats[f]);
        unsigned bytes = f + 2U;
        int64_t maximum = (INT64_C(1) << (bytes * 8U - 1U)) - 1;
        int64_t minimum = -maximum - 1;
        uint32_t ids[AUDIOD_MAX_CLIENTS];
        uint8_t input[AUDIOD_MAX_CLIENTS][67 * 2 * 4], output[67 * 2 * 4];
        assert(audiod_server_init(&server) == AUDIOD_OK);
        for (unsigned i = 0; i < AUDIOD_MAX_CLIENTS; ++i)
            assert(audiod_client_open(&server, &config, &ids[i]) == AUDIOD_OK);
        for (unsigned round = 0; round < 20; ++round) {
            int64_t expected[67 * 2] = {0};
            for (unsigned i = 0; i < AUDIOD_MAX_CLIENTS; ++i) {
                /* 最后一个客户端保持空队列，验证混音不中断及 underrun 统计。 */
                if (i == AUDIOD_MAX_CLIENTS - 1) continue;
                for (unsigned sample = 0; sample < 67 * 2; ++sample) {
                    unsigned kind = (sample + round) % 5;
                    int64_t value = kind == 0 ? maximum : kind == 1 ? minimum :
                                    kind == 2 ? -(int64_t)(i + 1) :
                                    kind == 3 ? (int64_t)(i + 1) : 0;
                    expected[sample] += value;
                    uint32_t bits = (uint32_t)value;
                    for (unsigned b = 0; b < bytes; ++b)
                        input[i][sample * bytes + b] = (uint8_t)(bits >> (b * 8));
                }
                assert(audiod_client_submit(&server, ids[i], input[i], 67) == AUDIOD_OK);
            }
            assert(audiod_mix(&server, output, sizeof(output), 67) == AUDIOD_OK);
            for (unsigned sample = 0; sample < 67 * 2; ++sample) {
                int64_t value = expected[sample];
                if (value > maximum) value = maximum;
                if (value < minimum) value = minimum;
                uint32_t bits = (uint32_t)value;
                for (unsigned b = 0; b < bytes; ++b)
                    assert(output[sample * bytes + b] == (uint8_t)(bits >> (b * 8)));
            }
        }
        assert(server.mixed_frames == 20 * 67);
        for (unsigned i = 0; i < AUDIOD_MAX_CLIENTS; ++i) {
            assert(server.clients[ids[i]].queued_frames == 0);
            assert(server.clients[ids[i]].underruns ==
                   (i == AUDIOD_MAX_CLIENTS - 1 ? 20 * 67U : 0U));
            assert(audiod_client_close(&server, ids[i]) == AUDIOD_OK);
        }
    }
}

int main(void) {
    audiod_server_t server = {0};
    os_audio_stream_config_t config = test_config(OS_AUDIO_SAMPLE_S16_LE);
    os_audio_stream_stats_t stats = {0};
    uint32_t left = AUDIOD_INVALID_CLIENT;
    uint32_t right = AUDIOD_INVALID_CLIENT;
    int16_t first[] = {1000, -1000};
    int16_t second[] = {2000, 1000};
    int16_t mixed[2] = {0};
    stats.hdr.size = sizeof(stats);
    stats.hdr.version = 1U;
    if (audiod_server_init(&server) != AUDIOD_OK ||
        audiod_client_open(&server, &config, &left) != AUDIOD_OK ||
        audiod_client_open(&server, &config, &right) != AUDIOD_OK ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_OK ||
        audiod_client_submit(&server, right, second, 1U) != AUDIOD_OK ||
        audiod_mix(&server, mixed, sizeof(mixed), 1U) != AUDIOD_OK ||
        mixed[0] != 3000 || mixed[1] != 0) {
        puts("audiod: fail");
        return 1;
    }
    if (audiod_mix(&server, mixed, sizeof(mixed), 1U) != AUDIOD_OK ||
        audiod_client_get_stats(&server, left, &stats) != AUDIOD_OK ||
        stats.underruns == 0U || stats.mixed_frames != 2U ||
        audiod_device_disconnect(&server) != AUDIOD_OK ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_EDEVREMOVED ||
        audiod_device_recover(&server) != AUDIOD_OK ||
        audiod_client_get_stats(&server, left, &stats) != AUDIOD_OK ||
        stats.device_generation != 3U || stats.state != OS_AUDIO_STATE_READY ||
        audiod_client_submit(&server, left, first, 1U) != AUDIOD_OK ||
        audiod_client_close(&server, left) != AUDIOD_OK ||
        audiod_client_close(&server, right) != AUDIOD_OK) {
        puts("audiod: fail");
        return 1;
    }
    test_formats();
    puts("audiod: mixing, signed PCM boundaries, saturation, underruns and ring wrap passed");
    return 0;
}
