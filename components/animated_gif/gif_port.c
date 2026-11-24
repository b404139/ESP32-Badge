#include "gif_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

long millis(void)
{
    return (long)(esp_timer_get_time() / 1000);
}

void delay(unsigned long ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}