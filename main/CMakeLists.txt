idf_component_register(SRCS "lvgl_stable.c" "my_font.c" "my_font_18.c" "my_font_13.c"
                            "light_scene_image.c" "dark_scene_image.c"
                            "heart_image.c" "sun_image.c" "moon_image.c" "lunar_special.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer driver lvgl
                             esp_http_server)
