/* Vapi AtomS3R voice client — entry point.
 *
 * Boots the board + audio pipeline and connects to WiFi. The call itself is
 * driven by the front button (tap to start, tap again to end); a small console
 * over the USB serial monitor exposes start/stop and diagnostics.
 */

#include <stdlib.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_console.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "common.h"
#include "wifi_failover.h"
#include "esp_capture_defaults.h"

#define TAG "VAPI_MAIN"

#define RUN_ASYNC(name, body)           \
    void run_async##name(void *arg)     \
    {                                   \
        body;                           \
        media_lib_thread_destroy(NULL); \
    }                                   \
    media_lib_thread_create_from_scheduler(NULL, #name, run_async##name, NULL);

/* Call creation does a blocking HTTPS POST, so it cannot run on the console
 * task without starving the REPL. */
static int start_cli(int argc, char **argv)
{
    RUN_ASYNC(start, { vapi_call_start(); vapi_refresh_display(); });
    return 0;
}

static int stop_cli(int argc, char **argv)
{
    RUN_ASYNC(stop, { vapi_call_stop(); vapi_refresh_display(); });
    return 0;
}

static int sys_cli(int argc, char **argv)
{
    sys_state_show();
    return 0;
}

static int wifi_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password]\n");
        return -1;
    }
    char *ssid = argv[1];
    char *password = argc > 2 ? argv[2] : NULL;
    return network_connect_wifi(ssid, password);
}

static int dump_cli(int argc, char **argv)
{
    bool enable = (argc > 1);
    printf("Enable AEC dump %d\n", enable);
    esp_capture_enable_aec_src_dump(enable);
    return 0;
}

static int rec2play_cli(int argc, char **argv)
{
    test_capture_to_player();
    return 0;
}

static int say_cli(int argc, char **argv)
{
    if (argc > 1) {
        vapi_send_text(argv[1]);
    } else {
        printf("usage: say \"<text>\"\n");
    }
    return 0;
}

static int control_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: control <mute-assistant|unmute-assistant|"
               "mute-customer|unmute-customer|say-first-message>\n");
        return -1;
    }
    return vapi_send_control(argv[1]);
}

/* Live AEC tuning: the right mic gain depends on the enclosure and how far the
 * speaker sits from the mic, so it is worth being able to move it during a call
 * rather than by reflashing. */
static int gain_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: gain <db>   e.g. gain 18   (default %.0f)\n", (double)VAPI_MIC_GAIN_DB);
        return -1;
    }
    return vapi_media_set_mic_gain(atof(argv[1]));
}

/* Post-AEC makeup gain. This is the one to reach for when Vapi cannot hear the
 * talker; `gain` (analog) is the one that governs whether the AEC works at all. */
static int dgain_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: dgain <db>   e.g. dgain 12   (default %.0f)\n",
               (double)VAPI_MIC_DIGITAL_GAIN_DB);
        return -1;
    }
    return vapi_media_set_digital_gain(atof(argv[1]));
}

/* `gate 0` disables the far-end gate, which is the way to A/B how much of the
 * remaining echo the AEC is actually cancelling on its own. */
static int gate_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: gate <db>|0    e.g. gate 40   (0 = off, restores barge-in)\n");
        return -1;
    }
    return vapi_media_set_echo_gate(atof(argv[1]));
}

/* The AtomS3R has a single button and no VOL+/- keys, so volume lives here. */
static int vol_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: vol <+N|-N>   e.g. vol +8\n");
        return -1;
    }
    vapi_volume_step(atoi(argv[1]));
    return 0;
}

static int init_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "vapi>";
    repl_config.task_stack_size = 10 * 1024;
    repl_config.task_priority = 22;
    repl_config.max_cmdline_length = 1024;
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif
    esp_console_cmd_t cmds[] = {
        { .command = "start",    .help = "Create a Vapi call and start talking\r\n", .func = start_cli },
        { .command = "stop",     .help = "Hang up\r\n",                              .func = stop_cli },
        { .command = "i",        .help = "Show system status / loadings\r\n",        .func = sys_cli },
        { .command = "wifi",     .help = "wifi <ssid> [password]\r\n",               .func = wifi_cli },
        { .command = "dump",     .help = "Toggle AEC data dump\r\n",                 .func = dump_cli },
        { .command = "say",      .help = "say \"<text>\": inject a message into the call\r\n", .func = say_cli },
        { .command = "control",  .help = "control <mute-assistant|...>: send a control envelope\r\n", .func = control_cli },
        { .command = "vol",      .help = "vol <+N|-N>: step speaker volume\r\n",     .func = vol_cli },
        { .command = "gain",     .help = "gain <db>: analog mic gain (governs AEC convergence)\r\n", .func = gain_cli },
        { .command = "dgain",    .help = "dgain <db>: post-AEC makeup gain (governs how well Vapi hears you)\r\n", .func = dgain_cli },
        { .command = "gate",     .help = "gate <db>|0: mic attenuation while the assistant speaks\r\n", .func = gate_cli },
        { .command = "rec2play", .help = "Loopback: record then play (mic/speaker test)\r\n", .func = rec2play_cli },
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return 0;
}

