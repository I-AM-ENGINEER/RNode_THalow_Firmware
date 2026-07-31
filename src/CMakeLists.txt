idf_component_register(                        SRCS "button.c" "status_led.c" "battery.c" "rns_framing.c" "kiss.c" "ble.c" "main.c" "system.c" "slip.c" "thalow_config.c" "wifi.c" "web_proxy.c" "gzip_inflate.c" "captive_portal.c" "firewall.c" "rns_proxy.c" "switch.c"
                        INCLUDE_DIRS "../inc"
                        PRIV_REQUIRES nvs_flash
                        REQUIRES esp_event esp_netif esp_driver_uart esp_driver_gpio lwip bt esp_wifi esp_http_server esp_http_client json esp_timer esp_psram esp_driver_tsens esp_adc spi_flash esp_hw_support esp_common esp_system mdns)

target_add_binary_data(${COMPONENT_LIB} "../web/thalow_inject.js" TEXT)

# --- auto version from git ---
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} describe --tags --exact-match HEAD
    OUTPUT_VARIABLE _git_tag
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_res)
if(_git_res EQUAL 0 AND NOT "${_git_tag}" STREQUAL "")
    set(FW_VERSION_STR "${_git_tag}")
else()
    set(FW_VERSION_STR "v0.0.0")
endif()
message(STATUS "Firmware version: ${FW_VERSION_STR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${FW_VERSION_STR}")
