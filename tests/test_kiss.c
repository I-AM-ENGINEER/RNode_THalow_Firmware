/*
 * test_kiss.c — Unit tests for KISS TNC framing.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define KISS_FRAME_MAX 1024

/* ---- kiss module inlined ---- */
#define FEND 0xC0
#define FESC 0xDB
#define TFEND 0xDC
#define TFESC 0xDD
#define CMD_DATA 0x00
#define CMD_FW_VERSION 0x50
#define FW_MAJOR 0x01
#define FW_MINOR 0x59

typedef void (*kiss_tx_cb)(void *user, const uint8_t *buf, size_t len);
typedef void (*kiss_data_cb)(void *user, const uint8_t *data, size_t len);
typedef struct kiss {
	kiss_tx_cb tx_cb; void *tx_user;
	kiss_data_cb data_cb; void *data_user;
	uint8_t rx_frame[KISS_FRAME_MAX]; int rx_len;
	bool in_frame; bool escape;
} kiss_t;

static void send_kiss(kiss_t *k, uint8_t cmd, const uint8_t *data, int len) {
	uint8_t buf[KISS_FRAME_MAX * 2 + 8];
	int pos = 0;
	buf[pos++] = FEND;
	buf[pos++] = cmd;
	for (int i = 0; i < len; i++) {
		if (data[i] == FEND) { buf[pos++] = FESC; buf[pos++] = TFEND; }
		else if (data[i] == FESC) { buf[pos++] = FESC; buf[pos++] = TFESC; }
		else { buf[pos++] = data[i]; }
	}
	buf[pos++] = FEND;
	if (k->tx_cb) k->tx_cb(k->tx_user, buf, pos);
}
static void handle_frame(kiss_t *k, const uint8_t *frame, int len) {
	if (len < 1) return;
	uint8_t cmd = frame[0];
	if (cmd == CMD_DATA) {
		if (k->data_cb) k->data_cb(k->data_user, frame + 1, len - 1);
		return;
	}
	if (cmd == CMD_FW_VERSION) {
		uint8_t v[2] = { FW_MAJOR, FW_MINOR };
		send_kiss(k, CMD_FW_VERSION, v, 2);
		return;
	}
	/* Generic echo for other commands */
	send_kiss(k, cmd, frame + 1, len - 1);
}
static void kiss_init(kiss_t *k, kiss_tx_cb tx_cb, void *tx_user) {
	k->tx_cb = tx_cb; k->tx_user = tx_user;
	k->data_cb = NULL; k->data_user = NULL;
	k->rx_len = 0; k->in_frame = false; k->escape = false;
}
static void kiss_set_data_callback(kiss_t *k, kiss_data_cb cb, void *user) {
	k->data_cb = cb; k->data_user = user;
}
static void kiss_rx_byte(kiss_t *k, uint8_t b) {
	if (b == FEND) {
		if (k->in_frame && k->rx_len > 0)
			handle_frame(k, k->rx_frame, k->rx_len);
		k->in_frame = true; k->escape = false; k->rx_len = 0;
		return;
	}
	if (!k->in_frame) return;
	if (b == FESC) { k->escape = true; return; }
	if (k->escape) {
		if (b == TFEND) b = FEND;
		else if (b == TFESC) b = FESC;
		k->escape = false;
	}
	if (k->rx_len < KISS_FRAME_MAX) k->rx_frame[k->rx_len++] = b;
}
static void kiss_send_data(kiss_t *k, const uint8_t *data, size_t len) {
	send_kiss(k, CMD_DATA, data, len);
}
/* ---- end kiss module ---- */

static int tests_run = 0, tests_pass = 0, tests_fail = 0;

typedef struct { uint8_t data[2048]; size_t len; int count; } rx_t;
static void data_cb(void *user, const uint8_t *data, size_t len) {
	rx_t *r = (rx_t*)user;
	memcpy(r->data, data, len); r->len = len; r->count++;
}
typedef struct { uint8_t data[2048]; size_t len; int count; } tx_t;
static void tx_cb(void *user, const uint8_t *buf, size_t len) {
	tx_t *t = (tx_t*)user;
	memcpy(t->data, buf, len); t->len = len; t->count++;
}

