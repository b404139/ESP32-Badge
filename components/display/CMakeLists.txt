idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES waveshare__esp32_s3_touch_amoled_1_75 esp_lcd driver
                                espressif__esp_lcd_touch espressif__esp_lcd_co5300
                                esp_system heap)