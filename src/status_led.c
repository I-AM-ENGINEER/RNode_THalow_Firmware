#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "status_led.h"
#include "ble.h"
#include "config/project_config.h"

#if LED_ACTIVE_LOW
#define LED_ON           (0)
#define LED_OFF          (1)
#else
#define LED_ON           (1)
#define LED_OFF          (0)
#endif

#define STATUS_TASK_MS   (10)

static const char *TAG = "status_led";

static void status_task( void *arg ) {
	(void)arg;

	bool led_state = false;
	uint32_t last_blink = 0;

	gpio_config_t led_conf = {
		.pin_bit_mask  = (1ULL << LED_PIN),
		.mode          = GPIO_MODE_OUTPUT,
		.pull_up_en    = GPIO_PULLUP_DISABLE,
		.pull_down_en  = GPIO_PULLDOWN_DISABLE,
		.intr_type     = GPIO_INTR_DISABLE,
	};
	gpio_config(&led_conf);
	gpio_set_level(LED_PIN, LED_OFF);

	vTaskDelay(pdMS_TO_TICKS(3000));

	for (;;) {
		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
		uint32_t blink_period = 0;

		switch (ble_get_state()) {
		case BLE_STATE_OFF:
			gpio_set_level(LED_PIN, LED_OFF);
			break;
		case BLE_STATE_ON:
			blink_period = 500;
			break;
		case BLE_STATE_PAIRING:
			blink_period = 100;
			break;
		case BLE_STATE_CONNECTED:
			gpio_set_level(LED_PIN, LED_ON);
			break;
		}

		if (blink_period && (now - last_blink) >= blink_period) {
			last_blink = now;
			led_state = !led_state;
			gpio_set_level(LED_PIN, led_state ? LED_ON : LED_OFF);
		}

		vTaskDelay(pdMS_TO_TICKS(STATUS_TASK_MS));
	}
}

void status_led_init( void ) {
	xTaskCreate(status_task, "status_led", 4096, NULL, 5, NULL);
	ESP_LOGI(TAG, "initialized");
}
