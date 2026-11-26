#include "interaction_internal.h"
#include "audio.h"
#include "display.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"
#include "qmi8658.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "sensors"
#define MIC_FRAME 240
#define SWIPE_MIN_PX 80

static esp_lcd_touch_handle_t s_tp;
static qmi8658_dev_t *s_imu;
static esp_codec_dev_handle_t s_mic;
static i2c_master_dev_handle_t s_pwr;
static bool s_hw_inited;
static volatile bool s_sensors_running;
static volatile bool s_mic_running;
static TaskHandle_t volatile s_sensor_task;
static TaskHandle_t volatile s_mic_task;

static void sensor_task(void *arg)
{
    uint32_t tick = 0;
    bool down = false, hold_sent = false;
    uint32_t down_ts = 0, last_tap_ts = 0, last_stroke_ev = 0;
    float stroke_dist = 0;
    uint16_t lx = 0, ly = 0, sx = 0, sy = 0;
    float lp_mag = 9.81f, gx = 0, gy = 0, gz = 9.81f, shake_acc = 0;
    uint32_t last_pat = 0, last_shake = 0, last_tilt = 0, ff_ms = 0;
    bool ff = false, pickup_sent = false;
    uint32_t last_pwr_click = 0;
    float last_nx = 0, last_ny = 0;

    while (s_sensors_running) {
        uint32_t now = interaction_now_ms();
        qmi8658_data_t d;
        if (qmi8658_read_sensor_data(s_imu, &d) == ESP_OK) {
            float mag = sqrtf(d.accelX * d.accelX + d.accelY * d.accelY + d.accelZ * d.accelZ);
            lp_mag += 0.05f * (mag - lp_mag);
            float hp = mag - lp_mag;
            if (display_debug_enabled()) {
                static uint32_t last_dbg = 0;
                if (now - last_dbg > 300) {
                    last_dbg = now;
                    char line[48];
                    snprintf(line, sizeof(line), "IMU mag=%.1f hp=%.1f", mag, hp);
                    display_debug_set_line(3, line);
                    float roll = atan2f(gy, gz) * 57.29578f;
                    float pitch = atan2f(-gx, sqrtf(gy * gy + gz * gz)) * 57.29578f;
                    snprintf(line, sizeof(line), "ROL %+.0f PIT %+.0f", roll, pitch);
                    display_debug_set_line(4, line);
                    ESP_LOGI(TAG, "imu mag=%.2f hp=%.2f roll=%.1f pitch=%.1f", mag, hp, roll, pitch);
                }
            }
            gx += 0.1f * (d.accelX - gx);
            gy += 0.1f * (d.accelY - gy);
            gz += 0.1f * (d.accelZ - gz);
            if (hp > 12.0f && now - last_pat > 250) {
                last_pat = now;
                input_event_t ev = { .type = EV_IMU_PAT, .ts = now };
                interaction_send(&ev);
            }
            shake_acc = 0.9f * shake_acc + 0.1f * fabsf(hp);
            if (shake_acc > 6.0f && now - last_shake > 2000) {
                last_shake = now;
                input_event_t ev = { .type = EV_IMU_SHAKE, .ts = now };
                interaction_send(&ev);
            }
            if (mag < 2.5f) {
                ff_ms += 10;
                if (ff_ms > 120 && !pickup_sent) {
                    pickup_sent = true;
                    ff = true;
                    input_event_t ev = { .type = EV_IMU_PICKUP, .ts = now };
                    interaction_send(&ev);
                }
            } else {
                if (ff && mag > 15.0f) {
                    input_event_t ev = { .type = EV_IMU_PUTDOWN, .ts = now };
                    interaction_send(&ev);
                }
                ff = false;
                ff_ms = 0;
                pickup_sent = false;
            }
            if (now - last_tilt > 200) {
                last_tilt = now;
                float nx = gx / 9.81f, ny = gy / 9.81f;
                if (nx > 1) nx = 1;
                if (nx < -1) nx = -1;
                if (ny > 1) ny = 1;
                if (ny < -1) ny = -1;
                if (fabsf(nx - last_nx) > 0.08f || fabsf(ny - last_ny) > 0.08f) {
                    last_nx = nx;
                    last_ny = ny;
                    input_event_t ev = {
                        .type = EV_IMU_TILT, .x = nx, .y = ny,
                        .v = sqrtf(nx * nx + ny * ny), .ts = now,
                    };
                    interaction_send(&ev);
                }
            }
        }
        if ((tick & 1) == 0) {
            esp_lcd_touch_read_data(s_tp);
            uint16_t x[1], y[1], str[1];
            uint8_t num = 0;
            esp_lcd_touch_get_coordinates(s_tp, x, y, str, &num, 1);
            if (num > 0) {
                if (!down) {
                    down = true;
                    down_ts = now;
                    stroke_dist = 0;
                    hold_sent = false;
                    lx = sx = x[0];
                    ly = sy = y[0];
                } else {
                    stroke_dist += hypotf((float)x[0] - lx, (float)y[0] - ly);
                    lx = x[0];
                    ly = y[0];
                    if (stroke_dist > 20 && now - last_stroke_ev > 60) {
                        last_stroke_ev = now;
                        input_event_t ev = {
                            .type = EV_TOUCH_STROKE,
                            .x = (x[0] / 466.0f) * 2 - 1,
                            .y = (y[0] / 466.0f) * 2 - 1,
                            .ts = now,
                        };
                        interaction_send(&ev);
                    }
                    if (!hold_sent && now - down_ts > 600) {
                        hold_sent = true;
                        input_event_t ev = { .type = EV_TOUCH_HOLD_BEGIN, .ts = now };
                        interaction_send(&ev);
                    }
                }
            } else if (down) {
                down = false;
                input_event_t ev = { .ts = now };
                ev.v = (float)(now - down_ts);
                if (hold_sent) {
                    ev.type = EV_TOUCH_HOLD_END;
                } else if (stroke_dist > 30) {
                    ev.type = EV_TOUCH_STROKE_END;
                } else if (now - last_tap_ts < 300) {
                    ev.type = EV_TOUCH_DOUBLE;
                    last_tap_ts = 0;
                } else {
                    ev.type = EV_TOUCH_TAP;
                    ev.x = (lx / 466.0f) * 2 - 1;
                    ev.y = (ly / 466.0f) * 2 - 1;
                    last_tap_ts = now;
                }
                interaction_send(&ev);
                // A directional drag is additionally reported as a swipe so apps that
                // navigate (gif) can use it while stroke-based apps (pet) still see
                // STROKE_END.
                if (!hold_sent && stroke_dist > 30) {
                    float dx = (float)lx - (float)sx;
                    float dy = (float)ly - (float)sy;
                    if (fabsf(dx) > SWIPE_MIN_PX && fabsf(dx) > fabsf(dy) * 1.5f) {
                        input_event_t sw = { .type = EV_TOUCH_SWIPE, .x = dx, .y = dy, .ts = now };
                        interaction_send(&sw);
                    }
                }
            }
        }
        if (s_pwr && (tick % 5) == 0) {
            const uint8_t reg = 0x49;
            uint8_t status = 0;
            if (i2c_master_transmit_receive(s_pwr, &reg, 1, &status, 1, 50) == ESP_OK && (status & 0x08)) {
                const uint8_t clear[] = {0x49, 0x08};
                i2c_master_transmit(s_pwr, clear, sizeof(clear), 50);
                if (now - last_pwr_click > 250) {
                    last_pwr_click = now;
                    input_event_t ev = { .type = EV_BUTTON_PWR, .ts = now };
                    interaction_send(&ev);
                    ESP_LOGI(TAG, "PWR short press");
                }
            }
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_sensor_task = NULL;
    vTaskDelete(NULL);
}

static void audio_rec_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(MIC_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        s_mic_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint32_t blow_ms = 0, voice_ms = 0, last_blow = 0, last_clap = 0, last_voice = 0;
    while (s_mic_running) {
        if (esp_codec_dev_read(s_mic, buf, MIC_FRAME * 2 * sizeof(int16_t)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        float sums[2] = {0}, crossings[2] = {0};
        for (int ch = 0; ch < 2; ch++) {
            const int slot = ch;
            for (int i = 1; i < MIC_FRAME; i++) {
                float sample = buf[i * 2 + slot] / 32768.0f;
                sums[ch] += sample * sample;
                if ((buf[i * 2 + slot] >= 0) != (buf[(i - 1) * 2 + slot] >= 0)) {
                    crossings[ch]++;
                }
            }
        }
        float rms1 = sqrtf(sums[0] / MIC_FRAME);
        float rms2 = sqrtf(sums[1] / MIC_FRAME);
        float rms = fmaxf(rms1, rms2);
        float zcr = (crossings[0] + crossings[1]) / (2.0f * MIC_FRAME);
        uint32_t now = interaction_now_ms();
        if (display_debug_enabled()) {
            static uint32_t last_dbg = 0;
            if (now - last_dbg > 300) {
                last_dbg = now;
                char line[48];
                snprintf(line, sizeof(line), "MIC1=%.3f MIC2=%.3f", rms1, rms2);
                display_debug_set_line(2, line);
                ESP_LOGI(TAG, "mic1 rms=%.3f mic2 rms=%.3f zcr=%.3f", rms1, rms2, zcr);
            }
        }
        if (rms > 0.05f && zcr < 0.10f) {
            blow_ms += 10;
        } else {
            blow_ms = 0;
        }
        if (blow_ms >= 300 && now - last_blow > 1500) {
            last_blow = now;
            blow_ms = 0;
            input_event_t ev = { .type = EV_MIC_BLOW, .ts = now };
            interaction_send(&ev);
        }
        if (rms > 0.3f && now - last_clap > 1000) {
            last_clap = now;
            input_event_t ev = { .type = EV_MIC_CLAP, .ts = now };
            interaction_send(&ev);
        }
        if (rms > 0.02f && rms < 0.2f && zcr > 0.1f && zcr < 0.35f) {
            voice_ms += 10;
        } else {
            voice_ms = 0;
        }
        if (voice_ms >= 500 && now - last_voice > 3000) {
            last_voice = now;
            voice_ms = 0;
            input_event_t ev = { .type = EV_MIC_VOICE, .ts = now };
            interaction_send(&ev);
        }
    }
    free(buf);
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sensors_init(void)
{
    if (s_hw_inited) {
        return ESP_OK;
    }
    s_tp = display_touch();
    if (!s_tp) {
        return ESP_FAIL;
    }

    s_imu = malloc(sizeof(qmi8658_dev_t));
    if (!s_imu) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = qmi8658_init(s_imu, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        return ret;
    }
    qmi8658_set_accel_range(s_imu, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(s_imu, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(s_imu, true);
    qmi8658_write_register(s_imu, QMI8658_CTRL5, 0x03);

    const i2c_device_config_t pwr_cfg = {
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(bsp_i2c_get_handle(), &pwr_cfg, &s_pwr);
    if (ret == ESP_OK) {
        const uint8_t reg = 0x41;
        uint8_t enabled = 0;
        ret = i2c_master_transmit_receive(s_pwr, &reg, 1, &enabled, 1, 100);
        if (ret == ESP_OK) {
            const uint8_t set[] = {0x41, enabled | 0x08};
            ret = i2c_master_transmit(s_pwr, set, sizeof(set), 100);
        }
        if (ret == ESP_OK) {
            const uint8_t clear[] = {0x49, 0x08};
            ret = i2c_master_transmit(s_pwr, clear, sizeof(clear), 100);
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "AXP2101 PWR short-press IRQ enabled");
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 PWR key unavailable (%d); PWR click disabled", ret);
        if (s_pwr) {
            i2c_master_bus_rm_device(s_pwr);
            s_pwr = NULL;
        }
    }

    if (!s_pwr) {
        return ESP_FAIL;
    }
    const uint8_t power_init[][2] = {
        {0x80, 0x01}, {0x90, 0x00}, {0x91, 0x00},
        {0x82, 0x12}, {0x92, 0x1c}, {0x90, 0x01},
    };
    for (size_t i = 0; i < sizeof(power_init) / sizeof(power_init[0]); i++) {
        ret = i2c_master_transmit(s_pwr, power_init[i], sizeof(power_init[i]), 100);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 microphone power init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    uint8_t ldo_readback = 0, voltage_readback = 0;
    i2c_master_transmit_receive(s_pwr, &(const uint8_t){0x90}, 1, &ldo_readback, 1, 100);
    i2c_master_transmit_receive(s_pwr, &(const uint8_t){0x92}, 1, &voltage_readback, 1, 100);
    ESP_LOGI(TAG, "AXP2101 ALDO1 enabled at 3.3V (0x90=%02x 0x92=%02x)", ldo_readback, voltage_readback);

    ret = audio_init();
    if (ret != ESP_OK) {
        return ret;
    }
    s_mic = audio_microphone();
    if (!s_mic) {
        return ESP_FAIL;
    }
    s_hw_inited = true;
    ESP_LOGI(TAG, "touch + imu + mic ready (PWR %s)", s_pwr ? "enabled" : "disabled");
    return ESP_OK;
}

void sensors_start(void)
{
    s_sensors_running = true;
    s_mic_running = true;
    xTaskCreatePinnedToCore(sensor_task, "sensors", 6144, NULL, 5, (TaskHandle_t *)&s_sensor_task, 0);
    xTaskCreatePinnedToCore(audio_rec_task, "mic", 6144, NULL, 5, (TaskHandle_t *)&s_mic_task, 0);
}

void sensors_stop(void)
{
    s_sensors_running = false;
    s_mic_running = false;
    while (s_sensor_task || s_mic_task) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}