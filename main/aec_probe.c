/* AEC echo-reference probe — a boot-time diagnostic, not part of the call path.
 *
 * Why this exists: the first real call on this board showed the assistant's own
 * speech coming back as `user:` transcripts — total acoustic echo, zero
 * cancellation. Everything about the AEC *configuration* looks right (the ES8311
 * is opened with no_dac_ref implicitly false, and the capture stream really is
 * 2-channel with mask 3), so the question is what the second slot actually
 * contains at runtime.
 *
 * The AEC on this board works only if the ES8311's "internal reference signal
 * (ADCL + DACR)" mode puts a loopback of the speaker on the right slot of the
 * record stream. If that slot is silent, the canceller is subtracting nothing
 * and echo passes through untouched — which is exactly the symptom.
 *
 * So: read register 0x44 back off the chip, then play a tone and measure the
 * per-slot RMS of the raw 2-channel capture. Left is the mic, right should be
 * the speaker loopback.
 *
 * Runs before vapi_media_buildup() so it owns the codec outright — opening the
 * codec once av_render has it corrupts the state av_render then needs (a
 * documented gotcha on this board). Compile out with VAPI_AEC_PROBE 0.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "codec_init.h"
#include "settings.h"

#if VAPI_AEC_PROBE

#define TAG "AEC_PROBE"

#define ES8311_GPIO_REG44   (0x44)
#define PROBE_SR            (16000)
#define PROBE_MS            (400)
#define PROBE_CHUNK_FRAMES  (PROBE_SR / 1000 * 20)      /* 20 ms */
#define PROBE_ITERS         (PROBE_MS / 20)
#define TONE_HZ             (1000)
#define TONE_AMPL           (9000)

/* RMS and peak of one interleaved slot of a stereo s16 buffer. */
static double slot_rms(const int16_t *buf, int frames, int slot, int *peak)
{
    if (frames <= 0) {
        return 0.0;
    }
    double acc = 0.0;
    for (int i = 0; i < frames; i++) {
        int v = buf[2 * i + slot];
        acc += (double)v * v;
        int a = v < 0 ? -v : v;
        if (peak && a > *peak) {
            *peak = a;
        }
    }
    return sqrt(acc / frames);
}

static void run_phase(const char *label, bool play_tone, int vol,
                      esp_codec_dev_handle_t rec, esp_codec_dev_handle_t play,
                      int16_t *tone, int16_t *cap)
{
    double l_acc = 0.0, r_acc = 0.0;
    int n = 0, l_peak = 0, r_peak = 0;
    if (vol >= 0) {
        esp_codec_dev_set_out_vol(play, vol);
    }
    /* Interleave write and read: the DAC keeps playing out of its own buffer
     * while we read, so the two overlap in time even though the calls don't. */
    for (int i = 0; i < PROBE_ITERS; i++) {
        if (play_tone) {
            esp_codec_dev_write(play, tone, PROBE_CHUNK_FRAMES * 2 * sizeof(int16_t));
        }
        if (esp_codec_dev_read(rec, cap, PROBE_CHUNK_FRAMES * 2 * sizeof(int16_t)) != 0) {
            continue;
        }
        /* Discard the first few chunks: the amp and the I2S rings need a moment
         * to settle, and a cold first read would drag the average down. */
        if (i < 3) {
            continue;
        }
        l_acc += slot_rms(cap, PROBE_CHUNK_FRAMES, 0, &l_peak);
        r_acc += slot_rms(cap, PROBE_CHUNK_FRAMES, 1, &r_peak);
        n++;
    }
    if (n == 0) {
        ESP_LOGE(TAG, "%-12s no frames captured", label);
        return;
    }
    double l = l_acc / n, r = r_acc / n;
    /* ERLE headroom: how far the echo sits above the reference. The canceller
     * needs the echo at or below the reference and, crucially, undistorted —
     * it is a linear filter, so a clipped mic is uncancellable at any gain. */
    double db = (r > 1.0 && l > 1.0) ? 20.0 * log10(l / r) : 0.0;
    ESP_LOGW(TAG, "%-11s mic rms=%7.0f pk=%6d | ref rms=%7.0f pk=%6d | echo %+5.1f dB vs ref%s",
             label, l, l_peak, r, r_peak, db,
             l_peak >= 32000 ? "  <-- MIC CLIPPING" : "");
}

