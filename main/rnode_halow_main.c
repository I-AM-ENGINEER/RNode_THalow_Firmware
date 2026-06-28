#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "rnode_halow.h"

static const char *TAG = "rnode";

void app_main(void)
{
    /* NVS is required by WiFi/BLE and for storing settings. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "RNode-HaLow firmware starting (ESP32-S3, ESP-IDF)");
    ESP_LOGI(TAG, "board: %s", rnode_halow_board_name());
}
