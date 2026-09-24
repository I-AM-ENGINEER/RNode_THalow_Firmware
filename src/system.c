#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "system.h"
#include "slip.h"
#include "ble.h"
#include "kiss.h"
#include "rns_framing.h"
#include "status_led.h"
#include "button.h"
#include "battery.h"
#include "thalow_config.h"
#include "wifi.h"
#include "web_proxy.h"
#include "rns_proxy.h"
#include "switch.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "config/project_config.h"

static const char *TAG = "system";

#define HALOW_PORT      (4242)
#define MAX_PACKET      (RNS_FRAMING_MAX_PACKET)

static int             sock_fd = -1;
static SemaphoreHandle_t send_mutex;

static kiss_t         kiss;
static rns_framing_t  rns;

static bool           pairing_active = false;
static uint32_t       pairing_started_ms = 0;

// KISS <-> HaLow
static void on_kiss_data( void *user, const uint8_t *data, size_t len ) {
	(void)user;
	/* BLE client sent data -> fan out to radio (and TCP, if connected)
	 * via the central switch. */
	switch_rx(SW_SRC_BLE, data, len);
}

static void on_kiss_tx( void *user, const uint8_t *buf, size_t len ) {
	(void)user;
	ble_write(buf, len);
	ble_flush();
}

// RNS framing <-> KISS callbacks

/* Sink callbacks the switch invokes from its dispatch task. */
static bool ble_sink( const uint8_t *data, size_t len ) {
	/* Wrap as a KISS DATA frame and push to BLE. kiss_send_data builds the
	 * frame and calls on_kiss_tx -> ble_write/ble_flush. Best-effort: if
	 * no phone is connected the bytes are dropped inside ble_write -- we
	 * still report delivered=true so the switch does not pile up packets
	 * forever waiting for a phone that may never come back. */
	kiss_send_data(&kiss, data, len);
	return true;
}

/* Forward decl: system_halow_send returns true once the encoded frame has
 * been pushed onto the SLIP socket (see its definition below). */
static void on_rns_frame( void *user, const uint8_t *data, size_t len ) {
	(void)user;
	status_led_notify_traffic();
	ESP_LOGD(TAG, "rns frame %d -> switch", (int)len);
	/* Radio sent a frame -> fan out to BLE and TCP via the switch. */
	switch_rx(SW_SRC_HALOW, data, len);
}

// Button -> pairing lifecycle

static void on_button_event( button_event_t event ) {
	if (event == BUTTON_EVENT_LONG_PRESS) {
		/* BLE may be disabled in config (ble_en=0). ble_enable_pairing()
		 * touches NimBLE state that is never initialised in that case, so
		 * calling it panics (ble_hs_synced derefs NULL). Bail out cleanly. */
		if (!thalow_config_get_ble_enabled()) {
			ESP_LOGW(TAG, "long-press ignored: BLE disabled by config");
			return;
		}
		pairing_active = true;
		pairing_started_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
		ble_enable_pairing();
	} else if (event == BUTTON_EVENT_SHORT_PRESS) {
		status_led_show_battery((int)battery_get_percent());
	} else if (event == BUTTON_EVENT_VERY_LONG_PRESS) {
		ESP_LOGW(TAG, "FACTORY RESET triggered (30 s hold)");
		/* 2-second fast-blink confirmation, then erase ALL NVS and reboot.
		 * Full erase (not just thalow-config) so BLE bonds / paired phones
		 * are also cleared -- a true factory reset. On reboot,
		 * thalow_config_init() rebuilds defaults from the MAC suffix. */
		status_led_factory_reset_notify();
		vTaskDelay(pdMS_TO_TICKS(2100));
		nvs_flash_erase();
		esp_restart();
	}
}


static bool halow_sink( const uint8_t *data, size_t len ) {
	/* If the radio SLIP socket is down, tell the switch to keep the packet
	 * queued and retry on the next dispatch loop; once the link comes back
	 * up, all queued packets drain. system_halow_send itself logs+drops if
	 * sock_fd < 0, but we short-circuit here so the switch retains it. */
	if (!system_halow_connected())
		return false;
	system_halow_send(data, len);
	return true;
}

void system_halow_send( const uint8_t *data, size_t len ) {
	if (sock_fd < 0) {
		ESP_LOGW(TAG, "halow send: no socket, dropping %d bytes", (int)len);
		return;
	}

	uint8_t enc[MAX_PACKET * 2 + 4];
	int enc_len = rns_framing_encode(data, len, enc, sizeof(enc));
	if (enc_len < 0)
		return;

	xSemaphoreTake(send_mutex, portMAX_DELAY);
	send(sock_fd, enc, enc_len, 0);
	xSemaphoreGive(send_mutex);

	status_led_notify_traffic();
}

bool system_halow_connected( void ) {
	return sock_fd >= 0;
}

// transport tasks

static void ble_rx_task( void *arg ) {
	(void)arg;
	for (;;) {
		uint8_t b = (uint8_t)ble_read();
		kiss_rx_byte(&kiss, b);
	}
}