void vapi_aec_probe(void)
{
    esp_codec_dev_handle_t rec  = get_record_handle();
    esp_codec_dev_handle_t play = get_playback_handle();
    if (rec == NULL || play == NULL) {
        ESP_LOGE(TAG, "no codec handles (rec=%p play=%p)", rec, play);
        return;
    }

    esp_codec_dev_sample_info_t play_fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = PROBE_SR,
    };
    esp_codec_dev_sample_info_t rec_fs = {
        .bits_per_sample = 16, .channel = 2, .channel_mask = 3, .sample_rate = PROBE_SR,
    };
    if (esp_codec_dev_open(play, &play_fs) != 0 || esp_codec_dev_open(rec, &rec_fs) != 0) {
        ESP_LOGE(TAG, "failed to open codec for probe");
        esp_codec_dev_close(play);
        esp_codec_dev_close(rec);
        return;
    }
    esp_codec_dev_set_out_vol(play, DEFAULT_PLAYBACK_VOL);

    /* What is actually in the chip? 0x58 = internal reference (ADCL + DACR),
     * i.e. mic left / speaker loopback right. 0x08 = no reference. */
    int reg = -1;
    if (esp_codec_dev_read_reg(rec, ES8311_GPIO_REG44, &reg) == 0) {
        ESP_LOGW(TAG, "ES8311 reg 0x44 = 0x%02X (%s)", reg,
                 reg == 0x58 ? "internal reference ENABLED — right slot should carry the speaker"
                             : "NOT the reference value; expected 0x58");
    } else {
        ESP_LOGE(TAG, "could not read ES8311 reg 0x44");
    }

    int16_t *tone = calloc(PROBE_CHUNK_FRAMES * 2, sizeof(int16_t));
    int16_t *cap  = calloc(PROBE_CHUNK_FRAMES * 2, sizeof(int16_t));
    if (tone == NULL || cap == NULL) {
        ESP_LOGE(TAG, "OOM");
        free(tone); free(cap);
        esp_codec_dev_close(rec); esp_codec_dev_close(play);
        return;
    }
    for (int i = 0; i < PROBE_CHUNK_FRAMES; i++) {
        int16_t v = (int16_t)(TONE_AMPL * sinf(2.0f * (float)M_PI * TONE_HZ * i / PROBE_SR));
        tone[2 * i] = v;
        tone[2 * i + 1] = v;
    }

    float base_gain = 0.0f;
    esp_codec_dev_get_in_gain(rec, &base_gain);
    ESP_LOGW(TAG, "mic in_gain at boot = %.1f dB", base_gain);

    ESP_LOGW(TAG, "--- A: speaker volume sweep (mic gain unchanged) ---");
    run_phase("silence", false, DEFAULT_PLAYBACK_VOL, rec, play, tone, cap);
    static const int vols[] = {85, 70, 60, 50};
    for (int i = 0; i < (int)(sizeof(vols) / sizeof(vols[0])); i++) {
        char label[16];
        snprintf(label, sizeof(label), "vol %d", vols[i]);
        run_phase(label, true, vols[i], rec, play, tone, cap);
    }

    /* The reference is a bit-exact digital copy of the DAC input, so analog mic
     * gain moves the echo without moving the reference. That buys a better
     * echo-to-reference ratio while keeping the speaker usably loud — which
     * turning the volume down does not. */
    ESP_LOGW(TAG, "--- B: mic gain sweep at speaker vol 85 ---");
    static const float gains[] = {30.0f, 24.0f, 18.0f, 12.0f, 6.0f, 0.0f};
    for (int i = 0; i < (int)(sizeof(gains) / sizeof(gains[0])); i++) {
        char label[16];
        snprintf(label, sizeof(label), "gain %.0fdB", gains[i]);
        esp_codec_dev_set_in_gain(rec, gains[i]);
        run_phase(label, true, 85, rec, play, tone, cap);
    }
    esp_codec_dev_set_in_gain(rec, base_gain);
    esp_codec_dev_set_out_vol(play, DEFAULT_PLAYBACK_VOL);
    ESP_LOGW(TAG, "--- want echo at or below 0 dB vs ref, with mic still hearing a voice ---");

    free(tone);
    free(cap);
    esp_codec_dev_close(rec);
    esp_codec_dev_close(play);
}

#endif /* VAPI_AEC_PROBE */
