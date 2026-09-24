#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kiss.h"

#define FEND              0xC0
#define FESC              0xDB
#define TFEND             0xDC
#define TFESC             0xDD

#define CMD_DATA          0x00
#define CMD_FREQUENCY     0x01
#define CMD_BANDWIDTH     0x02
#define CMD_TXPOWER       0x03
#define CMD_SF            0x04
#define CMD_CR            0x05
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
#define CMD_STAT_BAT      0x27
#define CMD_BLINK         0x30

#define DETECT_REQ        0x73
#define DETECT_RESP       0x46
#define RADIO_ON          0x01

#define PLATFORM_ESP32    0x80
#define MCU_ESP32_S3      0x87   /* ESP32-S3 (markqvist RNS: MCU_ESP32_S3) */
#define BOARD_MODEL       0x42
/* RNS REQUIRED_FW_VER_MAJ=1, REQUIRED_FW_VER_MIN=52 (0x34). A value below
 * this is HARD-REJECTED by RNS Python (RNodeInterface.detect raises IOError),
 * Sideband, and Columba-Python -- but NOT by Columba-Kotlin, which is why
 * only Kotlin worked. Report 1.89 (0x59), the current upstream RNode version,
 * to stay comfortably above the floor. See:
 * https://github.com/markqvist/Reticulum/blob/master/RNS/Interfaces/RNodeInterface.py
 * Decoding convention is fixed: byte[0]=major, byte[1]=minor, plain ints. */
#define FW_MAJOR          0x01
#define FW_MINOR          0x59

/* Frame-building scratch. send_kiss can be reached from two tasks (the
 * switch dispatch task via kiss_send_data, and the BLE RX task via the
 * config-echo path in handle_frame), so the scratch is static and guarded
 * by a mutex instead of a ~2 KB stack frame inside a 4 KB task. */
static uint8_t tx_scratch[KISS_FRAME_MAX * 2 + 8];
static SemaphoreHandle_t tx_scratch_lock;

static void send_kiss( kiss_t *k, uint8_t cmd, const uint8_t *data, int len ) {
	/* Worst-case frame size: every payload byte escaped (2 bytes each) +
	 * FEND + cmd + trailing FEND = 2*len + 4. Allocated generously. */
	if (tx_scratch_lock == NULL)
		tx_scratch_lock = xSemaphoreCreateMutex();

	xSemaphoreTake(tx_scratch_lock, portMAX_DELAY);

	int pos = 0;

	tx_scratch[pos++] = FEND;
	tx_scratch[pos++] = cmd;

	for (int i = 0; i < len; i++) {
		if (data[i] == FEND) {
			tx_scratch[pos++] = FESC;
			tx_scratch[pos++] = TFEND;
		} else if (data[i] == FESC) {
			tx_scratch[pos++] = FESC;
			tx_scratch[pos++] = TFESC;
		} else {
			tx_scratch[pos++] = data[i];
		}
	}

	tx_scratch[pos++] = FEND;
	if (k->tx_cb)
		k->tx_cb(k->tx_user, tx_scratch, pos);

	xSemaphoreGive(tx_scratch_lock);
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
	/* Radio-config echo handlers. This device has no real LoRa radio --
	 * the HaLow module is the actual RF path. But Reticulum's initRadio()
	 * sends frequency/bandwidth/txpower/SF/CR during connect and validateRadioState()
	 * rejects the interface unless the device echoes them back. Echo whatever
	 * the host sent so the validation passes. Stock RNS (desktop + Sideband)
	 * needs this; Columba skips the LoRa validation. */
	case CMD_FREQUENCY:
	case CMD_BANDWIDTH:
	case CMD_TXPOWER:
	case CMD_SF:
	case CMD_CR:
		send_kiss(k, cmd, frame + 1, len - 1);
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
		uint8_t m = MCU_ESP32_S3;
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
	k->rx_overflow = false;
	k->rx_dropped  = 0;
}

void kiss_set_data_callback( kiss_t *k, kiss_data_cb cb, void *user ) {
	k->data_cb   = cb;
	k->data_user = user;
}

void kiss_rx_byte( kiss_t *k, uint8_t b ) {
	if (!k)
		return;

	if (b == FEND) {
		if (k->rx_overflow) {
			/* Frame boundary reached: resume normal reception. The
			 * oversized frame is NOT delivered -- a silently truncated
			 * frame is worse than a dropped one (KISS has no CRC). */
			k->rx_overflow = false;
		} else if (k->in_frame && k->rx_len > 0) {
			handle_frame(k, k->rx_frame, k->rx_len);
		}
		k->in_frame = true;
		k->escape = false;
		k->rx_len = 0;
		return;
	}

	if (!k->in_frame)
		return;

	if (k->rx_overflow)
		return; /* discard until the next FEND */

	if (b == FESC) {
		k->escape = true;
		return;
	}

	if (k->escape) {
		if (b == TFEND) b = FEND;
		else if (b == TFESC) b = FESC;
		k->escape = false;
	}

	if (k->rx_len < KISS_FRAME_MAX) {
		k->rx_frame[k->rx_len++] = b;
	} else {
		k->rx_overflow = true;
		k->rx_dropped++;
		k->escape = false;
	}
}

void kiss_send_data( kiss_t *k, const uint8_t *data, size_t len ) {
	send_kiss(k, CMD_DATA, data, len);
}

void kiss_send_battery( kiss_t *k, uint8_t state, uint8_t percent,
                        int voltage_mv ) {
	/* Official RNode payload is [state, percent] (Framing.h CMD_STAT_BAT,
	 * Utilities.h kiss_indicate_battery); stock RNS/Sideband read exactly
	 * the first two payload bytes and ignore the rest. Columba's parser
	 * (columba_rnode_interface.py CMD_STAT_BAT) instead keeps the LAST
	 * payload byte as the percent. This frame satisfies both: percent sits
	 * at [1] for RNS and again at the tail for Columba, with the battery
	 * voltage in 10 mV units big-endian between them for future clients. */
	if (percent > 100)
		percent = 100;
	int dv = voltage_mv / 10;
	if (dv < 0)
		dv = 0;
	if (dv > 0xFFFF)
		dv = 0xFFFF;
	uint8_t p[5] = {
		state,
		percent,
		(uint8_t)((unsigned)dv >> 8),
		(uint8_t)((unsigned)dv & 0xFF),
		percent,
	};
	send_kiss(k, CMD_STAT_BAT, p, sizeof(p));
}