static void test_data_roundtrip(void) {
	printf("  [TEST] KISS data round-trip ... "); tests_run++;
	kiss_t k; tx_t tx = {0}; rx_t rx = {0};
	kiss_init(&k, tx_cb, &tx);
	kiss_set_data_callback(&k, data_cb, &rx);
	uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	kiss_send_data(&k, payload, sizeof(payload));
	if (tx.count != 1) { printf("FAIL (tx.count=%d)\n", tx.count); tests_fail++; return; }
	for (size_t i = 0; i < tx.len; i++) kiss_rx_byte(&k, tx.data[i]);
	if (rx.count != 1) { printf("FAIL (rx.count=%d)\n", rx.count); tests_fail++; return; }
	if (rx.len != sizeof(payload)) { printf("FAIL (rx.len=%zu)\n", rx.len); tests_fail++; return; }
	if (memcmp(rx.data, payload, sizeof(payload)) != 0) { printf("FAIL (mismatch)\n"); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}
static void test_escape_roundtrip(void) {
	printf("  [TEST] KISS FEND/FESC escape round-trip ... "); tests_run++;
	kiss_t k; tx_t tx = {0}; rx_t rx = {0};
	kiss_init(&k, tx_cb, &tx);
	kiss_set_data_callback(&k, data_cb, &rx);
	uint8_t payload[] = {FEND, FESC, FEND, 0x41, FESC, FESC, FEND};
	kiss_send_data(&k, payload, sizeof(payload));
	for (size_t i = 0; i < tx.len; i++) kiss_rx_byte(&k, tx.data[i]);
	if (rx.len != sizeof(payload) || memcmp(rx.data, payload, sizeof(payload)) != 0) {
		printf("FAIL (mismatch len=%zu)\n", rx.len); tests_fail++; return;
	}
	tests_pass++; printf("OK\n");
}
static void test_max_payload(void) {
	/* NOTE: rx_frame[KISS_FRAME_MAX] stores CMD_DATA(1) + payload. So the
	 * maximum round-trippable DATA payload is KISS_FRAME_MAX-1 = 1023 bytes.
	 * This documents a real firmware limitation: an RNS payload of exactly
	 * 1024 bytes loses its last byte in the KISS RX path because the cmd
	 * byte consumes one slot. See audit finding. */
	printf("  [TEST] KISS max-1 data frame (1023 bytes) ... "); tests_run++;
	kiss_t k; tx_t tx = {0}; rx_t rx = {0};
	kiss_init(&k, tx_cb, &tx);
	kiss_set_data_callback(&k, data_cb, &rx);
	int maxlen = KISS_FRAME_MAX - 1;
	uint8_t payload[KISS_FRAME_MAX];
	for (int i = 0; i < maxlen; i++) payload[i] = (uint8_t)(i & 0xFF);
	kiss_send_data(&k, payload, maxlen);
	if (tx.len > KISS_FRAME_MAX * 2 + 8) { printf("FAIL (tx.len=%zu exceeds buffer)\n", tx.len); tests_fail++; return; }
	for (size_t i = 0; i < tx.len; i++) kiss_rx_byte(&k, tx.data[i]);
	if (rx.len != (size_t)maxlen) { printf("FAIL (rx.len=%zu want %d)\n", rx.len, maxlen); tests_fail++; return; }
	if (memcmp(rx.data, payload, maxlen) != 0) { printf("FAIL (mismatch)\n"); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}
static void test_all_fend_expansion(void) {
	printf("  [TEST] KISS worst-case all-FEND payload ... "); tests_run++;
	kiss_t k; tx_t tx = {0};
	kiss_init(&k, tx_cb, &tx);
	uint8_t payload[256]; memset(payload, FEND, sizeof(payload));
	kiss_send_data(&k, payload, sizeof(payload));
	if (tx.len != 515) { printf("FAIL (tx.len=%zu expected 515)\n", tx.len); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}
static void test_rx_overflow(void) {
	printf("  [TEST] KISS RX overflow truncates ... "); tests_run++;
	kiss_t k; rx_t rx = {0};
	kiss_init(&k, NULL, NULL);
	kiss_set_data_callback(&k, data_cb, &rx);
	kiss_rx_byte(&k, FEND);
	kiss_rx_byte(&k, CMD_DATA);
	for (int i = 0; i < KISS_FRAME_MAX + 200; i++) kiss_rx_byte(&k, 0x41);
	kiss_rx_byte(&k, FEND);
	if (rx.count != 1) { printf("FAIL (count=%d)\n", rx.count); tests_fail++; return; }
	if (rx.len > KISS_FRAME_MAX - 1) { printf("FAIL (rx.len=%zu > MAX-1)\n", rx.len); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}
static void test_cmd_echo(void) {
	printf("  [TEST] KISS CMD_FW_VERSION echo ... "); tests_run++;
	kiss_t k; tx_t tx = {0};
	kiss_init(&k, tx_cb, &tx);
	uint8_t req[] = {FEND, CMD_FW_VERSION, FEND};
	for (int i = 0; i < 3; i++) kiss_rx_byte(&k, req[i]);
	if (tx.count != 1) { printf("FAIL (tx.count=%d)\n", tx.count); tests_fail++; return; }
	if (tx.len != 5) { printf("FAIL (tx.len=%zu expected 5)\n", tx.len); tests_fail++; return; }
	if (tx.data[1] != CMD_FW_VERSION) { printf("FAIL (cmd byte)\n"); tests_fail++; return; }
	if (tx.data[2] != FW_MAJOR || tx.data[3] != FW_MINOR) { printf("FAIL (version bytes)\n"); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}

int main(void) {
	printf("=== KISS framing unit tests ===\n");
	test_data_roundtrip();
	test_escape_roundtrip();
	test_max_payload();
	test_all_fend_expansion();
	test_rx_overflow();
	test_cmd_echo();
	printf("\n=== Results: %d/%d passed, %d failed ===\n",
	       tests_pass, tests_run, tests_fail);
	return tests_fail > 0 ? 1 : 0;
}
