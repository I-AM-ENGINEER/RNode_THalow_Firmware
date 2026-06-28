idf_component_register(SRCS "rnode_halow_main.c" "rnode_halow.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash
                       REQUIRES esp_event esp_netif)
