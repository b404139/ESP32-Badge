#include "grokbot_app.h"
#include "display.h"
#include "grokbot_player.h"
#include "interaction.h"
#include "storage.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "grokbot";
static constexpr const char *kAtlasPath = "/spiffs/grokbot.atlas";
static constexpr int kCanvasSize = 440;
static constexpr int kOffset = (DISPLAY_W - kCanvasSize) / 2;
static constexpr uint32_t kDebounceMs = 1500;

static GrokBotPlayer s_player;
static uint16_t *s_framebuf;
static uint8_t *s_previous_packed;
static uint16_t s_palette[16];
static int s_dirty_x0;
static int s_dirty_y0;
static int s_dirty_x1;
static int s_dirty_y1;
static int64_t s_decode_total_us;
static int64_t s_draw_total_us;
static int64_t s_flush_total_us;
static volatile bool s_running;
static volatile bool s_paused;
static bool s_reverse;
static bool s_first_frame;
static bool s_debug_was_on;
static int s_manual_index;
static uint32_t s_last_switch_ms;
static uint32_t s_last_tilt_ms;
static TaskHandle_t volatile s_play_task;

static void uppercase_name(char *out, size_t size, const char *name)
{
    size_t i = 0;
    for (; name[i] && i + 1 < size; ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
    }
    out[i] = '\0';
}

static int find_state(const char *name)
{
    for (int i = 0; i < s_player.clip_count(); ++i) {
        if (strcmp(s_player.clip_name(i), name) == 0) {
            return i;
        }
    }
    return -1;
}

static void apply_state(int index)
{
    if (index < 0 || index >= s_player.clip_count() || !s_player.select(index)) {
        return;
    }
    s_manual_index = index;
    s_reverse = false;
    char name[24];
    char line[40];
    uppercase_name(name, sizeof(name), s_player.clip_name(index));
    snprintf(line, sizeof(line), "GROK %s", name);
    display_debug_set_line(0, line);
    snprintf(line, sizeof(line), "NO %02d/%02d", index + 1, s_player.clip_count());
    display_debug_set_line(6, line);
    ESP_LOGI(TAG, "state -> %s", s_player.selected_name());
}

static void switch_state(int delta)
{
    const int count = s_player.clip_count();
    if (count > 0) {
        apply_state((s_manual_index + delta + count) % count);
    }
}

static bool switch_named(const char *name, uint32_t now)
{
    if (now - s_last_tilt_ms < 1000) {
        return false;
    }
    s_last_tilt_ms = now;
    const int index = find_state(name);
    if (index < 0) {
        return false;
    }
    apply_state(index);
    return true;
}

static void random_state(void)
{
    const int count = s_player.clip_count();
    if (count <= 1) {
        return;
    }
    int next = rand() % count;
    if (next == s_manual_index) {
        next = (next + 1) % count;
    }
    apply_state(next);
}

static void process_inputs(void)
{
    input_event_t ev;
    while (xQueueReceive(interaction_queue(), &ev, 0) == pdTRUE) {
        const uint32_t now = interaction_now_ms();
        if (now - s_last_switch_ms < kDebounceMs) {
            continue;
        }
        switch (ev.type) {
        case EV_TOUCH_TAP:
            if (ev.v > 0.0f && ev.v >= 500.0f) {
                break;
            }
            s_last_switch_ms = now;
            switch_state(1);
            sfx_play(SFX_BOOP);
            break;
        case EV_TOUCH_SWIPE:
            s_last_switch_ms = now;
            switch_state(ev.x < 0.0f ? 1 : -1);
            sfx_play(SFX_BOOP);
            break;
        case EV_TOUCH_DOUBLE:
            s_last_switch_ms = now;
            random_state();
            sfx_play(SFX_GIGGLE);
            break;
        case EV_IMU_PAT:
            s_last_switch_ms = now;
            switch_state(1);
            sfx_play(SFX_BOOP);
            break;
        case EV_MIC_BLOW:
            s_last_switch_ms = now;
            switch_state(1);
            sfx_play(SFX_GIGGLE);
            break;
        case EV_MIC_CLAP:
            s_last_switch_ms = now;
            random_state();
            sfx_play(SFX_BOOP);
            break;
        case EV_BUTTON_PWR:
            s_last_switch_ms = now;
            switch_state(1);
            sfx_play(SFX_BOOP);
            break;
        case EV_MIC_VOICE:
            s_last_switch_ms = now;
            if (switch_named("listening", now)) {
                sfx_play(SFX_GIGGLE);
            }
            break;
        case EV_IMU_TILT:
            if (ev.v >= 0.45f && strcmp(s_player.selected_name(), "curious") != 0) {
                s_last_switch_ms = now;
                if (switch_named("curious", now)) {
                    sfx_play(SFX_BOOP);
                }
            }
            break;
        case EV_IMU_PICKUP:
            s_paused = true;
            sfx_play(SFX_YAWN);
            break;
        case EV_IMU_PUTDOWN:
            s_paused = false;
            sfx_play(SFX_BOOP);
            break;
        default:
            break;
        }
    }
}

