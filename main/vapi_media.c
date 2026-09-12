/* Audio pipeline for the Vapi AtomS3R voice client.
 *
 * Capture:  ES8311 mic (2ch: mic + DAC loopback) → AEC → PCM 16 kHz mono
 * Playback: PCM 16 kHz mono → av_render → ES8311 speaker (2ch, see below)
 *
 * The whole path is raw PCM. Vapi's `vapi.websocket` transport has no codec —
 * binary frames on the socket are s16le samples — so there is no Opus encoder
 * or decoder anywhere in this firmware. That keeps esp_audio_enc/dec out of the
 * hot path entirely; esp_capture notices the source and sink are both PCM and
 * bypasses its encoder stage.
 *
 * Derived from the media_sys.c example in Espressif's esp-webrtc-solution
 * (Public Domain / CC0).
 */

#include "codec_init.h"
#include "codec_board.h"

#include "esp_capture_path_simple.h"
#include "esp_capture_audio_enc.h"
#include "av_render.h"
#include "common.h"
#include "settings.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_log.h"
#include <math.h>

#define RET_ON_NULL(ptr, v) do {                                \
    if (ptr == NULL) {                                          \
        ESP_LOGE(TAG, "Memory allocate fail on %d", __LINE__);  \
        return v;                                               \
    }                                                           \
} while (0)

#define TAG "VAPI_MEDIA"

typedef struct {
    esp_capture_path_handle_t   capture_handle;
    esp_capture_path_handle_t   sink_path;      /* primary sink, valid while capturing */
    esp_capture_aenc_if_t      *aud_enc;
    esp_capture_audio_src_if_t *aud_src;
    esp_capture_path_if_t      *path_if;
    bool                        capturing;
} capture_system_t;

typedef struct {
    audio_render_handle_t audio_render;
    av_render_handle_t    player;
    bool                  stream_open;
    uint32_t              pts_ms;   /* monotonic playback timestamp */
} player_system_t;

static capture_system_t capture_sys;
static player_system_t  player_sys;

static int build_capture_system(void)
{
    /* Still registered even though nothing encodes: esp_capture wants an
     * encoder interface present before it will agree to bypass it. */
    capture_sys.aud_enc = esp_capture_new_audio_encoder();
    RET_ON_NULL(capture_sys.aud_enc, -1);
#if VAPI_ENABLE_AEC
    /* ES8311 in stereo: left slot = mic, right slot = DAC loopback, which the
     * AEC uses as its echo reference. See VAPI_AEC_CHANNEL in settings.h.
     * Note the AEC alone does not stop echo on this board — see the echo gate
     * below and docs/AEC-TUNING.md. */
    esp_capture_audio_aec_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
        .channel       = VAPI_AEC_CHANNEL,
        .channel_mask  = VAPI_AEC_CHANNEL_MASK,
    };
    capture_sys.aud_src = esp_capture_new_audio_aec_src(&codec_cfg);
#else
    /* No AEC: raw mono mic. Fine for push-to-talk, echoes badly on open mic. */
    esp_capture_audio_codec_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
    };
    capture_sys.aud_src = esp_capture_new_audio_codec_src(&codec_cfg);
#endif
    RET_ON_NULL(capture_sys.aud_src, -1);
    esp_capture_simple_path_cfg_t simple_cfg = {
        .aenc = capture_sys.aud_enc,
    };
    capture_sys.path_if = esp_capture_build_simple_path(&simple_cfg);
    RET_ON_NULL(capture_sys.path_if, -1);
    esp_capture_cfg_t cfg = {
        .sync_mode    = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src    = capture_sys.aud_src,
        .capture_path = capture_sys.path_if,
    };
    esp_capture_open(&cfg, &capture_sys.capture_handle);
    return 0;
}

