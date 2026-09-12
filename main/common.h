/* Common header shared by the Vapi AtomS3R voice client. */

#pragma once

#include <stdbool.h>
#include "settings.h"
#include "display.h"
#include "vapi_media.h"
#include "network.h"
#include "sys_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up the audio board (codec, mic, speaker PA).
 */
void init_board(void);

#if VAPI_AEC_PROBE
/**
 * @brief  Boot-time AEC echo-reference diagnostic.
 *
 * Must run before vapi_media_buildup() — it drives the codec directly, and
 * doing that once av_render owns the codec corrupts the state av_render needs.
 */
void vapi_aec_probe(void);
#endif

/**
 * @brief  Vapi call configuration.
 *
 * All fields are borrowed pointers owned by the caller for the session.
 */
typedef struct {
    const char *api_url;      /*!< Base URL, e.g. https://api.vapi.ai */
    const char *api_key;      /*!< Vapi PRIVATE key (required — see README) */
    const char *assistant_id; /*!< Assistant to call (required) */
    const char *first_message;/*!< Optional greeting override (NULL/"" = assistant default) */
    int         max_seconds;  /*!< Optional call cap; <= 0 leaves it to the assistant */
} vapi_call_cfg_t;

/* --- Call lifecycle ------------------------------------------------------- */

/** Create the client's locks. Call once from app_main, before anything else. */
int vapi_client_init(void);

/** Create the call over REST, open the websocket, and start streaming audio. */
int vapi_call_start(void);

/** Hang up and tear down the websocket + audio paths. */
int vapi_call_stop(void);

/** True while the websocket is open and audio is flowing. */
bool vapi_call_is_active(void);

/** Periodic status query (called from the main loop). */
void vapi_call_query(void);

/** Inject a message into the conversation (ClientInboundMessage `add-message`). */
int vapi_send_text(const char *text);

/**
 * @brief  Send a ClientInboundMessage `control` envelope.
 *
 * @param  control  one of: mute-assistant, unmute-assistant, mute-customer,
 *                  unmute-customer, say-first-message. The server rejects
 *                  anything outside that set.
 */
int vapi_send_control(const char *control);

/* --- Physical control actions (wired to the AtomS3R button gestures) ------- */

/** Button tap: start the call if idle, end it if active. */
void vapi_toggle_call(void);

/** Double tap: toggle the microphone mute. */
void vapi_toggle_mute(void);

/** `vol` console command: change speaker volume by delta (clamped 0-100). */
void vapi_volume_step(int delta);

/** Long press: tell the assistant to stop talking (barge-in). */
void vapi_interrupt_assistant(void);

/** Very long press: toggle local hold — mic muted and speaker silenced. */
void vapi_toggle_hold(void);

/** Play a short "button pressed" tone through the speaker. */
void vapi_beep(void);

/** Set up the front button (call once after the board + media are up). */
void controls_init(void);

/** Recompute the status colour from the current call/mute/mode flags. */
void vapi_refresh_display(void);

/** Tell the app whether WiFi is up (drives the idle vs. no-wifi colour). */
void vapi_set_wifi_state(bool connected);

#ifdef __cplusplus
}
#endif
