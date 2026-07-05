#include <string.h>
#include <stdbool.h>

#include "kiss.h"

#define FEND              0xC0
#define FESC              0xDB
#define TFEND             0xDC
#define TFESC             0xDD

#define CMD_DATA          0x00
#define CMD_RADIO_STATE   0x06
#define CMD_DETECT        0x08
#define CMD_PROMISC       0x0E
#define CMD_READY         0x0F
#define CMD_BOARD         0x47
#define CMD_PLATFORM      0x48
#define CMD_MCU           0x49
#define CMD_FW_VERSION    0x50
#define CMD_STAT_RX       0x21
#define CMD_STAT_TX       0x22
#define CMD_BLINK         0x30

#define DETECT_REQ        0x73
#define DETECT_RESP       0x46
#define RADIO_ON          0x01

#define PLATFORM_ESP32    0x80
#define MCU_ESP32         0x81
#define BOARD_MODEL       0x42
#define FW_MAJOR          0x01
#define FW_MINOR          0x00

static void send_kiss( kiss_t *k, uint8_t cmd, const uint8_t *data, int len ) {
	uint8_t buf[KISS_FRAME_MAX + 8];
	int pos = 0;

	buf[pos++] = FEND;
	buf[pos++] = cmd;

	for (int i = 0; i < len; i++) {
		if (data[i] == FEND) {
			buf[pos++] = FESC;
			buf[pos++] = TFEND;
		} else if (data[i] == FESC) {
			buf[pos++] = FESC;
			buf[pos++] = TFESC;
		} else {
			buf[pos++] = data[i];
		}
	}

	buf[pos++] = FEND;
	if (k->tx_cb)
		k->tx_cb(k->tx_user, buf, pos);
}

static void handle_frame( kiss_t *k, const uint8_t *frame, int len ) {
	if (len < 1)
		return;

	uint8_t cmd = frame[0];
	uint8_t val = (len >= 2) ? frame[1] : 0;

	switch (cmd) {
	case CMD_DATA:
		if (k->data_cb)
			k->data_cb(k->data_user, frame + 1, len - 1);
		break;

	case CMD_DETECT:
		if (val == DETECT_REQ) {
			uint8_t resp = DETECT_RESP;
			send_kiss(k, CMD_DETECT, &resp, 1);
		}
		break;

	case CMD_PLATFORM: {
		uint8_t p = PLATFORM_ESP32;
		send_kiss(k, CMD_PLATFORM, &p, 1);
		break;
	}

	case CMD_MCU: {
		uint8_t m = MCU_ESP32;
		send_kiss(k, CMD_MCU, &m, 1);
		break;
	}

	case CMD_BOARD: {
		uint8_t b = BOARD_MODEL;
		send_kiss(k, CMD_BOARD, &b, 1);
		break;
	}

	case CMD_FW_VERSION: {
		uint8_t v[2] = { FW_MAJOR, FW_MINOR };
		send_kiss(k, CMD_FW_VERSION, v, 2);
		break;
	}

	case CMD_RADIO_STATE: {
		uint8_t state = (val == RADIO_ON) ? 0x01 : 0x00;
		send_kiss(k, CMD_RADIO_STATE, &state, 1);
		if (val == RADIO_ON) {
			uint8_t ready = 0x01;
			send_kiss(k, CMD_READY, &ready, 1);
		}
		break;
	}

	case CMD_READY: {
		uint8_t ready = 0x01;
		send_kiss(k, CMD_READY, &ready, 1);
		break;
	}

	case CMD_PROMISC: {
		uint8_t p = val;
		send_kiss(k, CMD_PROMISC, &p, 1);
		break;
	}

	case CMD_STAT_RX: {
		uint8_t s[4] = { 0, 0, 0, 0 };
		send_kiss(k, CMD_STAT_RX, s, 4);
		break;
	}

	case CMD_STAT_TX: {
		uint8_t s[4] = { 0, 0, 0, 0 };
		send_kiss(k, CMD_STAT_TX, s, 4);
		break;
	}

	default:
		break;
	}
}

void kiss_init( kiss_t *k, kiss_tx_cb tx_cb, void *tx_user ) {
	k->tx_cb   = tx_cb;
	k->tx_user = tx_user;
	k->data_cb = NULL;
	k->data_user = NULL;
	k->rx_len   = 0;
	k->in_frame = false;
	k->escape   = false;
}

void kiss_set_data_callback( kiss_t *k, kiss_data_cb cb, void *user ) {
	k->data_cb   = cb;
	k->data_user = user;
}

void kiss_rx_byte( kiss_t *k, uint8_t b ) {
	if (!k)
		return;

	if (b == FEND) {
		if (k->in_frame && k->rx_len > 0)
			handle_frame(k, k->rx_frame, k->rx_len);
		k->in_frame = true;
		k->escape = false;
		k->rx_len = 0;
		return;
	}

	if (!k->in_frame)
		return;

	if (b == FESC) {
		k->escape = true;
		return;
	}

	if (k->escape) {
		if (b == TFEND) b = FEND;
		else if (b == TFESC) b = FESC;
		k->escape = false;
	}

	if (k->rx_len < KISS_FRAME_MAX)
		k->rx_frame[k->rx_len++] = b;
}

void kiss_send_data( kiss_t *k, const uint8_t *data, size_t len ) {
	send_kiss(k, CMD_DATA, data, len);
}