static int build_player_system(void)
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = get_playback_handle(),
    };
    player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
    if (player_sys.audio_render == NULL) {
        ESP_LOGE(TAG, "Fail to create audio render");
        return -1;
    }
    esp_codec_dev_set_out_vol(i2s_cfg.play_handle, DEFAULT_PLAYBACK_VOL);

    /* The render FIFO is this firmware's jitter buffer, and it needs to be
     * generous. Vapi does not pace its websocket sends evenly: measured p95 gap
     * between assistant audio chunks is ~51 ms against a 20 ms frame period, so
     * a shallow buffer underruns audibly on every burst. 100 KB at 16 kHz/2ch/
     * 16-bit is ~1.5 s of slack, which rides through that comfortably.
     * (Measured against the same transport from a desktop client.) */
    av_render_cfg_t render_cfg = {
        .audio_render           = player_sys.audio_render,
        .audio_raw_fifo_size    = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data        = false,
    };
    player_sys.player = av_render_open(&render_cfg);
    if (player_sys.player == NULL) {
        ESP_LOGE(TAG, "Fail to create player");
        return -1;
    }
    return 0;
}

int vapi_media_buildup(void)
{
    /* Registered for the beep/loopback paths and so esp_capture has an encoder
     * to decline; no codec runs during a call. */
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    build_capture_system();
    build_player_system();
    return 0;
}

/* --- Capture -------------------------------------------------------------- */

/* Makeup gain as a Q8 multiplier, so the per-sample path is an integer multiply
 * and a shift rather than a float op on every one of 16000 samples a second. */
static int32_t s_dgain_q8 = 256;   /* 0 dB until set */
static uint32_t s_clip_count = 0;

/* --- Far-end gate ---------------------------------------------------------
 * s_far_until_ms is "the wall-clock time until which the assistant is still
 * coming out of the speaker". Written by the websocket task in write_frame,
 * read by the mic pump in read_frame. A stale read is harmless: worst case the
 * gate opens or shuts one 20 ms frame late.
 */
static volatile uint32_t s_far_until_ms = 0;
/* Write-ahead playout clock: the wall-clock time at which the audio handed to
 * av_render so far will have finished coming out of the speaker.
 *
 * This replaces av_render_get_audio_fifo_level(), which was measured returning
 * 0 ms on every single call — so the gate had no delay compensation at all and
 * was muting the mic while the *previous* phrase played, then opening again
 * mid-sentence. Telemetry showed it shut anywhere between 12% and 99% of frames
 * with no relation to whether the assistant was actually audible.
 *
 * Bytes written and the sample rate are enough to know this exactly: audio
 * queued now starts playing when everything already queued has drained. The one
 * assumption is that playback does not underrun, which holds because Vapi
 * streams continuously at 1x. */
static volatile uint32_t s_playout_end_ms = 0;
/* Gate telemetry: without this the gate is a black box — "still echoing" could
 * mean it never shuts, shuts at the wrong time, or does not attenuate enough,
 * and those need completely different fixes. */
static uint32_t s_g_frames = 0, s_g_shut = 0, s_g_far_hits = 0;
static uint32_t s_g_lead_ms = 0;
static uint32_t s_g_in_level = 0, s_g_out_level = 0, s_g_report_ms = 0;
static int32_t  s_gate_atten_q8 = 0;   /* 0 = gate disabled */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int vapi_media_set_echo_gate(float atten_db)
{
    if (atten_db <= 0.0f) {
        s_gate_atten_q8 = -1;   /* -1 = explicitly off; 0 = full mute */
        ESP_LOGI(TAG, "echo gate: off (barge-in possible, echo likely)");
        return 0;
    }
    if (atten_db >= 60.0f) {
        /* Full mute. Zeroing beats attenuating: -40 dB of a peak-20000 echo is
         * still ~200, which is inside the range Vapi's STT will transcribe. */
        s_gate_atten_q8 = 0;
        ESP_LOGI(TAG, "echo gate: hard mute while the assistant is audible");
        return 0;
    }
    s_gate_atten_q8 = (int32_t)(powf(10.0f, -atten_db / 20.0f) * 256.0f + 0.5f);
    if (s_gate_atten_q8 < 1) {
        s_gate_atten_q8 = 1;
    }
    ESP_LOGI(TAG, "echo gate: -%.0f dB while the assistant is audible", atten_db);
    return 0;
}

/* Mean |sample| is close enough to RMS for a speech/silence decision and costs
 * no multiply per sample on the websocket task's hot path. */
