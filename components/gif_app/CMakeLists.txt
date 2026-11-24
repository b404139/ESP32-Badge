idf_component_register(SRCS "gif_app.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES app_framework display animated_gif interaction
                                storage esp_timer heap)