idf_component_register(SRCS "interaction.c" "sensors.c" "sfx.c"
                       INCLUDE_DIRS "include"
                       REQUIRES display waveshare__esp32_s3_touch_amoled_1_75
                                waveshare__qmi8658 espressif__esp_codec_dev
                                espressif__esp_lcd_touch driver esp_timer heap)