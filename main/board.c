/* Board bring-up for the M5Stack AtomS3R + Atomic Echo Base.
 *
 * Most of the work is done by the `codec_board` component via the
 * ATOMS3_ECHO_BASE entry in board_cfg.txt. Two things it does not cover:
 *
 *   1. The Echo Base's NS4168 amplifier is not wired to a plain PA GPIO
 *      (board_cfg says `pa: -1`). It is gated by a PI4IOE5V6408 I/O expander at
 *      0x43 on the same I2C bus as the ES8311. Without the unmute below the
 *      codec initialises cleanly and plays absolute silence.
 *
 *   2. The AtomS3R's LCD backlight is driven by an LP5562 LED driver at 0x30 on
 *      a *separate* internal I2C bus (SDA 45 / SCL 0). We do not use the display
 *      yet, so that one is left alone — see docs/BUILD-NOTES.md.
 *
 * Register values for the PI4IOE come from the M5Stack AtomS3R HAL, as used in
 * Vapi's 2025 hardware workshop firmware (main/m5-atom-s3.h, ConfigurePI4IOE):
 *   https://github.com/VapiAI/vapicon-2025-hardware-workshop
 */

#include <stdio.h>
#include "esp_log.h"
#include "codec_init.h"
#include "codec_board.h"
#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "Board";

/* PI4IOE5V6408 I/O expander — shares the ES8311's I2C bus. */
#define PI4IOE_I2C_PORT   (0)      /* codec_board's default port for this board */
#define PI4IOE_I2C_ADDR   (0x43)
#define PI4IOE_SCL_HZ     (400 * 1000)

#define PI4IOE_REG_IO_DIR      (0x03)
#define PI4IOE_REG_OUT_STATE   (0x05)
#define PI4IOE_REG_HIGH_Z      (0x07)
#define PI4IOE_REG_PULL_SEL    (0x0D)

static int pi4ioe_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100 /* ms */);
}

/* Take the Echo Base speaker amplifier out of mute. */
static void unmute_echo_base_speaker(void)
{
    /* codec_board owns this bus; init_i2c() is a no-op if it is already up. */
    if (init_i2c(PI4IOE_I2C_PORT) != 0) {
        ESP_LOGE(TAG, "I2C init failed — speaker will stay muted");
        return;
    }
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(PI4IOE_I2C_PORT);
    if (bus == NULL) {
        ESP_LOGE(TAG, "no I2C bus handle for port %d — speaker will stay muted", PI4IOE_I2C_PORT);
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PI4IOE_I2C_ADDR,
        .scl_speed_hz    = PI4IOE_SCL_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "PI4IOE not reachable at 0x%02X — is the Echo Base attached?", PI4IOE_I2C_ADDR);
        return;
    }

    int ret = 0;
    ret |= pi4ioe_write(dev, PI4IOE_REG_HIGH_Z,    0x00); /* leave high-impedance */
    ret |= pi4ioe_write(dev, PI4IOE_REG_PULL_SEL,  0xFF); /* pull-ups on */
    ret |= pi4ioe_write(dev, PI4IOE_REG_IO_DIR,    0x6E); /* 0 = input, 1 = output */
    ret |= pi4ioe_write(dev, PI4IOE_REG_OUT_STATE, 0xFF); /* unmute the speaker */
    if (ret != 0) {
        ESP_LOGE(TAG, "PI4IOE write failed — speaker will stay muted");
        return;
    }
    ESP_LOGI(TAG, "Echo Base speaker unmuted (PI4IOE5V6408 @0x%02X)", PI4IOE_I2C_ADDR);
}

void init_board(void)
{
    ESP_LOGI(TAG, "Init board: %s", TEST_BOARD_NAME);
    set_codec_board_type(TEST_BOARD_NAME);

    /* Must happen before init_codec(): the codec's first playback starts as soon
     * as it is opened, and a muted amp at that point just drops those samples. */
    unmute_echo_base_speaker();

    /* Single ES8311 for both directions, so plain I2S STD mode — no TDM (that
     * is for multi-mic ES7210 arrays). reuse_dev stays false so record and
     * playback get separate esp_codec_dev handles and can be opened with
     * different sample formats. */
    codec_init_cfg_t cfg = {
        .in_mode    = CODEC_I2S_MODE_STD,
        .out_mode   = CODEC_I2S_MODE_STD,
        .in_use_tdm = false,
        .reuse_dev  = false,
    };
    init_codec(&cfg);
}
