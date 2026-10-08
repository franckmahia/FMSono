#include "SonoDSP.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE 32768u
#define FFT_SIZE 2048u
#define HOP 256u
#define MAX_NOTCHES 4u
#define CEILING 0.5f

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } Biquad;
struct SonoProcessor {
    double input_rate, output_rate, position;
    uint32_t target_frames;
    float ring[RING_SIZE];
    _Atomic uint64_t write_index, read_index;
    _Atomic float gain_db;
    _Atomic bool muted, feedback_enabled, tripped;
    _Atomic uint32_t fault_reason;
    _Atomic float input_peak, output_peak, limiter_db, safety_db, feedback_hz;
    _Atomic uint32_t notch_count, underruns, overruns;
    bool primed;
    uint32_t consecutive_underruns;
    double smooth_gain, limiter_gain, safety_gain, gain_ramp, limiter_release;
    Biquad highpass, notches[MAX_NOTCHES];
    Biquad resample_filters[2];
    bool downsampling;
    float notch_hz[MAX_NOTCHES];
    uint32_t active_notches;
    float analysis[FFT_SIZE], real[FFT_SIZE], imaginary[FFT_SIZE];
    float window[FFT_SIZE], twiddle_real[FFT_SIZE / 2], twiddle_imag[FFT_SIZE / 2];
    uint32_t analysis_index, analysis_frames, hop_count;
    int candidate_bin;
    uint32_t candidate_frames;
    double candidate_power;
};

