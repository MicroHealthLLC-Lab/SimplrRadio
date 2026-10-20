# The board (BOARD, chosen in the root CMakeLists.txt) picks its source files and drivers;
# everything else is shared.
include("${CMAKE_CURRENT_LIST_DIR}/../board_config.cmake" OPTIONAL)
if(NOT BOARD)
    set(BOARD "tab5")
endif()

if(BOARD STREQUAL "tab5")
    # WiFi through the ESP32-C6 co-processor (esp_hosted over SDIO)
    set(BOARD_SRCS "board_m5stack_tab5.c" "tab5_keyboard.c" "tab5_keymap.c")
    set(BOARD_REQUIRES espressif__m5stack_tab5 esp_driver_i2c esp_hosted)
else()
    # LilyGO T-Deck: WiFi built into the ESP32-S3
    set(BOARD_SRCS "board_tdeck.c" "tdeck_keyboard.c" "tdeck_keymap.c")
    set(BOARD_REQUIRES espressif__esp_bsp_generic espressif__esp_lcd_touch_gt911 esp_driver_i2c
                       esp_driver_i2s esp_driver_gpio)
endif()

idf_component_register(
    SRCS
        "app.cpp"
        ${BOARD_SRCS}
        "bt_hid.cpp"
        "bt_store.cpp"
        "hid_keymap.c"
        "hid_report.c"
        "lv_mem_psram.c"
        "mp3_decoder.c"
        "player.c"
        "portal.cpp"
        "portal_client.cpp"
        "radio_browser.c"
        "secret_vault.cpp"
        "settings.cpp"
        "settings_nvs.cpp"
        "spectrum.c"
        "stream.c"
        "text.c"
        "ui_page.cpp"
        "ui_view.cpp"
        "wifi_mgr.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bt
        esp_app_format
        esp_driver_gpio
        esp_driver_i2s
        esp_event
        esp_http_client
        esp_lcd
        esp_netif
        esp_partition
        esp_psram
        esp_timer
        esp_wifi
        esp-tls
        json
        lvgl__lvgl
        espressif__esp_lvgl_port
        espressif__esp-dsp
        chmorgan__esp-libhelix-mp3
        mbedtls
        nvs_flash
        ${BOARD_REQUIRES}
)

# LVGL's allocator (lv_mem_psram.c) is only referenced from the lvgl library, which links
# after this one: keep it in the image
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