/* Constrain the radio's own TCP bridge so only the ESP32 (192.168.7.1 on
 * the SLIP link) may connect to it directly. External hosts must go through
 * the ESP32-side rns_proxy instead. Sent right after the HaLow link comes
 * up; best-effort, fire-and-forget -- a failure here does not tear down the
 * radio socket. The radio returns the updated cfg as JSON; we read and
 * discard it. */
static void radio_lock_tcp_whitelist(void) {
	static const char *body =
		"{\"enable\":true,\"port\":4242,\"whitelist\":\"192.168.7.1/32\"}";
	esp_http_client_config_t cfg = {
		.host = HALOW_WEB_HOST,
		.port = HALOW_WEB_PORT,
		.path = "/api/tcp_server_cfg",
		.method = HTTP_METHOD_POST,
		.transport_type = HTTP_TRANSPORT_OVER_TCP,
		.timeout_ms = 3000,
	};
	esp_http_client_handle_t c = esp_http_client_init(&cfg);
	if (!c) {
		ESP_LOGW(TAG, "tcp_server_cfg: client init failed");
		return;
	}
	esp_http_client_set_header(c, "Content-Type", "application/json");
	esp_http_client_set_post_field(c, body, strlen(body));
	esp_err_t err = esp_http_client_open(c, strlen(body));
	if (err == ESP_OK) {
		int wr = esp_http_client_write(c, body, strlen(body));
		if (wr >= 0)
			esp_http_client_fetch_headers(c);
		int code = esp_http_client_get_status_code(c);
		ESP_LOGI(TAG, "radio tcp_server_cfg whitelist=192.168.7.1/32 -> HTTP %d", code);
	} else {
		ESP_LOGW(TAG, "tcp_server_cfg open failed: %s", esp_err_to_name(err));
	}
	esp_http_client_cleanup(c);
}

static void halow_link_task( void *arg ) {
	(void)arg;

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = PP_HTONS(HALOW_PORT);
	addr.sin_addr.s_addr = SLIP_PEER_IP;

	uint8_t rx_buf[MAX_PACKET];

	for (;;) {
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}

		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			close(fd);
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}

		ESP_LOGI(TAG, "halow connected");

		radio_lock_tcp_whitelist();

		rns_framing_rx_reset(&rns);

		xSemaphoreTake(send_mutex, portMAX_DELAY);
		sock_fd = fd;
		xSemaphoreGive(send_mutex);

		for (;;) {
			int n = recv(fd, rx_buf, sizeof(rx_buf), 0);
			if (n <= 0)
				break;
			for (int i = 0; i < n; i++)
				rns_framing_rx_byte(&rns, rx_buf[i]);
		}

		xSemaphoreTake(send_mutex, portMAX_DELAY);
		sock_fd = -1;
		xSemaphoreGive(send_mutex);

		close(fd);
		ESP_LOGW(TAG, "halow disconnected, retrying");
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

void system_run( void *arg ) {
	(void)arg;

	ESP_LOGI(TAG, "system task started");

	send_mutex = xSemaphoreCreateMutex();

	kiss_init(&kiss, on_kiss_tx, NULL);
	kiss_set_data_callback(&kiss, on_kiss_data, NULL);

	rns_framing_init(&rns, on_rns_frame, &kiss);

	/* Central packet switch: BLE <-> TCP <-> radio fan-out. Must come up
	 * before halow_link_task / rns_proxy so any packets arriving early are
	 * not lost (queues just sit empty until a sink is registered). */
	switch_init();
	switch_register_ble_sink(ble_sink);
	switch_register_halow_sink(halow_sink);

	thalow_config_init();

	if (thalow_config_get_ble_enabled()) {
		ble_init();
		xTaskCreate(ble_rx_task, "ble_rx", 4096, NULL, 5, NULL);
	} else {
		ESP_LOGI(TAG, "BLE disabled by config");
	}

	status_led_init();
	button_set_callback(on_button_event);
	button_init();
	battery_init();
	/* SLIP must come up BEFORE halow_link_task: the radio link task
	 * connects to 192.168.7.2:4242 over the SLIP netif, so starting it
	 * before slip_init() just makes the first connect attempt fail and
	 * retry every 1 s. The old 200 ms delay was a fragile workaround. */
	slip_init();
	xTaskCreate(halow_link_task, "halow_link", 4096, NULL, 5, NULL);
	wifi_init();
	web_proxy_init();
	rns_proxy_init();

	/* Battery indication cadence, matching official RNode firmware
	 * (Power.h pushes kiss_indicate_battery() every 5 s, unsolicited). */
	uint32_t battery_last_push = 0;

	for (;;) {
		uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

		if (pairing_active) {
			if (ble_get_state() != BLE_STATE_PAIRING) {
				pairing_active = false;
			} else if ((now - pairing_started_ms) >= BLE_PAIRING_TIMEOUT) {
				ble_disable_pairing();
				pairing_active = false;
			}
		}

		if ((now - battery_last_push) >= 5000) {
			battery_last_push = now;
			kiss_send_battery(&kiss, battery_get_state(),
			                  battery_get_percent(),
			                  battery_get_voltage_mv());
		}

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
