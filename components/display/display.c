#include "display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "display"
#define W DISPLAY_W
#define H DISPLAY_H
#define STRIP_ROWS 128

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_strip;
static SemaphoreHandle_t s_refresh_sem;
static bool s_inited;
static esp_lcd_touch_handle_t s_tp;
static bool s_touch_inited;

// Same register sequence as the Waveshare BSP. Two variants: a fast one (sleep-out
// ~120ms) for warm boots where the panel is already powered and stable, and a
// conservative one (sleep-out 600ms, as the BSP uses) for cold power-on where the
// panel needs the full settle time or it stays black / mis-colored.
static const co5300_lcd_init_cmd_t s_init_cmds_fast[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 10},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};
static const co5300_lcd_init_cmd_t s_init_cmds_slow[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 10},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

IRAM_ATTR static bool refresh_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t yield = pdFALSE;
    xSemaphoreGiveFromISR(s_refresh_sem, &yield);
    return yield == pdTRUE;
}

// ---- debug text overlay (5x7 font, 2x scale) ----
// Placed in the vertical middle band (widest part of the round panel) and each line
// horizontally centered, so the circular glass does not clip the text.
#define DBG_LINES 6
#define DBG_LINE_H 16
#define DBG_CHAR_W 10
#define DBG_H (DBG_LINES * DBG_LINE_H)
#define DBG_Y0 ((H - DBG_H) / 2)
#define DBG_ROW_BYTES ((W + 7) / 8)

static bool s_dbg_on;
static uint8_t s_dbg_mask[DBG_H][DBG_ROW_BYTES];
static int s_dbg_len[DBG_LINES];
static portMUX_TYPE s_dbg_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct { char c; uint8_t r[7]; } glyph_t;
static const glyph_t s_font[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'_', {0,0,0,0,0,0,0x1F}},
    {'.', {0,0,0,0,0,0x0C,0x0C}},
    {'-', {0,0,0,0x1F,0,0,0}},
    {'=', {0,0,0x1F,0,0x1F,0,0}},
    {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {':', {0,0x0C,0x0C,0,0x0C,0x0C,0}},
};

static const uint8_t *glyph(char c)
{
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].c == c) {
            return s_font[i].r;
        }
    }
    return s_font[0].r;
}

void display_debug_enable(bool on)
{
    s_dbg_on = on;
}

bool display_debug_enabled(void)
{
    return s_dbg_on;
}

void display_debug_set_line(int idx, const char *text)
{
    if (idx < 0 || idx >= DBG_LINES) {
        return;
    }
    portENTER_CRITICAL(&s_dbg_mux);
    memset(s_dbg_mask[idx * DBG_LINE_H], 0, DBG_LINE_H * DBG_ROW_BYTES);
    int len = 0;
    while (text[len] && len < (W / DBG_CHAR_W)) {
        len++;
    }
    s_dbg_len[idx] = len;
    int xoff = (W - len * DBG_CHAR_W) / 2;
    for (int j = 0; j < len; j++) {
        const uint8_t *g = glyph(text[j]);
        for (int gy = 0; gy < 7; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if (g[gy] & (0x10 >> gx)) {
                    int x = xoff + j * DBG_CHAR_W + gx * 2;
                    int y = idx * DBG_LINE_H + gy * 2;
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int xx = x + dx, yy = y + dy;
                            s_dbg_mask[yy][xx >> 3] |= (0x80 >> (xx & 7));
                        }
                    }
                }
            }
        }
    }
    portEXIT_CRITICAL(&s_dbg_mux);
}

