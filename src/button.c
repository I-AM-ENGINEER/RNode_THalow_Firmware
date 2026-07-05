#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button.h"
#include "config/project_config.h"

static const char *TAG = "button";

#define BUTTON_TASK_MS  (10)

static button_callback_t s_cb;

static void button_task( void *arg ) {
	(void)arg;

	bool btn_held = false;
	uint32_t press_start = 0;
	bool armed = false;

	vTaskDelay(pdMS_TO_TICKS(3000));

	for (;;) {
		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

		int btn = gpio_get_level(BUTTON_PIN);
		if (btn == 0) {
			if (!btn_held) {
				btn_held = true;
				press_start = now;
				armed = true;
			} else if (armed &&
			           (now - press_start) >= BUTTON_PAIRING_HOLD) {
				armed = false;
				if (s_cb)
					s_cb(BUTTON_EVENT_LONG_PRESS);
			}
		} else {
			btn_held = false;
			armed = false;
		}

		vTaskDelay(pdMS_TO_TICKS(BUTTON_TASK_MS));
	}
}

void button_set_callback( button_callback_t cb ) {
	s_cb = cb;
}

void button_init( void ) {
	gpio_config_t btn_conf = {
		.pin_bit_mask  = (1ULL << BUTTON_PIN),
		.mode          = GPIO_MODE_INPUT,
		.pull_up_en    = GPIO_PULLUP_ENABLE,
		.pull_down_en  = GPIO_PULLDOWN_DISABLE,
		.intr_type     = GPIO_INTR_DISABLE,
	};
	gpio_config(&btn_conf);

	xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);
	ESP_LOGI(TAG, "initialized");
}
