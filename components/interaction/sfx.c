#include "interaction_internal.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>

#define TAG "sfx"
#define RATE 24000
#define MAX_SAMPLES (RATE * 6 / 10)
#define PI 3.14159265358979f

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_q;
static int16_t *s_pcm;
static bool s_sfx_inited;
static volatile bool s_sfx_running;
static TaskHandle_t volatile s_sfx_task;

static void gen(sfx_id_t id, int *out_n)
{
    float dur = 0.12f;
    switch (id) {
    case SFX_PURR: dur = 0.4f; break;
    case SFX_GIGGLE: dur = 0.33f; break;
    case SFX_YAWN: dur = 0.5f; break;
    default: break;
    }
    int n = (int)(RATE * dur);
    for (int i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float s = 0;
        switch (id) {
        case SFX_BOOP: {
            float f = 600 + 300 * (t / dur);
            s = 0.5f * sinf(2 * PI * f * t) * sinf(PI * t / dur);
            break;
        }
        case SFX_PURR:
            s = 0.4f * sinf(2 * PI * 90 * t) * (0.5f + 0.5f * sinf(2 * PI * 25 * t));
            break;
        case SFX_GIGGLE: {
            float seg = fmodf(t, 0.11f);
            if (seg < 0.07f) {
                float f = 800 + 500 * (seg / 0.07f);
                s = 0.4f * sinf(2 * PI * f * t) * sinf(PI * seg / 0.07f);
            }
            break;
        }
        case SFX_YAWN: {
            float f = 400 - 220 * (t / dur);
            s = 0.35f * sinf(2 * PI * f * t) * sinf(PI * t / dur);
            break;
        }
        }
        int16_t v = (int16_t)(s * 32767);
        s_pcm[i] = v;
    }
    *out_n = n;
}

static void sfx_task(void *arg)
{
    sfx_id_t id;
    while (s_sfx_running) {
        if (xQueueReceive(s_q, &id, pdMS_TO_TICKS(100)) == pdTRUE) {
            int n = 0;
            gen(id, &n);
            esp_codec_dev_write(s_spk, s_pcm, n * sizeof(int16_t));
        }
    }
    s_sfx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sfx_init(void)
{
    if (s_sfx_inited) {
        return ESP_OK;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_err_t ret = esp_codec_dev_open(s_spk, &fs);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_codec_dev_set_out_vol(s_spk, 70);
    s_pcm = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm) {
        return ESP_ERR_NO_MEM;
    }
    s_q = xQueueCreate(4, sizeof(sfx_id_t));
    s_sfx_inited = true;
    return ESP_OK;
}

void sfx_start(void)
{
    s_sfx_running = true;
    xTaskCreate(sfx_task, "sfx", 4096, NULL, 4, (TaskHandle_t *)&s_sfx_task);
}

void sfx_stop(void)
{
    if (!s_sfx_task) {
        return;
    }
    s_sfx_running = false;
    while (s_sfx_task) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void sfx_play(sfx_id_t id)
{
    if (s_q) {
        xQueueSend(s_q, &id, 0);
    }
}