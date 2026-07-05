#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "ping/ping_sock.h"

#include "slip.h"
#include "config/project_config.h"

#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD
#define SLIP_MTU      1500
#define SLIP_BUF_SIZE (SLIP_MTU + 100)

static const char *TAG = "slip";

static struct netif slip_netif;
static uint8_t txbuf[SLIP_BUF_SIZE * 2];

static err_t slip_output_v4( struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr ) {
	(void)netif;
	(void)ipaddr;

	int len = 0;
	txbuf[len++] = SLIP_END;

	for (struct pbuf *q = p; q != NULL; q = q->next) {
		for (int i = 0; i < q->len; i++) {
			uint8_t b = ((uint8_t *)q->payload)[i];
			if (b == SLIP_END) {
				txbuf[len++] = SLIP_ESC;
				txbuf[len++] = SLIP_ESC_END;
			} else if (b == SLIP_ESC) {
				txbuf[len++] = SLIP_ESC;
				txbuf[len++] = SLIP_ESC_ESC;
			} else {
				txbuf[len++] = b;
			}
		}
	}

	txbuf[len++] = SLIP_END;
	uart_write_bytes(RADIO_UART_PORT, txbuf, len);

	return ERR_OK;
}

static err_t slip_netif_init( struct netif *netif ) {
	netif->output = slip_output_v4;
	netif->name[0] = 's';
	netif->name[1] = 'l';
	netif->mtu = SLIP_MTU;
	netif->flags = NETIF_FLAG_LINK_UP;
	return ERR_OK;
}

static void slip_rx_task( void *arg ) {
	struct netif *netif = (struct netif *)arg;
	uint8_t rxbuf[SLIP_BUF_SIZE];
	int rxlen = 0;
	bool escaped = false;
	uint8_t byte;

	for (;;) {
		int n = uart_read_bytes(RADIO_UART_PORT, &byte, 1, portMAX_DELAY);
		if (n <= 0)
			continue;

		switch (byte) {
		case SLIP_END:
			if (rxlen > 0) {
				struct pbuf *p = pbuf_alloc(PBUF_RAW, rxlen, PBUF_POOL);
				if (p) {
					pbuf_take(p, rxbuf, rxlen);
					if (netif->input(p, netif) != ERR_OK)
						pbuf_free(p);
				}
				rxlen = 0;
			}
			escaped = false;
			break;
		case SLIP_ESC:
			escaped = true;
			break;
		default:
			if (escaped) {
				if (byte == SLIP_ESC_END)
					byte = SLIP_END;
				else if (byte == SLIP_ESC_ESC)
					byte = SLIP_ESC;
				escaped = false;
			}
			if (rxlen < SLIP_BUF_SIZE)
				rxbuf[rxlen++] = byte;
			break;
		}
	}
}

static void on_ping_success( esp_ping_handle_t hdl, void *args ) {
	(void)args;
	uint32_t elapsed, recv_len;
	uint8_t ttl;
	ip_addr_t addr;
	esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
	esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
	esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
	esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
	ESP_LOGI(TAG, "reply from %s: bytes=%lu time=%lums ttl=%u",
	         ipaddr_ntoa(&addr), (unsigned long)recv_len,
	         (unsigned long)elapsed, ttl);
}

static void on_ping_timeout( esp_ping_handle_t hdl, void *args ) {
	(void)hdl;
	(void)args;
	ESP_LOGW(TAG, "ping timeout");
}

static void on_ping_end( esp_ping_handle_t hdl, void *args ) {
	(void)args;
	uint32_t transmitted, received;
	esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
	esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
	ESP_LOGI(TAG, "ping done: sent=%lu recv=%lu",
	         (unsigned long)transmitted, (unsigned long)received);
}

void slip_init( void ) {
	uart_config_t uart_config = {
		.baud_rate  = RADIO_UART_BAUDRATE,
		.data_bits  = UART_DATA_8_BITS,
		.parity     = UART_PARITY_DISABLE,
		.stop_bits  = UART_STOP_BITS_1,
		.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	uart_param_config(RADIO_UART_PORT, &uart_config);
	uart_set_pin(RADIO_UART_PORT, RADIO_UART_TX_PIN, RADIO_UART_RX_PIN,
	             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_driver_install(RADIO_UART_PORT, SLIP_BUF_SIZE * 2, SLIP_BUF_SIZE * 2, 0, NULL, 0);

	ip4_addr_t ipaddr, netmask, gw;
	ip4_addr_set_u32(&ipaddr,  SLIP_LOCAL_IP);
	ip4_addr_set_u32(&netmask, SLIP_NETMASK);
	ip4_addr_set_u32(&gw,      SLIP_LOCAL_IP);

	netif_add(&slip_netif, &ipaddr, &netmask, &gw, NULL,
	          slip_netif_init, tcpip_input);
	netif_set_default(&slip_netif);
	netif_set_link_up(&slip_netif);
	netif_set_up(&slip_netif);

	xTaskCreate(slip_rx_task, "slip_rx", 4096, &slip_netif, 5, NULL);

	ESP_LOGI(TAG, "SLIP up on UART%d: %s/30",
	         RADIO_UART_PORT, ip4addr_ntoa(&ipaddr));
}

void slip_start_ping( void ) {
	esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
	ip_addr_set_ip4_u32(&config.target_addr, SLIP_PEER_IP);
	config.count = 0;
	config.interval_ms = 1000;
	config.timeout_ms = 1000;
	config.data_size = 56;

	esp_ping_callbacks_t cbs = {
		.on_ping_success = on_ping_success,
		.on_ping_timeout = on_ping_timeout,
		.on_ping_end     = on_ping_end,
		.cb_args         = NULL,
	};

	esp_ping_handle_t ping;
	esp_ping_new_session(&config, &cbs, &ping);
	esp_ping_start(ping);

	ESP_LOGI(TAG, "pinging %s", ipaddr_ntoa(&config.target_addr));
}
