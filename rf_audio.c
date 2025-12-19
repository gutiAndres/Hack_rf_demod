// rf.c  -- updated: stream audio as Opus frames over TCP to a Python WS gateway
//          (replaces local aplay playback)
//
// Build notes (example):
//   gcc -O2 -pthread rf.c opus_tx.c ... -lhackrf -lzmq -lcjson -lopus -lm -o rf
//
// Runtime:
//   1) Start your Python gateway listening TCP_PORT (default 9000)
//   2) Run this RF engine; it will connect to 127.0.0.1:9000 and stream Opus frames

#define _GNU_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <complex.h>
#include <sys/time.h>
#include <errno.h>

#include <libhackrf/hackrf.h>
#include <cjson/cJSON.h>

// Project Includes
#include "psd.h"
#include "datatypes.h"
#include "sdr_HAL.h"
#include "ring_buffer.h"
#include "zmq_util.h"
#include "utils.h"
#include "fm_radio.h"

// NEW: Opus TX (TCP framing matches your Python gateway: !IIIHH, magic 'OPU0')
#include "opus_tx.h"


// ========================= Audio & PSD constants
#define AUDIO_CHUNK_SAMPLES 16384
#define PSD_SAMPLES_TOTAL   2097152
#define AUDIO_FS            48000   // IMPORTANT: must be 48k to match Opus best-practice

// ========================= Opus streaming defaults (to Python gateway)
#define AUDIO_TCP_DEFAULT_HOST "127.0.0.1"
#define AUDIO_TCP_DEFAULT_PORT 9000

#define OPUS_FRAME_MS_DEFAULT  20
#define OPUS_BITRATE_DEFAULT   32000
#define OPUS_COMPLEXITY_DEFAULT 5
#define OPUS_VBR_DEFAULT       0    // 0 = CBR, 1 = VBR

// =========================================================
// GLOBALS
zpair_t *zmq_channel = NULL;
hackrf_device* device = NULL;

// Two ring buffers:
//   rb         = large buffer used for acquisition/full-PSD (main thread reads)
//   audio_rb   = small buffer used only by audio thread (audio thread reads)
ring_buffer_t rb;
ring_buffer_t audio_rb;

volatile bool config_received = false;

DesiredCfg_t desired_config = {0};
PsdConfig_t psd_cfg = {0};
SDR_cfg_t hack_cfg = {0};
RB_cfg_t rb_cfg = {0};

// Audio thread control
pthread_t audio_thread;
volatile bool audio_thread_running = false;

// Track whether RX is currently running and last applied config
static bool rx_running = false;
static SDR_cfg_t last_applied_cfg;
static bool last_cfg_valid = false;

// Forward decls
void publish_results(double*, double*, int, SDR_cfg_t*);
void on_command_received(const char *payload);

// =========================================================
// HELPERS
static inline uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (tv.tv_usec / 1000ULL);
}

static inline void msleep_int(int ms) {
    if (ms <= 0) return;
    usleep((useconds_t)ms * 1000);
}

/** Compare relevant fields of SDR config to decide if reconfig is required */
static bool sdr_cfg_equal(const SDR_cfg_t *a, const SDR_cfg_t *b) {
    if (!a || !b) return false;
    if (a->center_freq != b->center_freq) return false;
    if (a->lna_gain != b->lna_gain) return false;
    if (a->vga_gain != b->vga_gain) return false;
    if (a->amp_enabled != b->amp_enabled) return false;
    if (a->ppm_error != b->ppm_error) return false;
    /* compare sample rate with small tolerance */
    if (fabs(a->sample_rate - b->sample_rate) > 1e-6) return false;
    return true;
}

// =========================================================
// RX CALLBACK (duplicate incoming bytes to both ring buffers)
int rx_callback(hackrf_transfer* transfer) {
    if (transfer->valid_length > 0) {
        rb_write(&rb, transfer->buffer, transfer->valid_length);
        rb_write(&audio_rb, transfer->buffer, transfer->valid_length);
    }
    return 0;
}

