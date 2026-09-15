/* Try several WiFi networks in turn.
 *
 * The networking helper this project uses (esp-webrtc-solution's
 * solutions/common) knows about exactly one network and retries it forever, so
 * failover is layered on top of it rather than built into it.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up WiFi across every configured network.
 *
 * Replaces a direct network_init() call. Networks come from WIFI_SSID,
 * WIFI_SSID_2 and WIFI_SSID_3 in settings.h; blank ones are skipped.
 */
int wifi_failover_init(int (*on_change)(bool connected));

/** Call every couple of seconds. Switches networks when the current one has
 *  failed enough times. */
void wifi_failover_poll(void);

/** Tell the failover logic a connection succeeded, so it stops counting. */
void wifi_failover_connected(void);

#ifdef __cplusplus
}
#endif
