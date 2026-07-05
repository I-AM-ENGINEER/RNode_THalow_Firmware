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
	system_halow_send(data, len);
}

static void on_kiss_tx( void *user, const uint8_t *buf, size_t len ) {
	(void)user;
	ble_write(buf, len);
	ble_flush();
}

// RNS framing <-> KISS callbacks

static void on_rns_frame( void *user, const uint8_t *data, size_t len ) {
	kiss_t *k = (kiss_t *)user;
	ESP_LOGI(TAG, "rns frame %d -> kiss", (int)len);
	kiss_send_data(k, data, len);
}

// Button -> pairing lifecycle

static void on_button_event( button_event_t event ) {
	if (event == BUTTON_EVENT_LONG_PRESS) {
		pairing_active = true;
		pairing_started_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
		ble_enable_pairing();
	}
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
}

// transport tasks

static void ble_rx_task( void *arg ) {
	(void)arg;
	for (;;) {
		uint8_t b = (uint8_t)ble_read();
		kiss_rx_byte(&kiss, b);
	}
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

	ble_init();
	status_led_init();
	button_set_callback(on_button_event);
	button_init();
	xTaskCreate(ble_rx_task, "ble_rx", 4096, NULL, 5, NULL);
	xTaskCreate(halow_link_task, "halow_link", 4096, NULL, 5, NULL);
	vTaskDelay(pdMS_TO_TICKS(200));
	slip_init();

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

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