int recover_hackrf(void) {
    printf("\n[RECOVERY] Initiating Hardware Reset sequence...\n");
    if (device != NULL) {
        if (rx_running) {
            hackrf_stop_rx(device);
            rx_running = false;
        }
        hackrf_close(device);
        device = NULL;
    }

    int attempts = 0;
    while (attempts < 3) {
        usleep(500000);
        int status = hackrf_open(&device);
        if (status == HACKRF_SUCCESS) {
            printf("[RECOVERY] Device Re-opened successfully.\n");
            return 0;
        }
        attempts++;
        fprintf(stderr, "[RECOVERY] Attempt %d failed.\n", attempts);
    }
    return -1;
}

// =========================================================
// PUBLISH (unchanged)
void publish_results(double* freq_array, double* psd_array, int length, SDR_cfg_t *local_hack) {
    if (!zmq_channel || !freq_array || !psd_array || length <= 0) return;
    cJSON *root = cJSON_CreateObject();
    double start_abs = freq_array[0] + (double)local_hack->center_freq;
    double end_abs   = freq_array[length-1] + (double)local_hack->center_freq;
    cJSON_AddNumberToObject(root, "start_freq_hz", start_abs);
    cJSON_AddNumberToObject(root, "end_freq_hz", end_abs);
    cJSON *pxx_array = cJSON_CreateDoubleArray(psd_array, length);
    cJSON_AddItemToObject(root, "Pxx", pxx_array);
    char *json_string = cJSON_PrintUnformatted(root);
    zpair_send(zmq_channel, json_string);
    free(json_string);
    cJSON_Delete(root);
}

// =========================================================
// ZMQ CALLBACK (unchanged)
void on_command_received(const char *payload) {
    printf("\n>>> [RF] Received Command Payload.\n");
    memset(&desired_config, 0, sizeof(DesiredCfg_t));
    if (parse_config_rf(payload, &desired_config) == 0) {
        find_params_psd(desired_config, &hack_cfg, &psd_cfg, &rb_cfg);
        print_config_summary(&desired_config, &hack_cfg, &psd_cfg, &rb_cfg);
        config_received = true;
    } else {
        fprintf(stderr, ">>> [PARSER] Failed to parse JSON configuration.\n");
    }
}

// =========================================================
// AUDIO STREAMING CONTEXT
typedef struct {
    fm_radio_t *radio;

    // TCP destination for opus_tx (Python gateway listener)
    const char *tcp_host;
    int tcp_port;

    // Opus parameters
    int opus_sample_rate;   // must be 8000/12000/16000/24000/48000 (we use 48000)
    int opus_channels;      // 1
    int bitrate;            // e.g., 32000
    int complexity;         // 0..10
    int vbr;                // 0/1
    int frame_ms;           // 20ms is typical
} audio_stream_ctx_t;

static void audio_stream_ctx_defaults(audio_stream_ctx_t *ctx, fm_radio_t *radio) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->radio = radio;

    // allow overrides via env for convenience
    const char *env_host = getenv("AUDIO_TCP_HOST");
    const char *env_port = getenv("AUDIO_TCP_PORT");
    const char *env_br   = getenv("OPUS_BITRATE");
    const char *env_cplx = getenv("OPUS_COMPLEXITY");
    const char *env_vbr  = getenv("OPUS_VBR");
    const char *env_fms  = getenv("OPUS_FRAME_MS");

    ctx->tcp_host = (env_host && env_host[0]) ? env_host : AUDIO_TCP_DEFAULT_HOST;

    ctx->tcp_port = AUDIO_TCP_DEFAULT_PORT;
    if (env_port && env_port[0]) {
        int p = atoi(env_port);
        if (p > 0 && p < 65536) ctx->tcp_port = p;
    }

    ctx->opus_sample_rate = AUDIO_FS;
    ctx->opus_channels    = 1;

    ctx->bitrate    = (env_br && env_br[0]) ? atoi(env_br) : OPUS_BITRATE_DEFAULT;
    ctx->complexity = (env_cplx && env_cplx[0]) ? atoi(env_cplx) : OPUS_COMPLEXITY_DEFAULT;
    ctx->vbr        = (env_vbr && env_vbr[0]) ? atoi(env_vbr) : OPUS_VBR_DEFAULT;
    ctx->frame_ms   = (env_fms && env_fms[0]) ? atoi(env_fms) : OPUS_FRAME_MS_DEFAULT;

    if (ctx->complexity < 0) ctx->complexity = 0;
    if (ctx->complexity > 10) ctx->complexity = 10;
    if (ctx->frame_ms <= 0) ctx->frame_ms = OPUS_FRAME_MS_DEFAULT;
    if (ctx->bitrate <= 0) ctx->bitrate = OPUS_BITRATE_DEFAULT;
    ctx->vbr = ctx->vbr ? 1 : 0;
}