static double filtered(Biquad *b, double x) {
    double y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static void configure_filter(Biquad *b, double rate, double hz, bool notch) {
    double w = 2 * M_PI * hz / rate, c = cos(w);
    double alpha = sin(w) / (2 * (notch ? 18.0 : 0.70710678)), a0 = 1 + alpha;
    *b = (Biquad){ .b0 = (notch ? 1 : (1 + c) / 2) / a0,
        .b1 = (notch ? -2 * c : -(1 + c)) / a0,
        .b2 = (notch ? 1 : (1 + c) / 2) / a0,
        .a1 = -2 * c / a0, .a2 = (1 - alpha) / a0 };
}

SonoProcessor *sono_create(double input_rate, double output_rate, uint32_t target_frames) {
    if (!isfinite(input_rate) || !isfinite(output_rate) || input_rate < 8000 ||
        output_rate < 8000 || input_rate > 192000 || output_rate > 192000 ||
        target_frames == 0 || target_frames > RING_SIZE / 4) return NULL;
    SonoProcessor *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->input_rate = input_rate; p->output_rate = output_rate; p->target_frames = target_frames;
    atomic_init(&p->write_index, 0); atomic_init(&p->read_index, 0);
    atomic_init(&p->gain_db, -18); atomic_init(&p->muted, true);
    atomic_init(&p->feedback_enabled, true); atomic_init(&p->tripped, false);
    atomic_init(&p->fault_reason, 0);
    atomic_init(&p->input_peak, 0); atomic_init(&p->output_peak, 0);
    atomic_init(&p->limiter_db, 0); atomic_init(&p->safety_db, 0);
    atomic_init(&p->feedback_hz, 0); atomic_init(&p->notch_count, 0);
    atomic_init(&p->underruns, 0); atomic_init(&p->overruns, 0);
    p->limiter_gain = 1; p->safety_gain = 1; p->candidate_bin = -1;
    p->gain_ramp = 1 - exp(-1 / (0.025 * output_rate));
    p->limiter_release = 1 - exp(-1 / (0.12 * output_rate));
    configure_filter(&p->highpass, output_rate, 90, false);
    p->downsampling = input_rate > output_rate;
    if (p->downsampling) {
        const double quality[2] = {0.5411961, 1.3065630};
        for (uint32_t i = 0; i < 2; i++) {
            double w = 2 * M_PI * output_rate * 0.42 / input_rate;
            double c = cos(w), alpha = sin(w) / (2 * quality[i]), a0 = 1 + alpha;
            p->resample_filters[i] = (Biquad){ .b0 = (1 - c) / (2 * a0), .b1 = (1 - c) / a0,
                .b2 = (1 - c) / (2 * a0), .a1 = -2 * c / a0, .a2 = (1 - alpha) / a0 };
        }
    }
    for (uint32_t i = 0; i < FFT_SIZE; i++) p->window[i] = 0.5f - 0.5f * cosf(2 * M_PI * i / FFT_SIZE);
    for (uint32_t i = 0; i < FFT_SIZE / 2; i++) {
        p->twiddle_real[i] = cosf(2 * M_PI * i / FFT_SIZE);
        p->twiddle_imag[i] = -sinf(2 * M_PI * i / FFT_SIZE);
    }
    return p;
}

void sono_destroy(SonoProcessor *p) { free(p); }
void sono_set_gain(SonoProcessor *p, float db) {
    if (p) atomic_store_explicit(&p->gain_db, isfinite(db) ? fminf(12, fmaxf(-60, db)) : -60, memory_order_relaxed);
}
void sono_set_muted(SonoProcessor *p, bool muted) { if (p) atomic_store(&p->muted, muted); }
void sono_set_feedback_enabled(SonoProcessor *p, bool enabled) {
    if (p) atomic_store(&p->feedback_enabled, enabled);
}

static float power_at(SonoProcessor *p, uint32_t bin) {
    return p->real[bin] * p->real[bin] + p->imaginary[bin] * p->imaginary[bin];
}

static void detect_feedback(SonoProcessor *p) {
    // Fixed-size, allocation-free radix-2 FFT on the output thread.
    for (uint32_t i = 0, j = 0; i < FFT_SIZE; i++) {
        p->real[j] = p->analysis[(p->analysis_index + i) % FFT_SIZE] * p->window[i];
        p->imaginary[j] = 0;
        uint32_t bit = FFT_SIZE >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
    }
    for (uint32_t length = 2; length <= FFT_SIZE; length <<= 1) {
        uint32_t half = length / 2, step = FFT_SIZE / length;
        for (uint32_t start = 0; start < FFT_SIZE; start += length) {
            for (uint32_t j = 0; j < half; j++) {
                uint32_t a = start + j, b = a + half, t = j * step;
                float re = p->real[b] * p->twiddle_real[t] - p->imaginary[b] * p->twiddle_imag[t];
                float im = p->real[b] * p->twiddle_imag[t] + p->imaginary[b] * p->twiddle_real[t];
                p->real[b] = p->real[a] - re; p->imaginary[b] = p->imaginary[a] - im;
                p->real[a] += re; p->imaginary[a] += im;
            }
        }
    }
    uint32_t lo = (uint32_t)(250 * FFT_SIZE / p->output_rate);
    uint32_t hi = (uint32_t)(fmin(10000, p->output_rate * 0.45) * FFT_SIZE / p->output_rate);
    float total = 0, peak = 0; uint32_t bin = lo;
    for (uint32_t i = lo; i <= hi; i++) {
        float power = power_at(p, i); total += power;
        if (power > peak) { peak = power; bin = i; }
    }
    float narrow = 0;
    for (int i = -2; i <= 2; i++) narrow += power_at(p, bin + i);
    // A persistent near-sinusoid is suspicious in the speech profile.
    bool suspect = peak > 20 && total > 0 && narrow / total > 0.78;
    if (!suspect) { p->candidate_bin = -1; p->candidate_frames = 0; return; }
    if (p->candidate_bin < 0 || abs((int)bin - p->candidate_bin) > 2) {
        p->candidate_bin = (int)bin; p->candidate_frames = HOP; p->candidate_power = peak;
        return;
    }
    p->candidate_frames += HOP;
    double elapsed = p->candidate_frames / p->output_rate;
    bool growing = peak > p->candidate_power * 3.5;
    if (elapsed < 0.30 && !(growing && elapsed > 0.12)) return;
    double left = log(fmax(power_at(p, bin - 1), 1e-20));
    double middle = log(fmax(peak, 1e-20));
    double right = log(fmax(power_at(p, bin + 1), 1e-20));
    double divisor = left - 2 * middle + right;
    double delta = fabs(divisor) > 1e-12 ? 0.5 * (left - right) / divisor : 0;
    double hz = (bin + fmax(-0.5, fmin(0.5, delta))) * p->output_rate / FFT_SIZE;
    atomic_store(&p->feedback_hz, (float)hz);
    bool existing = false;
    for (uint32_t i = 0; i < p->active_notches; i++)
        if (fabs(p->notch_hz[i] - hz) < hz * 0.06) existing = true;
    if (existing || p->active_notches >= MAX_NOTCHES - 1) {
        atomic_store(&p->fault_reason, 1); atomic_store(&p->tripped, true);
    } else {
        configure_filter(&p->notches[p->active_notches], p->output_rate, hz, true);
        p->notch_hz[p->active_notches++] = (float)hz;
        p->safety_gain *= 0.50118723; // 10^(-3/20) = -3 dB
        atomic_store(&p->notch_count, p->active_notches);
        atomic_store(&p->safety_db, (float)(-20 * log10(p->safety_gain)));
    }
    p->candidate_bin = -1; p->candidate_frames = 0;
    // Discard the pre-notch window so it cannot immediately retrigger.
    p->analysis_frames = 0;
}

static float process_sample(SonoProcessor *p, float raw, bool muted, bool protection, double target_gain) {
    double x = isfinite(raw) ? fmax(-32, fmin(32, raw)) : 0;
    double y = filtered(&p->highpass, x);
    for (uint32_t n = 0; n < p->active_notches; n++) y = filtered(&p->notches[n], y);
    if (protection && !muted && !atomic_load_explicit(&p->tripped, memory_order_relaxed)) {
        p->analysis[p->analysis_index] = (float)y;
        p->analysis_index = (p->analysis_index + 1) % FFT_SIZE;
        if (p->analysis_frames < FFT_SIZE) p->analysis_frames++;
        if (++p->hop_count >= HOP) {
            p->hop_count = 0;
            if (p->analysis_frames == FFT_SIZE) detect_feedback(p);
        }
    } else { p->candidate_bin = -1; p->candidate_frames = 0; p->analysis_frames = 0; }
    if (muted || atomic_load_explicit(&p->tripped, memory_order_relaxed)) {
        p->smooth_gain = 0; return 0;
    }
    // 25 ms gain ramp. The emergency mute deliberately bypasses this ramp.
    p->smooth_gain += (target_gain - p->smooth_gain) * p->gain_ramp;
    y *= p->smooth_gain * p->safety_gain;
    double required = fabs(y) > CEILING ? CEILING / fabs(y) : 1;
    if (required < p->limiter_gain) p->limiter_gain = required;
    else p->limiter_gain += (1 - p->limiter_gain) * p->limiter_release;
    return (float)fmax(-CEILING, fmin(CEILING, y * p->limiter_gain));
}

void sono_process(SonoProcessor *p, const float *input, float *output, uint32_t frames) {
    if (!p || !input || !output) return;
    bool protection = atomic_load_explicit(&p->feedback_enabled, memory_order_relaxed);
    double target_gain = pow(10, atomic_load_explicit(&p->gain_db, memory_order_relaxed) / 20.0);
    float input_peak = 0, output_peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        bool muted = atomic_load_explicit(&p->muted, memory_order_relaxed);
        if (isfinite(input[i])) input_peak = fmaxf(input_peak, fabsf(input[i]));
        output[i] = process_sample(p, input[i], muted, protection, target_gain);
        output_peak = fmaxf(output_peak, fabsf(output[i]));
    }
    atomic_store(&p->input_peak, input_peak); atomic_store(&p->output_peak, output_peak);
    atomic_store(&p->limiter_db, (float)(-20 * log10(fmax(1e-12, p->limiter_gain))));
}

