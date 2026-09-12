/* AtomS3R 128x128 status display — colour-coded call state with a text label.
 *
 * Each state paints the screen one colour and centres a short instruction on it
 * ("PUSH TO CALL", "TALK / TAP TO END"). The colour is what you read across the
 * room; the text is what you read when you pick it up.
 *
 * Deliberately no LVGL — a 5x7 bitmap font scaled 2-3x is both bigger and far
 * cheaper than LVGL's smallest built-in face, and it costs one 475-byte table
 * instead of a rendering library. See font5x7.h.
 *
 * Two chips are involved, on two different buses:
 *
 *   - GC9107 LCD on SPI3 (MOSI 21, SCLK 15, CS 14, DC 42, RST 48). It is close
 *     enough to a GC9A01 that Espressif's esp_lcd_gc9a01 driver runs it, given
 *     the vendor init sequence below.
 *   - LP5562 LED driver at 0x30 driving the backlight (W channel). It sits on
 *     the AtomS3R's *internal* I2C bus (SDA 45 / SCL 0), which is a different
 *     bus from the Echo Base codec's (SDA 38 / SCL 39). Without this the panel
 *     is initialised and updating correctly but completely dark.
 *
 * Pin assignments, the GC9107 init commands, and the LP5562 register sequence
 * all come from the M5Stack AtomS3R HAL, as used in
 * Vapi's 2025 hardware workshop firmware (https://github.com/VapiAI/vapicon-2025-hardware-workshop)
 * (main/m5-atom-s3.h).
 */

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display.h"
#include "font5x7.h"

static const char *TAG = "Display";

#define LCD_W          128
#define LCD_H          128
/* The controller's frame RAM is taller than the visible panel; the visible area
 * starts 32 rows down. that workshop firmware does the same with lv_display_set_offset(0, 32).
 * If a band at the top or bottom stays unpainted, this is the number to change. */
#define LCD_Y_OFFSET   32

#define LCD_SPI_HOST   SPI3_HOST
#define PIN_LCD_MOSI   GPIO_NUM_21
#define PIN_LCD_SCLK   GPIO_NUM_15
#define PIN_LCD_CS     GPIO_NUM_14
#define PIN_LCD_DC     GPIO_NUM_42
#define PIN_LCD_RST    GPIO_NUM_48
#define LCD_PCLK_HZ    (40 * 1000 * 1000)

/* LP5562 backlight driver — internal I2C bus, distinct from the codec's. */
#define BL_I2C_PORT    (1)          /* port 0 belongs to codec_board */
#define PIN_BL_SDA     GPIO_NUM_45
#define PIN_BL_SCL     GPIO_NUM_0
#define LP5562_ADDR    (0x30)
#define LP5562_SCL_HZ  (400 * 1000)

#define LP5562_REG_ENABLE   (0x00)
#define LP5562_REG_CONFIG   (0x08)
#define LP5562_REG_W_PWM    (0x0E)
#define LP5562_REG_LED_MAP  (0x70)

/* Text is composed into a full framebuffer in PSRAM (plentiful: 8 MB), then
 * pushed out in horizontal strips through one small DMA-capable buffer, since
 * esp_lcd wants DMA memory to transmit from. */
#define STRIP_ROWS     (16)
#define TEXT_MARGIN_PX (126)   /* usable width when picking a font scale */

// clang-format off
static const gc9a01_lcd_init_cmd_t gc9107_init_cmds[] = {
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb2, (uint8_t[]){0x2f}, 1, 0},
    {0xb3, (uint8_t[]){0x03}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x01}, 1, 0},
    {0xac, (uint8_t[]){0xcb}, 1, 0},
    {0xab, (uint8_t[]){0x0e}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x19}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xe8, (uint8_t[]){0x24}, 1, 0},
    {0xe9, (uint8_t[]){0x48}, 1, 0},
    {0xea, (uint8_t[]){0x22}, 1, 0},
    {0xc6, (uint8_t[]){0x30}, 1, 0},
    {0xc7, (uint8_t[]){0x18}, 1, 0},
    {0xf0, (uint8_t[]){0x1f, 0x28, 0x04, 0x3e, 0x2a, 0x2e, 0x20, 0x00, 0x0c, 0x06, 0x00, 0x1c, 0x1f, 0x0f}, 14, 0},
    {0xf1, (uint8_t[]){0x00, 0x2d, 0x2f, 0x3c, 0x6f, 0x1c, 0x0b, 0x00, 0x00, 0x00, 0x07, 0x0d, 0x11, 0x0f}, 14, 0},
};
// clang-format on

