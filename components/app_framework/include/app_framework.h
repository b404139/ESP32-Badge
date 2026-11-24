#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    esp_err_t (*start)(void);
    void (*stop)(void);
} app_t;

esp_err_t app_framework_init(void);
esp_err_t app_framework_register(const app_t *app);
esp_err_t app_framework_start(void);
void app_framework_next(void);

#ifdef __cplusplus
}
#endif