static uint32_t mean_abs(const int16_t *s, int count)
{
    if (count <= 0) {
        return 0;
    }
    uint64_t acc = 0;
    for (int i = 0; i < count; i++) {
        acc += (uint32_t)(s[i] < 0 ? -s[i] : s[i]);
    }
    return (uint32_t)(acc / count);
}

int vapi_media_set_digital_gain(float db)
{
    if (db < 0.0f || db > 40.0f) {
        ESP_LOGE(TAG, "digital gain %.1f dB out of range (0..40)", db);
        return -1;
    }
    s_dgain_q8 = (int32_t)(powf(10.0f, db / 20.0f) * 256.0f + 0.5f);
    s_clip_count = 0;
    ESP_LOGI(TAG, "digital makeup gain: %.1f dB (q8=%d)", db, (int)s_dgain_q8);
    return 0;
}

/* Apply makeup gain in place, saturating rather than wrapping. Wrapping would
 * turn a loud syllable into white noise; saturation just clips it. */
static void apply_digital_gain(int16_t *samples, int count)
{
    if (s_dgain_q8 == 256) {
        return;
    }
    uint32_t clipped = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = ((int32_t)samples[i] * s_dgain_q8) >> 8;
        if (v > 32767)  { v = 32767;  clipped++; }
        if (v < -32768) { v = -32768; clipped++; }
        samples[i] = (int16_t)v;
    }
    /* Persistent clipping means the gain is too high for how loudly this person
     * talks; report it rarely so it is visible without flooding the log. */
    if (clipped) {
        s_clip_count += clipped;
        if (s_clip_count > 16000) {   /* ~1 s worth of samples */
            ESP_LOGW(TAG, "digital gain clipping — lower it with `dgain`");
            s_clip_count = 0;
        }
    }
}

int vapi_media_set_mic_gain(float db)
{
    esp_codec_dev_handle_t rec = get_record_handle();
    if (rec == NULL) {
        return -1;
    }
    int ret = esp_codec_dev_set_in_gain(rec, db);
    if (ret != 0) {
        ESP_LOGE(TAG, "failed to set mic gain to %.1f dB (ret=%d)", db, ret);
        return -1;
    }
    ESP_LOGI(TAG, "mic gain: %.1f dB", db);
    return 0;
}

int vapi_media_capture_start(void)
{
    if (capture_sys.capturing) {
        return 0;
    }
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .codec           = ESP_CAPTURE_CODEC_TYPE_PCM,
            .sample_rate     = VAPI_SAMPLE_RATE,
            .channel         = VAPI_CHANNELS,
            .bits_per_sample = VAPI_BITS_PER_SAMP,
        },
    };
    int ret = esp_capture_setup_path(capture_sys.capture_handle,
                                     ESP_CAPTURE_PATH_PRIMARY, &sink_cfg,
                                     &capture_sys.sink_path);
    if (ret != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Fail to setup capture path (ret=%d)", ret);
        return -1;
    }
    esp_capture_enable_path(capture_sys.sink_path, ESP_CAPTURE_RUN_TYPE_ALWAYS);
    ret = esp_capture_start(capture_sys.capture_handle);
    if (ret != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "Fail to start capture (ret=%d)", ret);
        return -1;
    }
    capture_sys.capturing = true;
    /* After esp_capture_start(), so it applies to the handle esp_capture has
     * actually opened. See VAPI_MIC_GAIN_DB — at the codec's 30 dB default the
     * echo drowns the AEC reference and cancellation fails outright. */
    vapi_media_set_mic_gain(VAPI_MIC_GAIN_DB);
    vapi_media_set_digital_gain(VAPI_MIC_DIGITAL_GAIN_DB);
#if VAPI_ECHO_GATE
    vapi_media_set_echo_gate(VAPI_ECHO_GATE_ATTEN_DB);
#endif
    ESP_LOGI(TAG, "capture started: PCM %d Hz / %dch / %d-bit",
             VAPI_SAMPLE_RATE, VAPI_CHANNELS, VAPI_BITS_PER_SAMP);
    return 0;
}

int vapi_media_capture_stop(void)
{
    if (!capture_sys.capturing) {
        return 0;
    }
    capture_sys.capturing = false;
    esp_capture_stop(capture_sys.capture_handle);
    capture_sys.sink_path = NULL;
    ESP_LOGI(TAG, "capture stopped");
    return 0;
}

