# Vendored bitbank2/AnimatedGIF (Apache-2.0), upstream src/AnimatedGIF.{h,cpp} + gif.inl.
# Port adaptations: MAX_WIDTH 512 (500px GIFs), ESP_PLATFORM unaligned access,
# gif_port.* provides millis()/delay() for ESP-IDF. SIMD .S files omitted (C fallback).
idf_component_register(SRCS "AnimatedGIF.cpp" "gif_port.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer)