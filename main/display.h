/* AtomS3R status display — one colour per call state. See display.c. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  What the screen is currently reporting.
 *
 * Ordered roughly by precedence: when several are true at once (muted while in
 * a call, say), vapi_refresh_display() picks the most specific one.
 */
typedef enum {
    DISPLAY_BOOT = 0,     /*!< dim amber   — powered, not yet on WiFi */
    DISPLAY_NO_WIFI,      /*!< amber       — WiFi lost / not joined */
    DISPLAY_IDLE,         /*!< dim blue    — ready, tap to call */
    DISPLAY_CONNECTING,   /*!< bright amber— signaling / ICE in progress */
    DISPLAY_IN_CALL,      /*!< green       — call up, mic live */
    DISPLAY_MUTED,        /*!< red         — call up, mic muted */
    DISPLAY_LISTEN_ONLY,  /*!< purple      — assistant muted */
    DISPLAY_HOLD,         /*!< yellow      — on hold, both directions muted */
} display_state_t;

/** Bring up the backlight + panel and paint the initial state. Safe to fail:
 *  every later call becomes a no-op if the hardware did not come up. */
void display_init(void);

/** Repaint if @p state differs from what is already shown. */
void display_set_state(display_state_t state);

#ifdef __cplusplus
}
#endif
