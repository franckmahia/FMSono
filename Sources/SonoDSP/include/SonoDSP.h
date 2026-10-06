#ifndef SONO_DSP_H
#define SONO_DSP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct SonoProcessor SonoProcessor;
typedef struct {
    float input_peak, output_peak, limiter_reduction_db, safety_reduction_db;
    float feedback_hz, buffer_ms;
    uint32_t notch_count, underruns, overruns, fault_reason;
    bool tripped;
} SonoMetrics;

SonoProcessor *sono_create(double input_rate, double output_rate, uint32_t target_frames);
void sono_destroy(SonoProcessor *processor);
void sono_set_gain(SonoProcessor *processor, float gain_db);
void sono_set_muted(SonoProcessor *processor, bool muted);
void sono_set_feedback_enabled(SonoProcessor *processor, bool enabled);
void sono_push(SonoProcessor *processor, const float *samples, uint32_t frames);
void sono_render(SonoProcessor *processor, float *samples, uint32_t frames);
SonoMetrics sono_metrics(SonoProcessor *processor);
// For deterministic offline verification, also used by the real output callback.
void sono_process(SonoProcessor *processor, const float *input, float *output, uint32_t frames);

#endif
