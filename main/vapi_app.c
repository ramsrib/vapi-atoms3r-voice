/* Vapi voice — application wiring.
 *
 * Owns the UI-facing state (call up? mic muted? assistant muted?), maps the
 * AtomS3R's single button onto call actions, and keeps the screen in sync.
 * The transport itself lives in vapi_client.c; this file never touches the
 * websocket.
 */

#include <string.h>
#include <math.h>
#include "media_lib_os.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "codec_init.h"
#include "common.h"

#define TAG "VAPI_APP"

/* Control state driven by the physical buttons. */
static int  s_volume         = DEFAULT_PLAYBACK_VOL;
static bool s_mic_muted      = false;
static bool s_assistant_muted = false;  /* server-side: mute-assistant */
static bool s_on_hold        = false;   /* both directions muted */
static bool s_have_wifi      = false;

/* Collapse the control flags into the one thing the screen shows. Ordered by
 * specificity: a muted mic during a call is more useful to see than "in call",
 * and hold outranks both. */
void vapi_refresh_display(void)
{
    display_state_t s;
    if (s_on_hold)                        s = DISPLAY_HOLD;
    else if (s_assistant_muted)           s = DISPLAY_LISTEN_ONLY;
    else if (s_mic_muted && vapi_call_is_active()) s = DISPLAY_MUTED;
    else if (vapi_call_is_active())       s = DISPLAY_IN_CALL;
    else if (s_have_wifi)                 s = DISPLAY_IDLE;
    else                                  s = DISPLAY_NO_WIFI;
    display_set_state(s);
}

void vapi_set_wifi_state(bool connected)
{
    s_have_wifi = connected;
    vapi_refresh_display();
}

/* --- Physical control actions (called from controls.c) -------------------- */

/* Short "button pressed" tone. Plays a ~70 ms sine to the speaker.
 *
 * Only while a call is active: av_render already holds the codec open then
 * (16 kHz/2ch/16-bit) so we can write straight into it. Opening and closing the
 * codec ourselves while idle corrupts the state the call's pipeline then needs,
 * which kills playback outright. */
void vapi_beep(void)
{
#define BEEP_SR     VAPI_SAMPLE_RATE
#define BEEP_MS     70
#define BEEP_HZ     1000
#define BEEP_FRAMES ((BEEP_SR) * (BEEP_MS) / 1000)
    static int16_t beep[BEEP_FRAMES * 2];   /* stereo, in .bss (~4.5 KB) */
    static bool    ready = false;

    if (!vapi_call_is_active()) {
        return;
    }
    esp_codec_dev_handle_t play = get_playback_handle();
    if (play == NULL) {
        return;
    }
    if (!ready) {
        const int fade = BEEP_SR * 5 / 1000;   /* 5 ms fade in/out */
        for (int i = 0; i < BEEP_FRAMES; i++) {
            float a = 6000.0f * sinf(2.0f * (float)M_PI * BEEP_HZ * i / BEEP_SR);
            if (i < fade)                a *= (float)i / fade;
            if (i > BEEP_FRAMES - fade)  a *= (float)(BEEP_FRAMES - i) / fade;
            int16_t v = (int16_t)a;
            beep[2 * i] = v;
            beep[2 * i + 1] = v;
        }
        ready = true;
    }
    esp_codec_dev_write(play, beep, sizeof(beep));
}

/* Run the heavy call open/close off the button task, which has a small stack.
 * Inlining it overflows that task and silently kills all button handling after
 * the first press. */
static void call_op_worker(void *arg)
{
    bool start = (bool)(intptr_t)arg;
    if (start) {
        ESP_LOGI(TAG, "button: starting call");
        display_set_state(DISPLAY_CONNECTING);
        vapi_call_start();
    } else {
        ESP_LOGI(TAG, "button: ending call");
        vapi_call_stop();
        s_assistant_muted = false;
        s_on_hold = false;
    }
    /* If vapi_call_start() bailed out, this drops the screen back to idle
     * rather than leaving it stuck on "connecting". */
    vapi_refresh_display();
    media_lib_thread_destroy(NULL);
}

void vapi_toggle_call(void)
{
    bool want_start = !vapi_call_is_active();
    media_lib_thread_handle_t h = NULL;
    if (media_lib_thread_create(&h, "call_op", call_op_worker,
                                (void *)(intptr_t)want_start, 8 * 1024, 10, 0) != 0) {
        ESP_LOGE(TAG, "button: failed to start call op");
    }
}

/* Mute the mic in the codec rather than asking Vapi to (`mute-customer`):
 * muting locally also stops the samples leaving the device, and it takes effect
 * without a round trip. */
void vapi_toggle_mute(void)
{
    s_mic_muted = !s_mic_muted;
    esp_codec_dev_handle_t rec = get_record_handle();
    if (rec) {
        esp_codec_dev_set_in_mute(rec, s_mic_muted);
    }
    ESP_LOGI(TAG, "MUTE: microphone %s", s_mic_muted ? "muted" : "unmuted");
    vapi_refresh_display();
}

void vapi_volume_step(int delta)
{
    s_volume += delta;
    if (s_volume < 0)   s_volume = 0;
    if (s_volume > 100) s_volume = 100;
    esp_codec_dev_handle_t play = get_playback_handle();
    if (play) {
        esp_codec_dev_set_out_vol(play, s_volume);
    }
    ESP_LOGI(TAG, "VOL: %d", s_volume);
}

/* Long press: shut the assistant up (and let it speak again). Server-side, so
 * it also stops Vapi generating speech rather than just silencing the speaker. */
void vapi_interrupt_assistant(void)
{
    if (!vapi_call_is_active()) {
        ESP_LOGW(TAG, "no active call");
        return;
    }
    s_assistant_muted = !s_assistant_muted;
    vapi_send_control(s_assistant_muted ? "mute-assistant" : "unmute-assistant");
    ESP_LOGI(TAG, "assistant %s", s_assistant_muted ? "muted" : "unmuted");
    vapi_refresh_display();
}

/* Very long press: hold — nothing goes either way, but the call stays up. */
void vapi_toggle_hold(void)
{
    if (!vapi_call_is_active()) {
        ESP_LOGW(TAG, "no active call");
        return;
    }
    s_on_hold = !s_on_hold;

    esp_codec_dev_handle_t rec = get_record_handle();
    if (rec) {
        esp_codec_dev_set_in_mute(rec, s_on_hold || s_mic_muted);
    }
    /* Leaving hold must not un-mute an assistant the long-press had muted
     * separately, so the unmute is conditional rather than symmetric. */
    if (s_on_hold) {
        vapi_send_control("mute-assistant");
    } else if (!s_assistant_muted) {
        vapi_send_control("unmute-assistant");
    }
    ESP_LOGI(TAG, "HOLD: %s", s_on_hold ? "on" : "off");
    vapi_refresh_display();
}