/* Per-thread stack/priority/affinity tuning. Much lighter than the WebRTC
 * sibling needs: with no Opus encoder or decoder in the path, the `aenc` and
 * `Adec` threads that wanted 40 KB each are simply not in this firmware.
 * `SrcRead` still runs the AEC, which is the one compute-heavy stage left. */
static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *thread_cfg)
{
    if (strcmp(thread_name, "SrcRead") == 0) {
        thread_cfg->stack_size = 40 * 1024;
        thread_cfg->priority = 16;
        thread_cfg->core_id = 0;
    }
    if (strcmp(thread_name, "Adec") == 0) {
        /* PCM passthrough, but av_render still runs this thread. */
        thread_cfg->stack_size = 8 * 1024;
        thread_cfg->priority = 10;
        thread_cfg->core_id = 1;
    }
    if (strcmp(thread_name, "buffer_in") == 0) {
        thread_cfg->stack_size = 6 * 1024;
        thread_cfg->priority = 10;
        thread_cfg->core_id = 0;
    }
    if (strcmp(thread_name, "start") == 0 || strcmp(thread_name, "stop") == 0) {
        thread_cfg->stack_size = 8 * 1024;   /* HTTPS POST + TLS handshake */
    }
    /* Pin the mic pump away from the AEC so a burst on one never delays the
     * other; the websocket task runs wherever FreeRTOS puts it. */
    if (strcmp(thread_name, "vapi_tx") == 0) {
        thread_cfg->core_id = 1;
    }
}

static int network_event_handler(bool connected)
{
    /* The call is driven by the front button, not auto-started on connect — so a
     * tap is a clean on/off switch. On WiFi loss, tear down any active call. */
    vapi_set_wifi_state(connected);
    if (connected) {
        wifi_failover_connected();
        ESP_LOGI(TAG, "WiFi connected — tap the button to start a call");
    } else {
        RUN_ASYNC(netstop, { vapi_call_stop(); vapi_refresh_display(); });
    }
    return 0;
}

/* Why did we just boot? A silent reboot mid-call is otherwise very hard to
 * attribute: brownout (speaker load on a bus-powered board), a panic, and the
 * interrupt watchdog all look identical from the outside. */
static void log_reset_reason(void)
{
    const char *why;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  why = "power-on";                        break;
    case ESP_RST_EXT:      why = "external pin";                    break;
    case ESP_RST_SW:       why = "software restart";                break;
    case ESP_RST_PANIC:    why = "PANIC / exception (see backtrace above)"; break;
    case ESP_RST_INT_WDT:  why = "INTERRUPT WATCHDOG (a task blocked IRQs >300ms)"; break;
    case ESP_RST_TASK_WDT: why = "TASK WATCHDOG";                   break;
    case ESP_RST_WDT:      why = "other watchdog";                  break;
    case ESP_RST_BROWNOUT: why = "BROWNOUT — supply sagged (USB power/cable/hub)"; break;
    case ESP_RST_DEEPSLEEP:why = "deep sleep wake";                 break;
    case ESP_RST_USB:      why = "USB peripheral reset";            break;
    case ESP_RST_SDIO:     why = "SDIO";                            break;
    default:               why = "unknown";                         break;
    }
    ESP_LOGW(TAG, "=== boot: reset reason = %s ===", why);
}

/* Periodic memory report. A steady decline in `min` across a long call is the
 * signature of a leak; a large drop in `largest` with `free` still healthy means
 * fragmentation. Both end in an allocation failure and a panic. */
static void log_heap(void)
{
    ESP_LOGI(TAG, "heap: free=%u min=%u largest=%u | psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    log_reset_reason();
    media_lib_add_default_adapter();
    media_lib_thread_set_schedule_cb(thread_scheduler);
    init_board();
    display_init();
#if VAPI_AEC_PROBE
    /* Before vapi_media_buildup(): the probe drives the codec directly, and
     * doing that once av_render owns it corrupts the state av_render needs. */
    vapi_aec_probe();
#endif
    vapi_media_buildup();
    vapi_client_init();
    controls_init();
    init_console();
    wifi_failover_init(network_event_handler);
    log_heap();
    int tick = 0;
    while (1) {
        media_lib_thread_sleep(2000);
        vapi_call_query();
        wifi_failover_poll();
        if (++tick % 5 == 0) {   /* every ~10 s */
            log_heap();
        }
    }
}