static void init_palette(void)
{
    for (int i = 0; i < 16; ++i) {
        const int value = i * 17;
        s_palette[i] = static_cast<uint16_t>(((value & 0xf8) << 8) | ((value & 0xfc) << 3) | (value >> 3));
    }
}

static bool draw_frame(void)
{
    const uint8_t *packed = s_player.packed_frame();
    const size_t row_bytes = kCanvasSize / 2;
    const size_t frame_bytes = static_cast<size_t>(kCanvasSize) * row_bytes;
    if (s_first_frame) {
        s_dirty_x0 = 0;
        s_dirty_y0 = 0;
        s_dirty_x1 = kCanvasSize - 1;
        s_dirty_y1 = kCanvasSize - 1;
    } else {
        s_dirty_x0 = kCanvasSize;
        s_dirty_y0 = kCanvasSize;
        s_dirty_x1 = -1;
        s_dirty_y1 = -1;
        for (int y = 0; y < kCanvasSize; ++y) {
            const uint8_t *current = packed + static_cast<size_t>(y) * row_bytes;
            const uint8_t *previous = s_previous_packed + static_cast<size_t>(y) * row_bytes;
            if (memcmp(current, previous, row_bytes) == 0) {
                continue;
            }
            for (size_t bx = 0; bx < row_bytes; ++bx) {
                if (current[bx] != previous[bx]) {
                    const int x = static_cast<int>(bx * 2);
                    if (x < s_dirty_x0) s_dirty_x0 = x;
                    if (x + 1 > s_dirty_x1) s_dirty_x1 = x + 1;
                    if (y < s_dirty_y0) s_dirty_y0 = y;
                    if (y > s_dirty_y1) s_dirty_y1 = y;
                }
            }
        }
        if (s_dirty_x1 < 0) {
            return false;
        }
    }
    for (int y = s_dirty_y0; y <= s_dirty_y1; ++y) {
        const uint8_t *src = packed + static_cast<size_t>(y) * row_bytes + s_dirty_x0 / 2;
        uint16_t *dst = s_framebuf + static_cast<size_t>(kOffset + y) * DISPLAY_W + kOffset + s_dirty_x0;
        for (int x = s_dirty_x0; x <= s_dirty_x1; x += 2) {
            const uint8_t pair = src[(x - s_dirty_x0) >> 1];
            dst[x - s_dirty_x0] = s_palette[pair >> 4];
            dst[x - s_dirty_x0 + 1] = s_palette[pair & 0x0f];
        }
    }
    memcpy(s_previous_packed, packed, frame_bytes);
    return true;
}

static void present_frame(void)
{
    const int64_t draw_start = esp_timer_get_time();
    const bool changed = draw_frame();
    s_draw_total_us += esp_timer_get_time() - draw_start;
    if (s_first_frame) {
        ESP_LOGI(TAG, "first frame drawn");
    }
    const int64_t flush_start = esp_timer_get_time();
    const bool debug_on = display_debug_enabled();
    if (s_first_frame || debug_on || s_debug_was_on) {
        display_flush(s_framebuf);
    } else if (changed) {
        display_flush_rect(s_framebuf, kOffset + s_dirty_x0, kOffset + s_dirty_y0,
                           s_dirty_x1 - s_dirty_x0 + 1, s_dirty_y1 - s_dirty_y0 + 1);
    }
    s_debug_was_on = debug_on;
    s_flush_total_us += esp_timer_get_time() - flush_start;
    if (s_first_frame) {
        ESP_LOGI(TAG, "first frame flushed");
    }
}

static bool render_current(void)
{
    const int64_t decode_start = esp_timer_get_time();
    if (!s_player.decode_current()) {
        return false;
    }
    s_decode_total_us += esp_timer_get_time() - decode_start;
    present_frame();
    return true;
}

static bool render_rebuilt(void)
{
    if (s_first_frame) {
        ESP_LOGI(TAG, "first rebuild begin");
    }
    const int64_t decode_start = esp_timer_get_time();
    if (!s_player.rebuild_current()) {
        return false;
    }
    s_decode_total_us += esp_timer_get_time() - decode_start;
    if (s_first_frame) {
        ESP_LOGI(TAG, "first rebuild decoded");
    }
    present_frame();
    return true;
}

static bool render_backward(void)
{
    const int64_t decode_start = esp_timer_get_time();
    if (!s_player.decode_backward()) {
        return false;
    }
    s_decode_total_us += esp_timer_get_time() - decode_start;
    present_frame();
    return true;
}

