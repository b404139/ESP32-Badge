#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Unified input event vocabulary shared by all apps. Sensors (touch/IMU/mic) run
 * always-on in the interaction component and publish here; the active app drains
 * the queue and maps events to its own reactions. */
typedef enum {
    EV_TOUCH_TAP, EV_TOUCH_DOUBLE, EV_TOUCH_STROKE, EV_TOUCH_STROKE_END,
    EV_TOUCH_HOLD_BEGIN, EV_TOUCH_HOLD_END,
    EV_TOUCH_SWIPE, /* x,y = net displacement in pixels (directional drag) */
    EV_IMU_PAT, EV_IMU_SHAKE, EV_IMU_TILT, EV_IMU_PICKUP, EV_IMU_PUTDOWN,
    EV_MIC_BLOW, EV_MIC_CLAP, EV_MIC_VOICE,
    EV_BUTTON_PWR,
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    float x, y; /* normalized -1..1 for touch/tilt; pixels for EV_TOUCH_SWIPE */
    float v; /* touch hold duration in ms, or tilt magnitude */
    uint32_t ts;
} input_event_t;

typedef enum { SFX_BOOP, SFX_PURR, SFX_GIGGLE, SFX_YAWN } sfx_id_t;

/* Idempotent hardware bring-up (touch/IMU/mic/speaker) + queues. Call once at boot. */
esp_err_t interaction_init(void);
/* Start always-on sensing + sfx tasks. Call once at boot. */
void interaction_start(void);
void interaction_stop(void);

QueueHandle_t interaction_queue(void);
/* Drop pending events (call on app switch so stale input doesn't leak across apps). */
void interaction_flush(void);
void sfx_play(sfx_id_t id);
uint32_t interaction_now_ms(void);

#ifdef __cplusplus
}
#endif