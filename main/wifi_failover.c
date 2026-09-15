/* Try several WiFi networks in turn.
 *
 * Two things about the underlying helper shape this.
 *
 * It knows one network and retries it forever: its disconnect handler calls
 * esp_wifi_connect() again and nothing ever reconsiders the SSID. Switching
 * therefore has to be driven from outside, through network_connect_wifi(),
 * which reconfigures and restarts the station.
 *
 * And network_init() prefers credentials stored in NVS over the ones passed to
 * it — "Force to use wifi config from nvs" — so once the board has connected to
 * anything, the configured SSID is ignored on every later boot. That silently
 * defeats a network list, so the first network is always applied explicitly
 * here rather than trusted to network_init().
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "network.h"
#include "settings.h"
#include "wifi_failover.h"

#define TAG "WIFI_FO"

typedef struct {
    const char *ssid;
    const char *pass;
} wifi_net_t;

static const wifi_net_t s_all[] = {
    { WIFI_SSID,   WIFI_PASSWORD   },
    { WIFI_SSID_2, WIFI_PASSWORD_2 },
    { WIFI_SSID_3, WIFI_PASSWORD_3 },
};

static wifi_net_t     s_nets[sizeof(s_all) / sizeof(s_all[0])];
static int            s_count;
static int            s_idx;
static volatile int   s_failures;

/* Counts failures only. The switch itself calls esp_wifi_stop(), which must not
 * happen on the event task, so it is left to wifi_failover_poll(). */
static void on_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_event_sta_disconnected_t *e = data;
    ESP_LOGW(TAG, "\"%s\" disconnected: reason=%d rssi=%d",
             s_nets[s_idx].ssid, e ? e->reason : -1, e ? e->rssi : 0);
    s_failures++;
}

void wifi_failover_connected(void)
{
    /* Clear the count, not the index: the network that just worked is the one
     * to try first if it drops. */
    s_failures = 0;
    ESP_LOGI(TAG, "connected on \"%s\"", s_nets[s_idx].ssid);
}

void wifi_failover_poll(void)
{
    if (s_count < 2 || network_is_connected()) {
        return;
    }
    /* Stay on this network for one retry. A dropped association and an absent
     * AP are indistinguishable from here and want opposite responses — the
     * first usually reconnects immediately, the second never will. One retry
     * serves the transient without stranding us on a network that is not
     * there. */
    if (s_failures < WIFI_ATTEMPTS_PER_NET) {
        return;
    }
    s_failures = 0;
    s_idx = (s_idx + 1) % s_count;
    ESP_LOGW(TAG, "switching to \"%s\" (%d of %d)",
             s_nets[s_idx].ssid, s_idx + 1, s_count);
    network_connect_wifi(s_nets[s_idx].ssid, s_nets[s_idx].pass);
}

int wifi_failover_init(int (*on_change)(bool connected))
{
    for (int i = 0; i < (int)(sizeof(s_all) / sizeof(s_all[0])); i++) {
        if (s_all[i].ssid && s_all[i].ssid[0]) {
            s_nets[s_count++] = s_all[i];
        }
    }
    if (s_count == 0) {
        ESP_LOGE(TAG, "no WiFi networks configured — set WIFI_SSID in .env");
        return -1;
    }
    ESP_LOGI(TAG, "%d network%s configured, starting with \"%s\"",
             s_count, s_count == 1 ? "" : "s", s_nets[0].ssid);

    network_init(s_nets[0].ssid, s_nets[0].pass, on_change);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               on_disconnected, NULL);

    /* Apply the first network explicitly. network_init() may have silently
     * preferred whatever is in NVS from a previous boot, which would make the
     * configured order meaningless. */
    network_connect_wifi(s_nets[0].ssid, s_nets[0].pass);
    return 0;
}