static void play_task(void *arg)
{
    const int64_t frame_us = 1000000LL / s_player.fps();
    int64_t next_frame = esp_timer_get_time();
    int64_t report_time = next_frame;
    uint32_t frames_since_report = 0;

    while (s_running) {
        process_inputs();
        bool ok = true;
        if (s_player.rebuild_pending()) {
            ok = render_rebuilt();
            s_player.advance();
        } else if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            next_frame = esp_timer_get_time();
            continue;
        } else if (!s_reverse) {
            ok = render_current();
            if (s_player.current_index() + 1 >= s_player.selected_frame_count()) {
                s_reverse = true;
            } else {
                s_player.advance();
            }
        } else if (s_player.current_index() <= s_player.start_frame()) {
            s_reverse = false;
            if (s_player.current_index() + 1 < s_player.selected_frame_count()) {
                s_player.seek(s_player.current_index() + 1);
            }
            ok = render_current();
            s_player.advance();
        } else {
            ok = render_backward();
        }
        if (!ok) {
            ESP_LOGE(TAG, "frame decode failed");
            s_running = false;
            break;
        }
        if (s_first_frame) {
            display_on(true);
            s_first_frame = false;
        }

        next_frame += frame_us;
        const int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            const int64_t wait_us = next_frame - now;
            TickType_t wait_ticks = pdMS_TO_TICKS(static_cast<uint32_t>((wait_us + 999) / 1000));
            if (wait_ticks < 1) {
                wait_ticks = 1;
            }
            vTaskDelay(wait_ticks);
        } else if (now - next_frame > frame_us) {
            next_frame = now;
            vTaskDelay(1);
        }

        ++frames_since_report;
        if (now - report_time >= 1000000) {
            const uint32_t frames = frames_since_report ? frames_since_report : 1;
            const uint32_t decode_us = static_cast<uint32_t>(s_decode_total_us / frames);
            const uint32_t draw_us = static_cast<uint32_t>(s_draw_total_us / frames);
            const uint32_t flush_us = static_cast<uint32_t>(s_flush_total_us / frames);
            const uint32_t free_spiram_k = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            char line[48];
            snprintf(line, sizeof(line), "FPS %lu D%lu R%lu W%lu PS%luK",
                     static_cast<unsigned long>(frames_since_report),
                     static_cast<unsigned long>(decode_us),
                     static_cast<unsigned long>(draw_us),
                     static_cast<unsigned long>(flush_us),
                     static_cast<unsigned long>(free_spiram_k));
            display_debug_set_line(5, line);
            ESP_LOGI(TAG, "fps=%lu state=%s frame=%d/%d decode=%luus draw=%luus flush=%luus psram=%luK",
                     static_cast<unsigned long>(frames_since_report), s_player.selected_name(),
                     s_player.current_index(), s_player.selected_frame_count(),
                     static_cast<unsigned long>(decode_us), static_cast<unsigned long>(draw_us),
                     static_cast<unsigned long>(flush_us), static_cast<unsigned long>(free_spiram_k));
            frames_since_report = 0;
            s_decode_total_us = 0;
            s_draw_total_us = 0;
            s_flush_total_us = 0;
            report_time = now;
        }
    }
    s_play_task = NULL;
    vTaskDelete(NULL);
}

static void grokbot_stop(void);

static esp_err_t grokbot_start(void)
{
    ESP_LOGI(TAG, "start begin");
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "start display ready");
    ret = storage_mount();
    if (ret != ESP_OK) {
        grokbot_stop();
        return ret;
    }
    ESP_LOGI(TAG, "start storage ready");
    s_framebuf = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(DISPLAY_W) * DISPLAY_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_framebuf) {
        grokbot_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "start framebuffer ready");
    memset(s_framebuf, 0, static_cast<size_t>(DISPLAY_W) * DISPLAY_H * 2);
    s_previous_packed = static_cast<uint8_t *>(heap_caps_malloc(
        static_cast<size_t>(kCanvasSize) * kCanvasSize / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_previous_packed) {
        grokbot_stop();
        return ESP_ERR_NO_MEM;
    }
    init_palette();
    ESP_LOGI(TAG, "start player init");
    if (!s_player.init(kAtlasPath)) {
        grokbot_stop();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "start player ready clips=%d", s_player.clip_count());
    s_manual_index = 0;
    char debug_line[40];
    snprintf(debug_line, sizeof(debug_line), "NO %02d/%02d", 1, s_player.clip_count());
    display_debug_set_line(6, debug_line);
    s_reverse = false;
    s_paused = false;
    s_first_frame = true;
    s_debug_was_on = false;
    s_last_switch_ms = 0;
    s_last_tilt_ms = 0;
    s_decode_total_us = 0;
    s_draw_total_us = 0;
    s_flush_total_us = 0;
    interaction_flush();
    s_running = true;
    if (xTaskCreatePinnedToCore(play_task, "grok_play", 16384, nullptr, 6,
                                (TaskHandle_t *)&s_play_task, 1) != pdPASS) {
        grokbot_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "GROKBOT_READY states=%d fps=%u canvas=%d",
             s_player.clip_count(), static_cast<unsigned>(s_player.fps()), kCanvasSize);
    return ESP_OK;
}

static void grokbot_stop(void)
{
    s_running = false;
    while (s_play_task) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_player.release();
    free(s_previous_packed);
    free(s_framebuf);
    s_previous_packed = nullptr;
    s_framebuf = nullptr;
    s_paused = false;
    s_reverse = false;
    ESP_LOGI(TAG, "grokbot app stopped");
}

extern "C" const app_t grokbot_app = {
    "grokbot",
    grokbot_start,
    grokbot_stop,
};
