#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config/project_config.h"
#include "system.h"

static const char *TAG = LOG_TAG_MAIN;

void app_main( void ) {
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_netif_init());

	ESP_LOGI(TAG, "RNode-HaLow firmware starting (ESP32-S3, ESP-IDF)");
	ESP_LOGI(TAG, "board: %s", BOARD_NAME);

	xTaskCreate(system_run, "system", 4096, NULL, 5, NULL);
}
