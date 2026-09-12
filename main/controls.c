/* Physical controls on the M5Stack AtomS3R.
 *
 * The AtomS3R has exactly one button: the front panel button, GPIO41,
 * active-low with an internal pull-up. So every action is a press gesture:
 *
 *   short press   -> start / end the call
 *   double press  -> mute / unmute the mic (local, in the codec)
 *   long  press   -> mute / unmute the assistant (server-side)
 *   very long     -> hold: silence both directions, keep the call up
 *
 * Volume has no gesture — there is nothing intuitive left, and the console
 * still has it. Use `vol +8` / `vol -8` over the serial monitor.
 *
 * Long presses fire on release, not on the timer expiring, so that a single
 * gesture cannot trigger two actions on the way past a threshold. The tradeoff
 * is that you get no feedback until you let go; vapi_beep() marks the moment.
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "common.h"

#define TAG "CONTROLS"

#define BTN_GPIO        GPIO_NUM_41   /* AtomS3R front button, active low */
#define POLL_MS         15
#define DEBOUNCE_N      3             /* consecutive stable reads before a level counts */

/* Press-duration thresholds, in milliseconds. */
#define LONG_PRESS_MS       700
#define VERY_LONG_PRESS_MS  2500
/* A second press starting within this window of the last release is a double. */
#define DOUBLE_GAP_MS       350

typedef enum {
    GESTURE_SHORT = 0,
    GESTURE_DOUBLE,
    GESTURE_LONG,
    GESTURE_VERY_LONG,
} gesture_t;

static const char *gesture_name(gesture_t g)
{
    switch (g) {
    case GESTURE_SHORT:     return "short";
    case GESTURE_DOUBLE:    return "double";
    case GESTURE_LONG:      return "long";
    case GESTURE_VERY_LONG: return "very-long";
    default:                return "?";
    }
}

static void on_gesture(gesture_t g, uint32_t held_ms)
{
    ESP_LOGI(TAG, "button %s press (%" PRIu32 " ms)", gesture_name(g), held_ms);
    vapi_beep();   /* audible feedback that the press registered */
    switch (g) {
    case GESTURE_SHORT:     vapi_toggle_call();        break;
    case GESTURE_DOUBLE:    vapi_toggle_mute();        break;
    case GESTURE_LONG:      vapi_interrupt_assistant(); break;
    case GESTURE_VERY_LONG: vapi_toggle_hold();   break;
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void controls_task(void *arg)
{
    bool     pressed      = false;  /* debounced button state */
    int      stable       = 0;      /* consecutive reads agreeing with `cand` */
    bool     cand         = false;
    uint32_t press_start  = 0;
    uint32_t last_release = 0;
    /* A short press is held back until the double-press window closes, so we
     * can tell "one tap" from "the first half of two taps". */
    bool     short_pending = false;

    while (1) {
        bool level = (gpio_get_level(BTN_GPIO) == 0);   /* active low */
        if (level == cand) {
            if (stable < DEBOUNCE_N) stable++;
        } else {
            cand = level;
            stable = 1;
        }

        if (stable == DEBOUNCE_N && cand != pressed) {
            pressed = cand;
            uint32_t t = now_ms();
            if (pressed) {
                press_start = t;
                if (short_pending && (t - last_release) <= DOUBLE_GAP_MS) {
                    /* Second tap landed in time — this is a double press. The
                     * pending short is consumed, and the release below is a
                     * no-op so the double does not also emit a short. */
                    short_pending = false;
                    on_gesture(GESTURE_DOUBLE, 0);
                    press_start = 0;
                }
            } else if (press_start != 0) {
                uint32_t held = t - press_start;
                press_start = 0;
                last_release = t;
                if (held >= VERY_LONG_PRESS_MS) {
                    on_gesture(GESTURE_VERY_LONG, held);
                } else if (held >= LONG_PRESS_MS) {
                    on_gesture(GESTURE_LONG, held);
                } else {
                    /* Hold it — a second tap may still turn this into a double. */
                    short_pending = true;
                }
            }
        }

        /* Double-press window elapsed with no second tap: it really was a tap. */
        if (short_pending && !pressed && (now_ms() - last_release) > DOUBLE_GAP_MS) {
            short_pending = false;
            on_gesture(GESTURE_SHORT, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void controls_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d init failed — button disabled", BTN_GPIO);
        return;
    }

    /* 6 KB stack: the poll loop is tiny, but on_gesture plays a tone (codec I/O)
     * and sends control messages; heavy call start/stop is dispatched off-task. */
    if (xTaskCreate(controls_task, "btns", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start button task");
        return;
    }
    ESP_LOGI(TAG, "button ready (tap=call double=mic long=assistant verylong=hold)");
}
