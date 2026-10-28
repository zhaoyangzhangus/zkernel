#pragma once
#pragma once

#include "base.h"

struct device;
struct audio_stream;

/* Intel HDA 的最小控制器接口；PCM 策略和混音仍由用户态音频服务负责。 */
bool hda_hardware_self_test(void);
bool hda_hardware_present(void);
uint32_t hda_last_error(void);
uint8_t hda_output_stream_count(void);
uint8_t hda_input_stream_count(void);
bool hda_controller_reset(void);
/* 验证一个最小的播放 BDL/PCM DMA 流；完整 codec 路由由音频服务负责。 */
bool hda_pcm_self_test(void);
/* 返回已注册的 HDA PCI 设备对象，供音频 syscall 创建 DMA 流。 */
struct device *hda_audio_device(void);

/* 将内核音频流绑定到 HDA BDL；未使用 HDA 后端时返回 K_ENOENT。 */
kstatus_t hda_audio_stream_configure(struct audio_stream *stream);
kstatus_t hda_audio_stream_start(struct audio_stream *stream);
kstatus_t hda_audio_stream_stop(struct audio_stream *stream);
kstatus_t hda_audio_stream_reset(struct audio_stream *stream);
kstatus_t hda_audio_stream_disconnect(struct audio_stream *stream);

kstatus_t hda_audio_stream_complete(struct audio_stream *stream, uint32_t period, uint32_t frames);
