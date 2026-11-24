idf_component_register(SRCS "grokbot_app.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES app_framework display grokbot_player interaction
                                storage esp_timer heap)
