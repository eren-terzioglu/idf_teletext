# Teletext viewer — M5Stack Tab5 (ESP32-P4, 1280×720 landscape), LVGL 9, ESP-IDF 6.0
# The pages are downloaded on the device over Wi-Fi and saved in the SPIFFS `storage` partition.
cmake_minimum_required(VERSION 3.16)

# Optional sdkconfig.defaults.local holds the Wi-Fi credentials; it is not shared (.gitignore).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(idf_teletext)
