// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

int audio_init(void);
int audio_device_present(void);
const char* audio_backend_name(void);
int audio_codec_ready(void);
void audio_beep(uint32_t frequency, uint32_t milliseconds);

#endif