// =========================================================
// AUDIO THREAD: drains audio_rb, converts IQ->PCM, encodes Opus, sends via TCP
void* audio_thread_fn(void* arg) {
    audio_stream_ctx_t *ctx = (audio_stream_ctx_t*)arg;
    if (!ctx || !ctx->radio) {
        fprintf(stderr, "[AUDIO] FATAL: ctx or radio is NULL\n");
        return NULL;
    }

    // sanity: Opus expects one of the standard rates; we use 48000
    if (!(ctx->opus_sample_rate == 8000  || ctx->opus_sample_rate == 12000 ||
          ctx->opus_sample_rate == 16000 || ctx->opus_sample_rate == 24000 ||
          ctx->opus_sample_rate == 48000)) {
        fprintf(stderr, "[AUDIO] FATAL: invalid opus_sample_rate=%d\n", ctx->opus_sample_rate);
        return NULL;
    }

    const int frame_samples = (ctx->opus_sample_rate * ctx->frame_ms) / 1000; // e.g., 960 @48k/20ms
    if (frame_samples <= 0) {
        fprintf(stderr, "[AUDIO] FATAL: invalid frame_samples\n");
        return NULL;
    }

    int8_t  *raw_iq_chunk = (int8_t*)malloc((size_t)AUDIO_CHUNK_SAMPLES * 2);
    int16_t *pcm_out      = (int16_t*)malloc((size_t)AUDIO_CHUNK_SAMPLES * sizeof(int16_t));

    signal_iq_t audio_sig;
    audio_sig.n_signal = AUDIO_CHUNK_SAMPLES;
    audio_sig.signal_iq = (double complex*)malloc((size_t)AUDIO_CHUNK_SAMPLES * sizeof(double complex));

    int16_t *pcm_accum = (int16_t*)malloc((size_t)frame_samples * sizeof(int16_t));
    int accum_len = 0;

    if (!raw_iq_chunk || !pcm_out || !audio_sig.signal_iq || !pcm_accum) {
        fprintf(stderr, "[AUDIO] FATAL: malloc failed\n");
        free(raw_iq_chunk);
        free(pcm_out);
        free(audio_sig.signal_iq);
        free(pcm_accum);
        return NULL;
    }

    opus_tx_t *tx = NULL;

    // local helper: (re)connect opus tx
    auto int ensure_tx(void) {
        if (tx) return 0;

        opus_tx_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate = ctx->opus_sample_rate;
        cfg.channels    = ctx->opus_channels;
        cfg.bitrate     = ctx->bitrate;
        cfg.complexity  = ctx->complexity;
        cfg.vbr         = ctx->vbr;

        tx = opus_tx_create(ctx->tcp_host, ctx->tcp_port, &cfg);
        if (!tx) {
            fprintf(stderr,
                    "[AUDIO] WARN: opus_tx_create failed (%s:%d). Will retry.\n",
                    ctx->tcp_host, ctx->tcp_port);
            return -1;
        }

        fprintf(stderr,
                "[AUDIO] Connected Opus TX to %s:%d (sr=%d ch=%d frame_ms=%d bitrate=%d vbr=%d cplx=%d)\n",
                ctx->tcp_host, ctx->tcp_port,
                cfg.sample_rate, cfg.channels, ctx->frame_ms, cfg.bitrate, cfg.vbr, cfg.complexity);

        return 0;
    }

    audio_thread_running = true;

    while (audio_thread_running) {

        // Wait for enough IQ bytes
        if (rb_available(&audio_rb) < (size_t)(AUDIO_CHUNK_SAMPLES * 2)) {
            usleep(1000);
            continue;
        }

        // Drain one chunk
        rb_read(&audio_rb, raw_iq_chunk, AUDIO_CHUNK_SAMPLES * 2);

        // Convert int8 IQ -> complex double
        for (int i = 0; i < AUDIO_CHUNK_SAMPLES; ++i) {
            double real = ((double)raw_iq_chunk[2*i]) / 128.0;
            double imag = ((double)raw_iq_chunk[2*i + 1]) / 128.0;
            audio_sig.signal_iq[i] = real + imag * I;
        }

        // IQ -> PCM (output at AUDIO_FS)
        int samples_gen = fm_radio_iq_to_pcm(ctx->radio, &audio_sig, pcm_out);
        if (samples_gen <= 0) continue;

        // Ensure TCP/Opus encoder is ready
        if (ensure_tx() != 0) {
            // Drop audio while reconnecting (keeps draining to avoid backlog)
            msleep_int(200);
            continue;
        }

        // Accumulate into exact Opus frames
        int idx = 0;
        while (idx < samples_gen) {
            int space = frame_samples - accum_len;
            int take  = samples_gen - idx;
            if (take > space) take = space;

            memcpy(&pcm_accum[accum_len], &pcm_out[idx], (size_t)take * sizeof(int16_t));
            accum_len += take;
            idx += take;

            if (accum_len == frame_samples) {
                if (opus_tx_send_frame(tx, pcm_accum, frame_samples) != 0) {
                    fprintf(stderr, "[AUDIO] WARN: opus_tx_send_frame failed. Reconnecting...\n");
                    opus_tx_destroy(tx);
                    tx = NULL;
                    accum_len = 0; // drop partial frame for simplicity
                    msleep_int(200);
                    break; // exit inner loop; next outer iteration will reconnect
                }
                accum_len = 0;
            }
        }
    }

    if (tx) opus_tx_destroy(tx);
    free(raw_iq_chunk);
    free(pcm_out);
    free(audio_sig.signal_iq);
    free(pcm_accum);
    return NULL;
}