int vapi_media_read_frame(uint8_t *buf, int buf_size)
{
    if (!capture_sys.capturing || capture_sys.sink_path == NULL) {
        return 0;
    }
    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
    };
    /* `false` = do not block: the send task owns its own 20 ms cadence and must
     * stay responsive to a hangup even when the mic path stalls. */
    if (esp_capture_acquire_path_frame(capture_sys.sink_path, &frame, false) != ESP_CAPTURE_ERR_OK) {
        return 0;
    }
    int n = frame.size < buf_size ? frame.size : buf_size;
    if (frame.size > buf_size) {
        ESP_LOGW(TAG, "capture frame %d > buffer %d, truncating", frame.size, buf_size);
    }
    memcpy(buf, frame.data, n);
    esp_capture_release_path_frame(capture_sys.sink_path, &frame);
    /* After the AEC, never before: amplifying the mic ahead of the canceller
     * would scale the echo right back up and undo the whole fix. */
    apply_digital_gain((int16_t *)buf, n / (int)sizeof(int16_t));

    /* Gate last, so the attenuation is not undone by the makeup gain. */
    if (s_gate_atten_q8 >= 0) {
        uint32_t t = now_ms();
        bool shut = (int32_t)(s_far_until_ms - t) > 0;
        int16_t *p = (int16_t *)buf;
        int count = n / (int)sizeof(int16_t);
        s_g_frames++;
        if (shut) {
            s_g_shut++;
            if (s_gate_atten_q8 == 0) {
                memset(p, 0, (size_t)n);
            } else {
                for (int i = 0; i < count; i++) {
                    p[i] = (int16_t)(((int32_t)p[i] * s_gate_atten_q8) >> 8);
                }
            }
        }
        /* Measured after the gate, unlike the first version of this telemetry —
         * reading it before made a working gate and a broken one look alike. */
        uint32_t lvl = mean_abs(p, count);
        if (lvl > s_g_out_level) {
            s_g_out_level = lvl;
        }
        if ((uint32_t)(t - s_g_report_ms) > 5000) {
            s_g_report_ms = t;
            ESP_LOGI(TAG, "gate: shut %lu/%lu | far hits %lu | playout lead %lu ms | in pk %lu | mic pk POST-gate %lu",
                     (unsigned long)s_g_shut, (unsigned long)s_g_frames,
                     (unsigned long)s_g_far_hits, (unsigned long)s_g_lead_ms,
                     (unsigned long)s_g_in_level, (unsigned long)s_g_out_level);
            s_g_frames = s_g_shut = s_g_far_hits = 0;
            s_g_in_level = s_g_out_level = 0;
        }
    }
    return n;
}

/* --- Playback ------------------------------------------------------------- */

int vapi_media_play_start(void)
{
    if (player_sys.stream_open) {
        return 0;
    }
    /* The stream we feed is mono, matching what Vapi sends. */
    av_render_audio_info_t aud_info = {
        .codec           = AV_RENDER_AUDIO_CODEC_PCM,
        .sample_rate     = VAPI_SAMPLE_RATE,
        .channel         = VAPI_CHANNELS,
        .bits_per_sample = VAPI_BITS_PER_SAMP,
    };
    if (av_render_add_audio_stream(player_sys.player, &aud_info) != 0) {
        ESP_LOGE(TAG, "Fail to add audio stream");
        return -1;
    }
    /* ...but the ES8311 must be driven with 2 channels regardless, because the
     * right slot is the DAC loopback the AEC reads as its echo reference. Mute
     * that and echo cancellation has nothing to cancel against. av_render
     * upmixes mono → stereo to satisfy this. Must be set *after*
     * av_render_add_audio_stream(). */
    av_render_audio_frame_info_t frame_info = {
        .sample_rate     = VAPI_SAMPLE_RATE,
        .channel         = 2,
        .bits_per_sample = VAPI_BITS_PER_SAMP,
    };
    av_render_set_fixed_frame_info(player_sys.player, &frame_info);
    player_sys.stream_open = true;
    player_sys.pts_ms = 0;
    s_far_until_ms = 0;
    s_playout_end_ms = 0;
    ESP_LOGI(TAG, "playback started");
    return 0;
}

