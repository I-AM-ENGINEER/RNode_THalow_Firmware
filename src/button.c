#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button.h"
#include "config/project_config.h"

static const char *TAG = "button";

#define BUTTON_TASK_MS  (10)
#define BUTTON_DEBOUNCE_MS (40)

static button_callback_t s_cb;

static void button_task( void *arg ) {
	(void)arg;

	bool btn_held = false;
	uint32_t press_start = 0;
	bool armed = false;
	bool long_fired = false;
	bool very_long_fired = false;

	vTaskDelay(pdMS_TO_TICKS(3000));

	for (;;) {
		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

		int btn = gpio_get_level(BUTTON_PIN);
		if (btn == 0) {
			if (!btn_held) {
				btn_held = true;
				press_start = now;
				armed = true;
				long_fired = false;
			} else if (armed &&
			           (now - press_start) >= BUTTON_PAIRING_HOLD) {
				armed = false;
				long_fired = true;
				if (s_cb)
					s_cb(BUTTON_EVENT_LONG_PRESS);
			} else if (long_fired && !very_long_fired &&
			           (now - press_start) >= BUTTON_FACTORY_RESET_HOLD) {
				/* Held past the pairing threshold all the way to 30 s: this is a
				 * factory-reset gesture. Fire once; the handler in system.c does
				 * the confirm-blink + NVS erase + reboot. */
				very_long_fired = true;
				if (s_cb)
					s_cb(BUTTON_EVENT_VERY_LONG_PRESS);
			}
		} else {
			if (btn_held && !long_fired) {
				uint32_t held = now - press_start;
				if (held >= BUTTON_DEBOUNCE_MS && s_cb)
					s_cb(BUTTON_EVENT_SHORT_PRESS);
			}
			btn_held = false;
			armed = false;
			very_long_fired = false;
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
