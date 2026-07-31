#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "status_led.h"
#include "ble.h"
#include "rns_proxy.h"
#include "thalow_config.h"
#include "system.h"
#include "wifi.h"
#include "config/project_config.h"

#if LED_ACTIVE_LOW
#define LED_ON           (0)
#define LED_OFF          (1)
#else
#define LED_ON           (1)
#define LED_OFF          (0)
#endif

#define LED_TICK_MS            (20)
#define LED_HEARTBEAT_MS       (10000)
#define LED_HEARTBEAT_PULSE_MS (LED_TICK_MS + 5)
#define LED_TRAFFIC_PULSE_MS   (50)

/* Timing for the count-blink patterns (WIFI_AP client count, and the
 * "not connected" blink used by BLE / WIFI_STA). These mirror the look of
 * the battery level blink in battery_blink_seq() so the visual language is
 * consistent across modes. */
#define LED_BLINK_ON_MS        (300)
#define LED_BLINK_GAP_MS       (200)
#define LED_BLINK_CYCLE_MS     (3000)   /* whole pattern repeats every 3 s */

static const char *TAG = "status_led";

static volatile uint32_t s_traffic_count = 0;
static volatile int s_battery_req = 0;
static bool s_task_created = false;

void status_led_notify_traffic( void ) {
	s_traffic_count++;
}

void status_led_show_battery( int percent ) {
	s_battery_req = percent + 1;
}

/* Forward decl: factory_reset_blink() below uses set_led() before its
 * definition (battery_blink_seq relied on it being defined earlier). */
static void set_led( bool on );

/* Set by status_led_factory_reset_notify(); the status_task picks it up
 * at the top of its loop and runs a blocking 2-second fast blink. */
static volatile bool s_factory_reset = false;

void status_led_factory_reset_notify( void ) {
	s_factory_reset = true;
}

