#include "fm_radio.h"
#include <math.h>

// Forward declarations (avoid implicit declaration / static conflict)
static void  biquad_lowpass(fm_radio_t *r, float fs, float fc, float Q);
static inline float biquad_process(fm_radio_t *r, float x);
static inline float dc_block_process(fm_radio_t *r, float x);

void fm_radio_init(fm_radio_t *radio, double fs, int audio_fs, int deemph_us) {
    radio->prev_sample = 1.0 + 0.0*I;
    radio->audio_acc = 0;
    radio->samples_in_acc = 0;
    radio->deemph_acc = 0;
    radio->gain = 60000.0;

    radio->decim_factor = (int)llround(fs / (double)audio_fs);
    if (radio->decim_factor < 1) radio->decim_factor = 1;

    float tau = (float)deemph_us * 1e-6f;
    float dt  = 1.0f / (float)audio_fs;
    radio->deemph_alpha = dt / (tau + dt);

    // Enable filters
    radio->enable_dc_block = 1;
    radio->enable_lpf = 1;

    // DC blocker (~30 Hz @ 48 kHz is around 0.996)
    radio->dc_r  = 0.996f;
    radio->dc_x1 = 0.0f;
    radio->dc_y1 = 0.0f;

    // Audio low-pass biquad:
    // - Voice:  4–6 kHz
    // - WBFM:  12–15 kHz (use 12 kHz as conservative default)
    biquad_lowpass(radio, (float)audio_fs, 12000.0f, 0.707f);
}

static void biquad_lowpass(fm_radio_t *r, float fs, float fc, float Q) {
    if (fc <= 0.0f) fc = 1.0f;
    if (fc > 0.49f * fs) fc = 0.49f * fs;

    const float w0 = 2.0f * (float)M_PI * (fc / fs);
    const float c  = cosf(w0);
    const float s  = sinf(w0);
    const float alpha = s / (2.0f * Q);

    float b0 = (1.0f - c) * 0.5f;
    float b1 = (1.0f - c);
    float b2 = (1.0f - c) * 0.5f;
    float a0 = (1.0f + alpha);
    float a1 = (-2.0f * c);
    float a2 = (1.0f - alpha);

    // normalize (a0 -> 1)
    r->b0 = b0 / a0;
    r->b1 = b1 / a0;
    r->b2 = b2 / a0;
    r->a1 = a1 / a0;
    r->a2 = a2 / a0;

    r->z1 = 0.0f;
    r->z2 = 0.0f;
}

static inline float biquad_process(fm_radio_t *r, float x) {
    // Direct Form II transposed
    float y = r->b0 * x + r->z1;
    r->z1 = r->b1 * x - r->a1 * y + r->z2;
    r->z2 = r->b2 * x - r->a2 * y;
    return y;
}

static inline float dc_block_process(fm_radio_t *r, float x) {
    // y[n] = x[n] - x[n-1] + R*y[n-1]
    float y = x - r->dc_x1 + r->dc_r * r->dc_y1;
    r->dc_x1 = x;
    r->dc_y1 = y;
    return y;
}

int fm_radio_iq_to_pcm(fm_radio_t *radio, signal_iq_t *sig, int16_t *pcm_out) {
    int out_idx = 0;

    for (size_t i = 0; i < sig->n_signal; i++) {
        // 1) FM demod: phase difference
        double complex diff = sig->signal_iq[i] * conj(radio->prev_sample);
        double angle = atan2(cimag(diff), creal(diff));
        radio->prev_sample = sig->signal_iq[i];

        // 2) crude decimation: accumulate then average
        radio->audio_acc += angle;
        radio->samples_in_acc++;

        if (radio->samples_in_acc >= radio->decim_factor) {
            float val = (float)(radio->audio_acc / (double)radio->samples_in_acc);
            radio->audio_acc = 0;
            radio->samples_in_acc = 0;

            // 3) de-emphasis
            radio->deemph_acc += radio->deemph_alpha * (val - radio->deemph_acc);
            float a = radio->deemph_acc;

            // 3b) DC blocker
            if (radio->enable_dc_block) {
                a = dc_block_process(radio, a);
            }

            // 3c) audio low-pass
            if (radio->enable_lpf) {
                a = biquad_process(radio, a);
            }

            // 4) gain + clip (NOTE: use 'a', not deemph_acc)
            double pcm = (double)a * (double)radio->gain;
            if (pcm >  32767.0) pcm =  32767.0;
            if (pcm < -32768.0) pcm = -32768.0;

            pcm_out[out_idx++] = (int16_t)pcm;
        }
    }

    return out_idx;
}

