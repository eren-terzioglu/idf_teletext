idf_component_register(
    SRCS
        "main.c"
        "tab5_display.c"
        "teletext_ui.c"
        "teletext_pages.c"
        "teletext_store.c"
        "teletext_cache.c"
        "teletext_wifi.c"
        "teletext_online.c"
    INCLUDE_DIRS "."
    REQUIRES spiffs esp_rom esp_timer nvs_flash
             m5stack_tab5 espressif__esp_lcd_st7121 esp_lcd esp_driver_i2c espressif__esp_lcd_touch lvgl__lvgl
             esp_netif esp_event esp_wifi esp_http_client mbedtls
             espressif__esp_wifi_remote espressif__esp_hosted espressif__cjson
             libwebp
)