// =========================================================
// MAIN
int main() {
    char *raw_verbose = getenv_c("VERBOSE");
    bool verbose_mode = (raw_verbose != NULL && strcmp(raw_verbose, "true") == 0);
    if (raw_verbose) free(raw_verbose);

    char *ipc_addr = getenv_c("IPC_ADDR");
    if (!ipc_addr) ipc_addr = strdup("ipc:///tmp/rf_engine");

    printf("[RF] Starting. IPC=%s, VERBOSE=%d\n", ipc_addr, verbose_mode);

    zmq_channel = zpair_init(ipc_addr, on_command_received, verbose_mode ? 1 : 0);
    if (!zmq_channel) {
        fprintf(stderr, "[RF] FATAL: Failed to initialize ZMQ at %s\n", ipc_addr);
        if (ipc_addr) free(ipc_addr);
        return 1;
    }
    zpair_start(zmq_channel);

    // Init HackRF
    printf("[RF] Initializing HackRF Library...\n");
    while (hackrf_init() != HACKRF_SUCCESS) {
        fprintf(stderr, "[RF] Error: HackRF Init failed. Retrying in 5s...\n");
        sleep(5);
    }
    printf("[RF] HackRF Library Initialized.\n");

    // Open device
    while (hackrf_open(&device) != HACKRF_SUCCESS) {
        fprintf(stderr, "[RF] Warning: Initial Open failed. Retrying in 5s...\n");
        sleep(5);
    }
    printf("[RF] HackRF Device Opened.\n");

    // Initialize BOTH ring buffers
    size_t FIXED_BUFFER_SIZE = 100 * 1024 * 1024;
    rb_init(&rb, FIXED_BUFFER_SIZE);
    size_t AUDIO_BUFFER_SIZE = AUDIO_CHUNK_SAMPLES * 2 * 8;
    rb_init(&audio_rb, AUDIO_BUFFER_SIZE);

    printf("[RF] Ring Buffers: big=%zu MB, audio=%zu KB\n",
           FIXED_BUFFER_SIZE / (1024*1024), AUDIO_BUFFER_SIZE / 1024);

    bool needs_recovery = false;

    // Local copies
    SDR_cfg_t local_hack_cfg;
    RB_cfg_t local_rb_cfg;
    PsdConfig_t local_psd_cfg;
    DesiredCfg_t local_desired_cfg;

    int8_t *linear_buffer = NULL;
    double *f_axis = NULL;
    double *p_vals = NULL;

    // audio resources
    fm_radio_t *radio_ptr = (fm_radio_t*)malloc(sizeof(fm_radio_t));
    if (!radio_ptr) {
        fprintf(stderr, "[RF] FATAL: malloc radio_ptr failed\n");
        return 1;
    }
    memset(radio_ptr, 0, sizeof(fm_radio_t));

    bool audio_thread_created = false;
    double last_radio_sample_rate = 0.0;

    // NEW: audio streaming context
    audio_stream_ctx_t audio_ctx;
    audio_stream_ctx_defaults(&audio_ctx, radio_ptr);

    fprintf(stderr, "[AUDIO] Stream target TCP %s:%d (Opus sr=%d ch=%d frame_ms=%d bitrate=%d)\n",
            audio_ctx.tcp_host, audio_ctx.tcp_port,
            audio_ctx.opus_sample_rate, audio_ctx.opus_channels,
            audio_ctx.frame_ms, audio_ctx.bitrate);

    while (1) {
        if (!config_received) {
            usleep(50000);
            continue;
        }

        if (device == NULL) { needs_recovery = true; goto error_handler; }

        /* Snapshot global config structs (atomically used below) */
        memcpy(&local_hack_cfg, &hack_cfg, sizeof(SDR_cfg_t));
        memcpy(&local_rb_cfg, &rb_cfg, sizeof(RB_cfg_t));
        memcpy(&local_psd_cfg, &psd_cfg, sizeof(PsdConfig_t));
        memcpy(&local_desired_cfg, &desired_config, sizeof(DesiredCfg_t));
        config_received = false;

        if (local_rb_cfg.total_bytes > rb.size) {
            printf("[RF] Error: Request bytes (%zu) exceeds buffer size!\n", local_rb_cfg.total_bytes);
            continue;
        }

        /* re-alloc PSD arrays */
        if (f_axis) free(f_axis);
        if (p_vals) free(p_vals);
        f_axis = (double*)malloc((size_t)local_psd_cfg.nperseg * sizeof(double));
        p_vals = (double*)malloc((size_t)local_psd_cfg.nperseg * sizeof(double));

        // If RX not running yet -> apply cfg and start RX
        if (!rx_running) {
            hackrf_apply_cfg(device, &local_hack_cfg);
            if (hackrf_start_rx(device, rx_callback, NULL) != HACKRF_SUCCESS) {
                fprintf(stderr, "[RF] Error: hackrf_start_rx failed on initial start.\n");
                needs_recovery = true; goto error_handler;
            }
            rx_running = true;
            last_applied_cfg = local_hack_cfg;
            last_cfg_valid = true;
        } else {
            // If RX running and config differs from last applied -> apply new cfg (but do not restart RX)
            if (!last_cfg_valid || !sdr_cfg_equal(&local_hack_cfg, &last_applied_cfg)) {
                printf("[RF] New SDR config differs from last - applying.\n");
                hackrf_apply_cfg(device, &local_hack_cfg);
                last_applied_cfg = local_hack_cfg;
                last_cfg_valid = true;
            } else {
                // identical config -> skip hackrf_apply_cfg() to avoid interruption
            }
        }

        // Initialize or re-init FM radio only if sample_rate changed
        if (!audio_thread_created || fabs(last_radio_sample_rate - local_hack_cfg.sample_rate) > 1e-6) {
            // IMPORTANT: output rate must match audio_ctx.opus_sample_rate (typically 48000)
            fm_radio_init(radio_ptr, local_hack_cfg.sample_rate, audio_ctx.opus_sample_rate, 75);
            last_radio_sample_rate = local_hack_cfg.sample_rate;
        }

        // Start audio thread once (it will keep running and drain audio_rb)
        if (!audio_thread_created) {
            if (pthread_create(&audio_thread, NULL, audio_thread_fn, (void*)&audio_ctx) == 0) {
                audio_thread_created = true;
            } else {
                fprintf(stderr, "[RF] Warning: failed to create audio thread\n");
            }
        }

        // Wait until big buffer has filled (do NOT stop RX) - time-based timeout
        uint64_t start_ms = now_ms();
        const uint64_t timeout_ms = 5000;
        bool bigbuffer_full = false;

        while (now_ms() - start_ms < timeout_ms) {
            if (rb_available(&rb) >= local_rb_cfg.total_bytes) { bigbuffer_full = true; break; }
            usleep(5000);
        }

        if (!bigbuffer_full) {
            fprintf(stderr, "[RF] Error: Acquisition Timeout.\n");
            needs_recovery = true;
            goto error_handler;
        }

        // Read linear buffer for full-band PSD while RX remains running
        linear_buffer = (int8_t*)malloc(local_rb_cfg.total_bytes);
        if (linear_buffer) {
            rb_read(&rb, linear_buffer, local_rb_cfg.total_bytes);
            signal_iq_t *sig = load_iq_from_buffer(linear_buffer, local_rb_cfg.total_bytes);

            double *freq = (double*)malloc((size_t)local_psd_cfg.nperseg * sizeof(double));
            double *psd  = (double*)malloc((size_t)local_psd_cfg.nperseg * sizeof(double));

            if (sig && freq && psd) {
                execute_welch_psd(sig, &local_psd_cfg, freq, psd);
                scale_psd(psd, local_psd_cfg.nperseg, local_desired_cfg.scale);

                double half_span = local_desired_cfg.span / 2.0;
                int start_idx = 0, end_idx = local_psd_cfg.nperseg - 1;
                for (int i = 0; i < local_psd_cfg.nperseg; ++i) {
                    if (freq[i] >= -half_span) { start_idx = i; break; }
                }
                for (int i = start_idx; i < local_psd_cfg.nperseg; ++i) {
                    if (freq[i] > half_span) { end_idx = i - 1; break; }
                    end_idx = i;
                }
                int valid_len = end_idx - start_idx + 1;
                if (valid_len > 0) {
                    publish_results(&freq[start_idx], &psd[start_idx], valid_len, &local_hack_cfg);
                } else {
                    printf("[RF] Warning: Span resulted in 0 bins.\n");
                }
            }

            free(linear_buffer);
            if (freq) free(freq);
            if (psd) free(psd);
            free_signal_iq(sig);
        }

        continue;

error_handler:
        // Try to recover hardware
        if (rx_running && device) {
            hackrf_stop_rx(device);
            rx_running = false;
        }
        if (needs_recovery) {
            recover_hackrf();
            needs_recovery = false;
            last_cfg_valid = false; // force reapply on next good config
        }
    }

    // Cleanup (unreachable normally)
    audio_thread_running = false;
    if (audio_thread_created) pthread_join(audio_thread, NULL);
    if (radio_ptr) free(radio_ptr);
    if (f_axis) free(f_axis);
    if (p_vals) free(p_vals);
    zpair_close(zmq_channel);
    rb_free(&rb);
    rb_free(&audio_rb);
    if (ipc_addr) free(ipc_addr);
    return 0;
}
