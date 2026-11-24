#include "gif_app.h"
#include "display.h"
#include "interaction.h"
#include "AnimatedGIF.h"
#include "storage.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TAG "gif"
// GIFs are pre-cropped to the panel size, so the decoder's composited RGB565 canvas
// has the same stride as the display and can be flushed directly (no extra copy).
#define GW DISPLAY_W
#define GH DISPLAY_H
#define GIF_MAX_BYTES (3200 * 1024)
// COOKED with no draw callback: 8-bpp composited canvas at [0, GW*GH), persistent
// RGB565 canvas at [GW*GH, GW*GH*3). Transparency/disposal accumulate in the decoder.
#define FRAMEBUF_BYTES (GW * GH * 3)
#define COOKED_OFFSET (GW * GH)

static const char *kPaths[] = {
    "/spiffs/idle.gif",
    "/spiffs/happy.gif",
    "/spiffs/playful.gif",
    "/spiffs/sad.gif",
    "/spiffs/shock.gif",
    "/spiffs/sleepy.gif",
};
#define GIF_COUNT ((int)(sizeof(kPaths) / sizeof(kPaths[0])))

static AnimatedGIF s_gif;
static uint8_t *s_framebuf;
static int s_index;
static volatile bool s_running;
static volatile bool s_need_load;
static volatile int s_switch_req;
static volatile bool s_random_req;
static volatile bool s_paused;
static bool s_first_frame;
static TaskHandle_t volatile s_play_task;
static TaskHandle_t volatile s_input_task;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Stream the GIF straight from SPIFFS; the first frame only needs the header plus
// the first frame's compressed data, so switching does not block on a full-file read.
// Size comes from stat() (object index) rather than fseek(END)/ftell.
static void *gif_open(const char *name, int32_t *size)
{
    struct stat st;
    if (stat(name, &st) != 0) {
        ESP_LOGE(TAG, "stat %s failed", name);
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > GIF_MAX_BYTES) {
        ESP_LOGE(TAG, "%s bad size %ld", name, (long)st.st_size);
        return NULL;
    }
    FILE *f = fopen(name, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", name);
        return NULL;
    }
    *size = (int32_t)st.st_size;
    return f;
}

static void gif_close(void *handle)
{
    if (handle) {
        fclose((FILE *)handle);
    }
}

static int32_t gif_read(GIFFILE *f, uint8_t *buf, int32_t len)
{
    int32_t n = len;
    if (f->iSize - f->iPos < n) {
        n = f->iSize - f->iPos;
    }
    if (n <= 0) {
        return 0;
    }
    n = (int32_t)fread(buf, 1, (size_t)n, (FILE *)f->fHandle);
    f->iPos += n;
    return n;
}

static int32_t gif_seek(GIFFILE *f, int32_t pos)
{
    if (pos < 0) {
        pos = 0;
    } else if (pos >= f->iSize) {
        pos = f->iSize - 1;
    }
    f->iPos = pos;
    fseek((FILE *)f->fHandle, pos, SEEK_SET);
    return pos;
}

static bool load_gif(int idx)
{
    const char *path = kPaths[idx];
    s_gif.close();
    s_gif.begin(GIF_PALETTE_RGB565_LE);
    s_gif.setDrawType(GIF_DRAW_COOKED);
    uint32_t t1 = now_ms();
    if (s_gif.open(path, gif_open, gif_close, gif_read, gif_seek, NULL) != 1) {
        ESP_LOGE(TAG, "%s decode open failed (%d)", path, s_gif.getLastError());
        return false;
    }
    if (s_gif.getCanvasWidth() != GW || s_gif.getCanvasHeight() != GH) {
        ESP_LOGE(TAG, "%s unexpected canvas %dx%d", path, s_gif.getCanvasWidth(), s_gif.getCanvasHeight());
        return false;
    }
    s_gif.setFrameBuf(s_framebuf);
    uint32_t t2 = now_ms();
    memset(s_framebuf, 0, FRAMEBUF_BYTES);
    uint32_t t3 = now_ms();
    ESP_LOGI(TAG, "loaded %s open=%u memset=%u ms (loop=%d)", path,
             (unsigned)(t2 - t1), (unsigned)(t3 - t2), s_gif.getLoopCount());
    return true;
}

static void flush_frame(void)
{
    display_flush((const uint16_t *)(s_framebuf + COOKED_OFFSET));
}

