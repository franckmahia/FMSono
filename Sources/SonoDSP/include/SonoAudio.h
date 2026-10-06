#ifndef SONO_AUDIO_H
#define SONO_AUDIO_H
#include "SonoDSP.h"
#include <stddef.h>

typedef struct {
    uint32_t id, input_channels, output_channels;
    double sample_rate;
    bool bluetooth, default_input, default_output;
    char name[256], uid[256];
} SonoDeviceInfo;

typedef struct SonoAudio SonoAudio;
typedef struct {
    SonoMetrics dsp;
    int32_t error_code;
    bool route_changed;
    double input_rate, output_rate, estimated_latency_ms;
} SonoAudioStatus;

int32_t sono_devices(SonoDeviceInfo *devices, uint32_t capacity);
uint32_t sono_default_input(void);
uint32_t sono_default_output(void);
SonoAudio *sono_audio_open(uint32_t input, uint32_t output, uint32_t channel,
                          bool follows_default, char *error, size_t error_capacity);
void sono_audio_close(SonoAudio *audio);
void sono_audio_gain(SonoAudio *audio, float db);
void sono_audio_mute(SonoAudio *audio, bool muted);
void sono_audio_protection(SonoAudio *audio, bool enabled);
SonoAudioStatus sono_audio_status(SonoAudio *audio);
#endif
