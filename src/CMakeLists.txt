idf_component_register(SRCS "button.c" "status_led.c" "rns_framing.c" "kiss.c" "ble.c" "main.c" "system.c" "slip.c"
                       INCLUDE_DIRS "../inc"
                       PRIV_REQUIRES nvs_flash
                       REQUIRES esp_event esp_netif esp_driver_uart esp_driver_gpio lwip bt)