void sono_push(SonoProcessor *p, const float *samples, uint32_t frames) {
    if (!p || !samples) return;
    uint64_t w = atomic_load_explicit(&p->write_index, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&p->read_index, memory_order_acquire);
    if (frames > RING_SIZE || w - r + frames > RING_SIZE) {
        atomic_fetch_add(&p->overruns, 1);
        atomic_store(&p->fault_reason, 2); atomic_store(&p->tripped, true);
        return; // Never overwrite samples the consumer is reading.
    }
    for (uint32_t i = 0; i < frames; i++) {
        double sample = isfinite(samples[i]) ? fmax(-32, fmin(32, samples[i])) : 0;
        if (p->downsampling)
            for (uint32_t filter = 0; filter < 2; filter++) sample = filtered(&p->resample_filters[filter], sample);
        p->ring[(w + i) & (RING_SIZE - 1)] = (float)sample;
    }
    atomic_store_explicit(&p->write_index, w + frames, memory_order_release);
}

void sono_render(SonoProcessor *p, float *output, uint32_t frames) {
    if (!p || !output) return;
    uint64_t r = atomic_load_explicit(&p->read_index, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&p->write_index, memory_order_acquire);
    uint64_t available = w - r;
    if (!p->primed) {
        if (available < p->target_frames + 2) { memset(output, 0, frames * sizeof(float)); return; }
        p->primed = true;
    }
    // Slow, bounded clock drift compensation between independent devices.
    double expected_consumption = frames * p->input_rate / p->output_rate;
    double drift = fmax(-0.003, fmin(0.003,
        ((double)available - expected_consumption - p->target_frames) / p->input_rate * 0.05));
    double step = p->input_rate / p->output_rate * (1 + drift);
    double target_gain = pow(10, atomic_load(&p->gain_db) / 20.0);
    bool protection = atomic_load(&p->feedback_enabled), starved = false;
    float input_peak = 0, output_peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        uint64_t offset = (uint64_t)p->position;
        if (offset + 1 >= available) {
            output[i] = 0; starved = true; p->smooth_gain = 0; continue;
        }
        double fraction = p->position - offset;
        float a = p->ring[(r + offset) & (RING_SIZE - 1)];
        float b = p->ring[(r + offset + 1) & (RING_SIZE - 1)];
        float x = (float)(a + (b - a) * fraction);
        if (isfinite(x)) input_peak = fmaxf(input_peak, fabsf(x));
        output[i] = process_sample(p, x, atomic_load(&p->muted), protection, target_gain);
        output_peak = fmaxf(output_peak, fabsf(output[i]));
        p->position += step;
    }
    uint64_t consumed = (uint64_t)p->position;
    if (consumed > available) consumed = available;
    p->position -= consumed;
    atomic_store_explicit(&p->read_index, r + consumed, memory_order_release);
    if (starved) {
        atomic_fetch_add(&p->underruns, 1); p->primed = false;
        if (++p->consecutive_underruns >= 5) {
            atomic_store(&p->fault_reason, 3); atomic_store(&p->tripped, true);
        }
    } else p->consecutive_underruns = 0;
    atomic_store(&p->input_peak, input_peak); atomic_store(&p->output_peak, output_peak);
    atomic_store(&p->limiter_db, (float)(-20 * log10(fmax(1e-12, p->limiter_gain))));
}

SonoMetrics sono_metrics(SonoProcessor *p) {
    if (!p) return (SonoMetrics){0};
    uint64_t r = atomic_load(&p->read_index), w = atomic_load(&p->write_index);
    return (SonoMetrics){ .input_peak = atomic_load(&p->input_peak), .output_peak = atomic_load(&p->output_peak),
        .limiter_reduction_db = atomic_load(&p->limiter_db), .safety_reduction_db = atomic_load(&p->safety_db),
        .feedback_hz = atomic_load(&p->feedback_hz), .notch_count = atomic_load(&p->notch_count),
        .buffer_ms = (float)((w >= r ? w - r : 0) * 1000 / p->input_rate),
        .tripped = atomic_load(&p->tripped), .fault_reason = atomic_load(&p->fault_reason),
        .underruns = atomic_load(&p->underruns), .overruns = atomic_load(&p->overruns) };
}