int vapi_media_play_stop(void)
{
    if (!player_sys.stream_open) {
        return 0;
    }
    player_sys.stream_open = false;
    s_far_until_ms = 0;
    s_playout_end_ms = 0;
    av_render_reset(player_sys.player);
    ESP_LOGI(TAG, "playback stopped");
    return 0;
}

int vapi_media_write_frame(const uint8_t *data, int size)
{
    if (!player_sys.stream_open || size <= 0) {
        return 0;
    }
    uint32_t in_lvl = mean_abs((const int16_t *)data, size / (int)sizeof(int16_t));
    if (in_lvl > s_g_in_level) {
        s_g_in_level = in_lvl;
    }
    /* Advance the playout clock by this frame's duration. If the clock has
     * fallen behind wall time the queue had drained, so restart it from now. */
    uint32_t frame_ms = (uint32_t)(size * 1000 /
        (VAPI_SAMPLE_RATE * VAPI_CHANNELS * (VAPI_BITS_PER_SAMP / 8)));
    uint32_t t_now = now_ms();
    uint32_t start = ((int32_t)(s_playout_end_ms - t_now) > 0) ? s_playout_end_ms : t_now;
    s_playout_end_ms = start + frame_ms;

    if (s_gate_atten_q8 >= 0 && in_lvl > VAPI_ECHO_GATE_THRESHOLD) {
        /* This frame becomes audible at `start` and ends at s_playout_end_ms.
         * Hold the mic shut until then plus a tail for the I2S DMA depth, the
         * room, and any drift in this clock. */
        uint32_t until = s_playout_end_ms + VAPI_ECHO_GATE_HANGOVER_MS;
        if ((int32_t)(until - s_far_until_ms) > 0) {
            s_far_until_ms = until;
        }
        s_g_far_hits++;
        s_g_lead_ms = (uint32_t)((int32_t)(start - t_now) > 0 ? start - t_now : 0);
    }
    av_render_audio_data_t audio_data = {
        .data = (uint8_t *)data,
        .size = (uint32_t)size,
        .pts  = player_sys.pts_ms,
    };
    /* Vapi frames arrive at whatever size it chooses, so derive the timestamp
     * from the byte count rather than assuming a fixed frame duration. */
    player_sys.pts_ms += (uint32_t)(size * 1000 /
        (VAPI_SAMPLE_RATE * VAPI_CHANNELS * (VAPI_BITS_PER_SAMP / 8)));
    return av_render_add_audio_data(player_sys.player, &audio_data);
}

/* --- Hardware check ------------------------------------------------------- */

/* Local mic → speaker loopback for 20 s. Confirms the codec, the PI4IOE speaker
 * unmute, and the AEC source without needing WiFi or a Vapi account. */
int test_capture_to_player(void)
{
    if (capture_sys.capturing || player_sys.stream_open) {
        ESP_LOGW(TAG, "rec2play: a call is using the audio path — hang up first");
        return -1;
    }
    if (vapi_media_play_start() != 0 || vapi_media_capture_start() != 0) {
        vapi_media_capture_stop();
        vapi_media_play_stop();
        return -1;
    }

    /* Suspend the echo gate for the duration. This loop deliberately feeds the
     * speaker from the microphone, which is exactly the condition the gate
     * exists to suppress — leave it armed and the loopback mutes itself after
     * the first loud frame, making working hardware look dead. */
    int32_t saved_gate = s_gate_atten_q8;
    s_gate_atten_q8 = -1;

    uint8_t *buf = malloc(VAPI_FRAME_BYTES * 4);
    if (buf == NULL) {
        vapi_media_capture_stop();
        vapi_media_play_stop();
        return -1;
    }
    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) < start_time + 20000) {
        media_lib_thread_sleep(10);
        int n;
        while ((n = vapi_media_read_frame(buf, VAPI_FRAME_BYTES * 4)) > 0) {
            vapi_media_write_frame(buf, n);
        }
    }
    free(buf);
    s_gate_atten_q8 = saved_gate;
    s_far_until_ms = 0;
    vapi_media_capture_stop();
    vapi_media_play_stop();
    return 0;
}
