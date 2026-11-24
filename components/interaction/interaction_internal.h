#pragma once
#include "interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

void interaction_send(const input_event_t *ev);

esp_err_t sensors_init(void);
void sensors_start(void);
void sensors_stop(void);

esp_err_t sfx_init(void);
void sfx_start(void);
void sfx_stop(void);

#ifdef __cplusplus
}
#endif