#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_W 466
#define DISPLAY_H 466

// Idempotent panel bring-up: long reset, QSPI init, brightness, DMA strip.
// Leaves the panel OFF (display_on) until the caller has flushed a first frame.
esp_err_t display_init(void);

// Flush a full DISPLAY_W x DISPLAY_H RGB565 (little-endian) framebuffer.
void display_flush(const uint16_t *fb);

// Flush a rectangular region; coordinates are framebuffer-relative.
void display_flush_rect(const uint16_t *fb, int x, int y, int width, int height);

void display_on(bool on);

// Idempotent touch bring-up shared by all apps.
esp_lcd_touch_handle_t display_touch(void);

// Debug overlay: draw up to 6 lines of 5x7 text (2x scale) over the top of every
// flushed frame. Used to inspect live input events / sensor values on-screen.
void display_debug_enable(bool on);
bool display_debug_enabled(void);
void display_debug_set_line(int idx, const char *text);

#ifdef __cplusplus
}
#endif