static esp_lcd_panel_handle_t s_panel;
static uint16_t              *s_strip;      /* STRIP_ROWS * LCD_W pixels, DMA-capable */
static uint16_t              *s_fb;         /* LCD_W * LCD_H pixels, PSRAM */
static display_state_t        s_state = DISPLAY_BOOT;

/* Pack a colour the way this panel wants it on the wire.
 *
 * The panel is configured LCD_RGB_ENDIAN_BGR, so it reads each pixel as
 * B5-G6-R5, and esp_lcd ships buffer bytes verbatim over SPI (high byte first),
 * so the 16-bit word must be byte-swapped in memory.
 *
 * If colours come out with red and blue swapped, drop the channel reordering
 * here. If they come out as noise/garbage, the byte swap is the suspect. */
static inline uint16_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

/* Background, text colour and label per state. '\n' splits lines; the font is
 * uppercase-only, and the scale is chosen from the longest line, so keep lines
 * to 10 characters or fewer or they shrink to an unreadable 1x. */
typedef struct {
    uint8_t     bg[3];
    uint8_t     fg[3];
    const char *label;
} screen_t;

static const screen_t s_screens[] = {
    [DISPLAY_BOOT]        = {{0x30, 0x18, 0x00}, {0xFF, 0xFF, 0xFF}, "STARTING"},
    [DISPLAY_NO_WIFI]     = {{0x70, 0x28, 0x00}, {0xFF, 0xFF, 0xFF}, "NO WIFI"},
    [DISPLAY_IDLE]        = {{0x00, 0x18, 0x60}, {0xFF, 0xFF, 0xFF}, "PUSH TO\nCALL"},
    [DISPLAY_CONNECTING]  = {{0xC0, 0x60, 0x00}, {0x00, 0x00, 0x00}, "CALLING"},
    [DISPLAY_IN_CALL]     = {{0x00, 0x90, 0x20}, {0xFF, 0xFF, 0xFF}, "TALK\n\nTAP TO END"},
    /* Full-scale red: at 0xC0 this panel rendered closer to dark yellow. */
    [DISPLAY_MUTED]       = {{0xFF, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, "MIC OFF\nTAP X2"},
    [DISPLAY_LISTEN_ONLY] = {{0x70, 0x00, 0xC0}, {0xFF, 0xFF, 0xFF}, "LISTEN\nONLY"},
    [DISPLAY_HOLD]        = {{0xC0, 0xC0, 0x00}, {0x00, 0x00, 0x00}, "ON HOLD"},
};

static const char *state_name(display_state_t s)
{
    switch (s) {
    case DISPLAY_BOOT:        return "boot";
    case DISPLAY_NO_WIFI:     return "no-wifi";
    case DISPLAY_IDLE:        return "idle";
    case DISPLAY_CONNECTING:  return "connecting";
    case DISPLAY_IN_CALL:     return "in-call";
    case DISPLAY_MUTED:       return "muted";
    case DISPLAY_LISTEN_ONLY: return "listen-only";
    case DISPLAY_HOLD:        return "hold";
    default:                  return "?";
    }
}

/* --- Framebuffer drawing ---------------------------------------------------
 * Everything composes into s_fb, then flush() pushes it to the panel. */

static void fb_fill(uint16_t packed)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) {
        s_fb[i] = packed;
    }
}

/* Draw one glyph with its top-left at (x0, y0), each font pixel becoming a
 * scale x scale block. Clipped against the framebuffer edges. */
static void fb_char(int x0, int y0, char c, int scale, uint16_t fg)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - FONT_FIRST_CHAR];

    for (int col = 0; col < FONT_WIDTH; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            if ((bits & (1u << row)) == 0) {
                continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                int y = y0 + row * scale + dy;
                if (y < 0 || y >= LCD_H) {
                    continue;
                }
                for (int dx = 0; dx < scale; dx++) {
                    int x = x0 + col * scale + dx;
                    if (x < 0 || x >= LCD_W) {
                        continue;
                    }
                    s_fb[y * LCD_W + x] = fg;
                }
            }
        }
    }
}

static int line_px(int len, int scale)
{
    /* Each character occupies FONT_WIDTH+1 columns; the last one needs no gap. */
    return len * (FONT_WIDTH + 1) * scale - scale;
}

/* Largest scale at which every line still fits the usable width. */
static int pick_scale(const char *text)
{
    int longest = 0, len = 0;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            if (len > longest) longest = len;
            len = 0;
            if (*p == '\0') break;
        } else {
            len++;
        }
    }
    for (int scale = 4; scale > 1; scale--) {
        if (line_px(longest, scale) <= TEXT_MARGIN_PX) {
            return scale;
        }
    }
    return 1;
}