static void factory_reset_blink( void ) {
	/* 20 cycles x (50 ms on + 50 ms off) = 2 s of fast blinking, then
	 * leave the LED solid on so the user sees a clear "reset committed"
	 * state during the brief window before esp_restart(). */
	for (int i = 0; i < 20; i++) {
		set_led(true);
		vTaskDelay(pdMS_TO_TICKS(50));
		set_led(false);
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	set_led(true);
}

static void set_led( bool on ) {
	gpio_set_level(LED_PIN, on ? LED_ON : LED_OFF);
}

static void battery_blink_seq( int percent ) {
	set_led(false);
	vTaskDelay(pdMS_TO_TICKS(1000));

	if (percent < 10) {
		set_led(true);
		vTaskDelay(pdMS_TO_TICKS(100));
		set_led(false);
		vTaskDelay(pdMS_TO_TICKS(200));
	} else {
		int n = (percent < 25) ? 1 : (percent < 50) ? 2 : (percent < 75) ? 3 : 4;
		for (int i = 0; i < n; i++) {
			set_led(true);
			vTaskDelay(pdMS_TO_TICKS(300));
			set_led(false);
			vTaskDelay(pdMS_TO_TICKS(200));
		}
	}

	set_led(false);
	vTaskDelay(pdMS_TO_TICKS(2000));
}

/* Count-blink: emit `count` short on-pulses within one LED_BLINK_CYCLE_MS
 * window, then stay off for the rest of the window. Each pulse is
 * LED_BLINK_ON_MS on followed by LED_BLINK_GAP_MS off. count=0 means "off
 * for the whole window" (no clients / nothing to report). */
static bool count_blink_on( uint32_t now_ms, int count ) {
	if (count <= 0)
		return false;
	/* Clamp to a sane upper bound so a runaway counter cannot overlap
	 * pulses into the next cycle window. */
	if (count > 6)
		count = 6;

	uint32_t phase = now_ms % LED_BLINK_CYCLE_MS;
	uint32_t pulse_span = (uint32_t)count *
		(LED_BLINK_ON_MS + LED_BLINK_GAP_MS);

	if (phase >= pulse_span)
		return false;
	return (phase % (LED_BLINK_ON_MS + LED_BLINK_GAP_MS)) < LED_BLINK_ON_MS;
}

/* Continuous blink (square wave). Used by BLE-advertising / WIFI_STA-
 * not-connected / BLE-pairing to signal "waiting for a connection". */
static bool square_blink_on( uint32_t now_ms, uint32_t half_period_ms ) {
	return (now_ms / half_period_ms) & 1;
}

/* Compute whether the mode LED should be lit at time now_ms. Modes that
 * report a connection (TCP, WIFI_STA connected) just return a steady on. */
static bool compute_mode_on( led_mode_t mode, uint32_t now_ms ) {
	switch (mode) {
	case LED_MODE_BLE:
		switch (ble_get_state()) {
		case BLE_STATE_CONNECTED:
			return true;
		case BLE_STATE_PAIRING:
			/* fast blink while the pairing window is open */
			return square_blink_on(now_ms, 50);
		case BLE_STATE_ON:
			/* advertising, waiting for a client -- slow blink so the user
			 * sees the device is alive and discoverable, mirroring the
			 * WIFI_STA-not-connected pattern. */
			return square_blink_on(now_ms, 500);
		case BLE_STATE_OFF:
		default:
			return false;
		}
	case LED_MODE_TCP:
		/* "TCP link" here means: at least one external Reticulum client is
		 * connected to this device's RNS TCP proxy port, which it relays to
		 * the radio. We deliberately do NOT report the esp<->radio SLIP
		 * link -- that is an internal detail and is up almost permanently,
		 * so it would make the LED meaningless. */
		return rns_proxy_client_count() > 0;
	case LED_MODE_WIFI_STA:
		if (wifi_sta_connected())
			return true;
		/* Not connected: blink slowly so the user sees it's trying /
		 * waiting, exactly like BLE_STATE_ON above. */
		return square_blink_on(now_ms, 500);
	case LED_MODE_WIFI_AP:
		return count_blink_on(now_ms, wifi_ap_station_count());
	case LED_MODE_TRAFFIC:
	default:
		return false;
	}
}

static void status_task( void *arg ) {
	(void)arg;

	uint32_t last_heartbeat = 0;
	uint32_t last_traffic = 0;
	uint32_t pulse_until = 0;
	uint32_t traffic_until = 0;

	gpio_config_t led_conf = {
		.pin_bit_mask = (1ULL << LED_PIN),
		.mode         = GPIO_MODE_OUTPUT,
		.pull_up_en   = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	gpio_config(&led_conf);
	gpio_set_level(LED_PIN, LED_OFF);

	vTaskDelay(pdMS_TO_TICKS(3000));

	last_heartbeat = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

	for (;;) {
		if (s_factory_reset) {
			s_factory_reset = false;
			factory_reset_blink();
			continue;
		}
		if (s_battery_req != 0) {
			int p = s_battery_req - 1;
			s_battery_req = 0;
			battery_blink_seq(p);
			continue;
		}

		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

		bool on = compute_mode_on(thalow_config_get_led_mode(), now);

		if (thalow_config_get_led_heartbeat()) {
			if ((now - last_heartbeat) >= LED_HEARTBEAT_MS) {
				last_heartbeat = now;
				pulse_until = now + LED_HEARTBEAT_PULSE_MS;
			}
			if ((int32_t)(pulse_until - now) > 0)
				on = true;
		}

		if (s_traffic_count != last_traffic) {
			last_traffic = s_traffic_count;
			traffic_until = now + LED_TRAFFIC_PULSE_MS;
		}
		if ((int32_t)(traffic_until - now) > 0)
			on = true;

		set_led(on);

		vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
	}
}

void status_led_init( void ) {
	if (s_task_created)
		return;
	s_task_created = true;
	xTaskCreate(status_task, "status_led", 4096, NULL, 5, NULL);
	ESP_LOGI(TAG, "initialized");
}