esp_err_t display_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3, W * H * 2);
    esp_err_t ret = spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_io);
    if (ret != ESP_OK) {
        return ret;
    }
    const int reset_reason = esp_reset_reason();
    const bool conservative = reset_reason != ESP_RST_SW && reset_reason != ESP_RST_DEEPSLEEP;
    co5300_vendor_config_t vendor_config = {
        .init_cmds = conservative ? s_init_cmds_slow : s_init_cmds_fast,
        .init_cmds_size = sizeof(s_init_cmds_fast) / sizeof(s_init_cmds_fast[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ret = esp_lcd_new_panel_co5300(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_lcd_panel_set_gap(s_panel, 0x06, 0);
    // Initialize before any Deep-Standby: sleeping an uninitialized panel can trap it
    // in a state the wake-up restore cannot recover (black screen on power-on).
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    // The AMOLED analog domain can latch a wrong color state that a GPIO reset cannot
    // clear (it does not cut internal analog power). With a reset GPIO, disp_sleep
    // enters Deep Standby (cuts internal power) and wake does reset+init, i.e. a true
    // power-cycle. On cold power-on let the panel finish its own power-on reset first,
    // else Deep Standby can trap it asleep (black screen).
    if (conservative) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_lcd_panel_disp_sleep(s_panel, true);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_disp_sleep(s_panel, false);
    vTaskDelay(pdMS_TO_TICKS(conservative ? 300 : 150));
    if (conservative) {
        esp_lcd_panel_init(s_panel);
    }
    // Hide the panel until the first frame is in GRAM; otherwise DISPON shows
    // uninitialized GRAM as a colored flash.
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_co5300_set_brightness(s_panel, 100);

    s_refresh_sem = xSemaphoreCreateBinary();
    if (!s_refresh_sem) {
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = refresh_cb };
    esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL);

    s_strip = (uint16_t *)heap_caps_aligned_alloc(64, W * STRIP_ROWS * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_strip) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "panel up (%dx%d) reset=%d conservative=%d", W, H, reset_reason, conservative);
    return ESP_OK;
}

// PSRAM frame buffers are not DMA-capable, so flush via one internal DMA strip,
// waiting for each transfer to finish before reusing it. The CO5300 expects RGB565
// most-significant byte first, so swap each pixel out of the ESP32's little-endian order.
void display_flush_rect(const uint16_t *fb, int x0, int y0, int width, int height)
{
    if (!fb || width <= 0 || height <= 0 || x0 < 0 || y0 < 0 ||
        x0 + width > W || y0 + height > H) {
        return;
    }
    // CO5300 QSPI updates require an even start and an odd inclusive end.
    int x1 = x0 + width;
    int y1 = y0 + height;
    x0 &= ~1;
    y0 &= ~1;
    x1 = (x1 + 1) & ~1;
    y1 = (y1 + 1) & ~1;
    width = x1 - x0;
    height = y1 - y0;
    for (int y = 0; y < height; y += STRIP_ROWS) {
        int rows = (height - y < STRIP_ROWS) ? (height - y) : STRIP_ROWS;
        const uint16_t *src = fb + (size_t)(y0 + y) * W + x0;
        for (int r = 0; r < rows; r++) {
            int yy = y0 + y + r;
            const uint16_t *row = src + (size_t)(r) * W;
            uint16_t *dst = s_strip + (size_t)(r) * width;
            if (s_dbg_on && yy >= DBG_Y0 && yy < DBG_Y0 + DBG_H) {
                const uint8_t *m = s_dbg_mask[yy - DBG_Y0];
                for (int x = 0; x < width; ++x) {
                    int xx = x0 + x;
                    uint16_t v = (m[xx >> 3] & (0x80 >> (xx & 7))) ? 0xFFFF : 0x0000;
                    dst[x] = (uint16_t)((v >> 8) | (v << 8));
                }
            } else {
                for (int x = 0; x < width; ++x) {
                    uint16_t v = row[x];
                    dst[x] = (uint16_t)((v >> 8) | (v << 8));
                }
            }
        }
        if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0 + y, x0 + width, y0 + y + rows, s_strip) == ESP_OK) {
            xSemaphoreTake(s_refresh_sem, portMAX_DELAY);
        }
    }
}

void display_flush(const uint16_t *fb)
{
    display_flush_rect(fb, 0, 0, W, H);
}

void display_on(bool on)
{
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, on);
    }
}

esp_lcd_touch_handle_t display_touch(void)
{
    if (s_touch_inited) {
        return s_tp;
    }
    bsp_display_cfg_t tcfg = {
        .touch_flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    if (bsp_touch_new(&tcfg, &s_tp) != ESP_OK) {
        return NULL;
    }
    s_touch_inited = true;
    return s_tp;
}