static void play_task(void *arg)
{
    s_need_load = true;
    s_first_frame = true;
    bool first_after_load = false;
    while (s_running) {
        if (s_switch_req != 0) {
            int dir = s_switch_req;
            s_switch_req = 0;
            s_index = (s_index + dir + GIF_COUNT) % GIF_COUNT;
            s_need_load = true;
        }
        if (s_random_req) {
            s_random_req = false;
            int r = rand() % GIF_COUNT;
            if (r == s_index) {
                r = (r + 1) % GIF_COUNT;
            }
            s_index = r;
            s_need_load = true;
        }
        bool loaded = false;
        if (s_need_load) {
            s_need_load = false;
            if (!load_gif(s_index)) {
                s_index = (s_index + 1) % GIF_COUNT;
                s_need_load = true;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            loaded = true;
            first_after_load = true;
        }
        // Paused: freeze on the current frame, but still show a newly loaded GIF once.
        if (s_paused && !loaded) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t t0 = now_ms();
        int delay = 0;
        int rc = s_gif.playFrame(false, &delay, NULL);
        if (rc < 0) {
            ESP_LOGE(TAG, "playFrame error (%d)", s_gif.getLastError());
            s_need_load = true;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        flush_frame();
        if (s_first_frame) {
            display_on(true);
            s_first_frame = false;
        }
        if (first_after_load) {
            ESP_LOGI(TAG, "first frame %u ms", (unsigned)(now_ms() - t0));
            first_after_load = false;
        }
        if (s_paused) {
            continue;
        }
        // rc == 0 means last frame; playFrame rewinds to the start on the next call.
        // Abort the remaining frame delay early when a switch arrives.
        int wait = delay - (int)(now_ms() - t0);
        while (s_running && wait > 0 && s_switch_req == 0 && !s_random_req) {
            int chunk = wait > 20 ? 20 : wait;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            wait -= chunk;
        }
    }
    s_play_task = NULL;
    vTaskDelete(NULL);
}

// Consume the shared interaction event stream (same vocabulary as pet) and map it
// to GIF navigation / playback, with synthesized sound feedback.
static void input_task(void *arg)
{
    input_event_t ev;
    while (s_running) {
        if (xQueueReceive(interaction_queue(), &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case EV_TOUCH_SWIPE:
            s_switch_req = (ev.x < 0) ? 1 : -1;
            sfx_play(SFX_BOOP);
            ESP_LOGI(TAG, "swipe dx=%.0f -> %s", ev.x, ev.x < 0 ? "next" : "prev");
            break;
        case EV_TOUCH_TAP:
            s_paused = !s_paused;
            sfx_play(SFX_BOOP);
            ESP_LOGI(TAG, "tap -> %s", s_paused ? "pause" : "resume");
            break;
        case EV_TOUCH_DOUBLE:
            s_random_req = true;
            sfx_play(SFX_GIGGLE);
            ESP_LOGI(TAG, "double -> random");
            break;
        case EV_IMU_SHAKE:
            s_switch_req = 1;
            sfx_play(SFX_BOOP);
            ESP_LOGI(TAG, "shake -> next");
            break;
        case EV_IMU_PICKUP:
            s_paused = true;
            sfx_play(SFX_YAWN);
            ESP_LOGI(TAG, "pickup -> pause");
            break;
        case EV_IMU_PUTDOWN:
            s_paused = false;
            sfx_play(SFX_BOOP);
            ESP_LOGI(TAG, "putdown -> resume");
            break;
        case EV_IMU_PAT:
            sfx_play(SFX_BOOP);
            break;
        case EV_MIC_BLOW:
            s_switch_req = 1;
            sfx_play(SFX_GIGGLE);
            ESP_LOGI(TAG, "blow -> next");
            break;
        case EV_MIC_CLAP:
            s_random_req = true;
            sfx_play(SFX_BOOP);
            ESP_LOGI(TAG, "clap -> random");
            break;
        default:
            break;
        }
    }
    s_input_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t gif_start(void)
{
    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (storage_mount() != ESP_OK) {
        return ESP_FAIL;
    }
    s_framebuf = (uint8_t *)heap_caps_malloc(FRAMEBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_framebuf) {
        return ESP_ERR_NO_MEM;
    }
    s_index = 0;
    s_switch_req = 0;
    s_random_req = false;
    s_paused = false;
    interaction_flush();
    s_running = true;
    xTaskCreatePinnedToCore(play_task, "gif_play", 8192, NULL, 6, (TaskHandle_t *)&s_play_task, 1);
    xTaskCreatePinnedToCore(input_task, "gif_input", 4096, NULL, 5, (TaskHandle_t *)&s_input_task, 0);
    ESP_LOGI(TAG, "gif app started");
    return ESP_OK;
}

static void gif_stop(void)
{
    s_running = false;
    while (s_play_task || s_input_task) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_gif.close();
    free(s_framebuf);
    s_framebuf = NULL;
    ESP_LOGI(TAG, "gif app stopped");
}

extern "C" const app_t gif_app = {
    "gif",
    gif_start,
    gif_stop,
};