/* Render a '\n'-separated string centred both ways. */
static void fb_text_centred(const char *text, uint16_t fg)
{
    const int scale     = pick_scale(text);
    const int line_h    = FONT_HEIGHT * scale;
    const int line_step = line_h + 3 * scale;   /* 3 font-pixels of leading */

    int lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
    }

    int block_h = lines * line_step - 3 * scale;
    int y = (LCD_H - block_h) / 2;

    const char *start = text;
    while (1) {
        const char *end = start;
        while (*end && *end != '\n') {
            end++;
        }
        int len = (int)(end - start);
        if (len > 0) {
            int x = (LCD_W - line_px(len, scale)) / 2;
            for (int i = 0; i < len; i++) {
                fb_char(x + i * (FONT_WIDTH + 1) * scale, y, start[i], scale, fg);
            }
        }
        y += line_step;
        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
}

/* Push the framebuffer to the panel, one DMA-able strip at a time. */
static void flush(void)
{
    for (int y = 0; y < LCD_H; y += STRIP_ROWS) {
        int rows = (y + STRIP_ROWS <= LCD_H) ? STRIP_ROWS : (LCD_H - y);
        memcpy(s_strip, &s_fb[y * LCD_W], (size_t)rows * LCD_W * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + rows, s_strip);
    }
}

static void render(display_state_t state)
{
    if (s_panel == NULL || s_strip == NULL || s_fb == NULL) {
        return;
    }
    const screen_t *sc = &s_screens[state];
    fb_fill(pack(sc->bg[0], sc->bg[1], sc->bg[2]));
    if (sc->label) {
        fb_text_centred(sc->label, pack(sc->fg[0], sc->fg[1], sc->fg[2]));
    }
    flush();
}

static int lp5562_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100 /* ms */);
}

/* Turn the LCD backlight on. Separate bus from the codec, so this creates its
 * own; a failure here is non-fatal but leaves the screen dark. */
static void backlight_on(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BL_I2C_PORT,
        .sda_io_num        = PIN_BL_SDA,
        .scl_io_num        = PIN_BL_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = {.enable_internal_pullup = 1},
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "internal I2C bus init failed — backlight stays off");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LP5562_ADDR,
        .scl_speed_hz    = LP5562_SCL_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "LP5562 not reachable at 0x%02X — backlight stays off", LP5562_ADDR);
        return;
    }

    int ret = 0;
    ret |= lp5562_write(dev, LP5562_REG_ENABLE,  0x40); /* chip enable          */
    ret |= lp5562_write(dev, LP5562_REG_CONFIG,  0x01); /* internal clock       */
    ret |= lp5562_write(dev, LP5562_REG_LED_MAP, 0x00); /* all channels via I2C */
    ret |= lp5562_write(dev, LP5562_REG_CONFIG,  0x41); /* + 558 Hz PWM         */
    ret |= lp5562_write(dev, LP5562_REG_W_PWM,   0xFF); /* backlight full       */
    if (ret != 0) {
        ESP_LOGE(TAG, "LP5562 write failed — backlight stays off");
        return;
    }
    ESP_LOGI(TAG, "backlight on (LP5562 @0x%02X)", LP5562_ADDR);
}

void display_init(void)
{
    backlight_on();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = GPIO_NUM_NC,
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = STRIP_ROWS * LCD_W * (int)sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed — display disabled");
        return;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = PIN_LCD_CS,
        .dc_gpio_num       = PIN_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed — display disabled");
        return;
    }

    gc9a01_vendor_config_t vendor_cfg = {
        .init_cmds      = gc9107_init_cmds,
        .init_cmds_size = sizeof(gc9107_init_cmds) / sizeof(gc9107_init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    if (esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "GC9107 panel init failed — display disabled");
        s_panel = NULL;
        return;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 0, LCD_Y_OFFSET);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_strip = heap_caps_malloc(STRIP_ROWS * LCD_W * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_strip == NULL) {
        ESP_LOGE(TAG, "no DMA memory for the strip buffer — display disabled");
        s_panel = NULL;
        return;
    }
    /* 32 KB — deliberately in PSRAM so it does not compete with WiFi/WebRTC for
     * internal RAM. Only the small strip buffer above has to be DMA-capable. */
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the framebuffer — display disabled");
        s_panel = NULL;
        return;
    }

    ESP_LOGI(TAG, "GC9107 %dx%d ready", LCD_W, LCD_H);
    render(s_state);
}

void display_set_state(display_state_t state)
{
    if (state == s_state) {
        return;
    }
    s_state = state;
    ESP_LOGI(TAG, "state: %s", state_name(state));
    render(state);
}
