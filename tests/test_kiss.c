/*
 * test_kiss.c — Unit tests for KISS TNC framing.
 *
 * Compiles the REAL src/kiss.c (against the FreeRTOS host stubs) instead of
 * an inlined copy, so the tests cannot drift from the firmware. The kiss
 * module needs the mutex shim from tests/stub; pass -I. -Istub -I../inc.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "kiss.h"
#include "rns_framing.h"

/* Commands the firmware itself defines (mirrors src/kiss.c). */
#define FEND 0xC0
#define CMD_DATA 0x00
#define CMD_FW_VERSION 0x50
#define FW_MAJOR 0x01
#define FW_MINOR 0x59
#define CMD_STAT_BAT 0x27

static int tests_run = 0, tests_pass = 0, tests_fail = 0;

typedef struct { uint8_t data[2200]; size_t len; int count; } rx_t;
static void data_cb(void *user, const uint8_t *data, size_t len) {
	rx_t *r = (rx_t*)user;
	memcpy(r->data, data, len); r->len = len; r->count++;
}
typedef struct { uint8_t data[2200]; size_t len; int count; } tx_t;
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
	uint8_t payload[] = {FEND, 0xDB, FEND, 0x41, 0xDB, 0xDB, FEND};
	kiss_send_data(&k, payload, sizeof(payload));
	for (size_t i = 0; i < tx.len; i++) kiss_rx_byte(&k, tx.data[i]);
	if (rx.len != sizeof(payload) || memcmp(rx.data, payload, sizeof(payload)) != 0) {
		printf("FAIL (mismatch len=%zu)\n", rx.len); tests_fail++; return;
	}
	tests_pass++; printf("OK\n");
}
static void test_full_rns_payload(void) {
	/* Regression for the off-by-one: rx_frame holds the command byte PLUS
	 * the payload, so with KISS_FRAME_MAX=1024 a full RNS payload of
	 * exactly RNS_FRAMING_MAX_PACKET (1024) bytes used to lose its last
	 * byte. It must now round-trip whole. */
	printf("  [TEST] KISS full 1024-byte RNS payload ... "); tests_run++;
	kiss_t k; tx_t tx = {0}; rx_t rx = {0};
	kiss_init(&k, tx_cb, &tx);
	kiss_set_data_callback(&k, data_cb, &rx);
	static uint8_t payload[RNS_FRAMING_MAX_PACKET];
	for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i & 0xFF);
	kiss_send_data(&k, payload, sizeof(payload));
	if (tx.len > KISS_FRAME_MAX * 2 + 8) { printf("FAIL (tx.len=%zu exceeds buffer)\n", tx.len); tests_fail++; return; }
	for (size_t i = 0; i < tx.len; i++) kiss_rx_byte(&k, tx.data[i]);
	if (rx.len != sizeof(payload)) { printf("FAIL (rx.len=%zu want %zu)\n", rx.len, sizeof(payload)); tests_fail++; return; }
	if (memcmp(rx.data, payload, sizeof(payload)) != 0) { printf("FAIL (mismatch)\n"); tests_fail++; return; }
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
static void test_rx_overflow_drops_whole_frame(void) {
	/* An oversized frame must be dropped WHOLE and reception must
	 * resynchronize at the next FEND: a silently truncated frame corrupts
	 * downstream parsing (KISS has no CRC). */
	printf("  [TEST] KISS RX overflow drops whole frame + resync ... "); tests_run++;
	kiss_t k; rx_t rx = {0};
	kiss_init(&k, NULL, NULL);
	kiss_set_data_callback(&k, data_cb, &rx);
	kiss_rx_byte(&k, FEND);
	kiss_rx_byte(&k, CMD_DATA);
	for (int i = 0; i < KISS_FRAME_MAX + 200; i++) kiss_rx_byte(&k, 0x41);
	kiss_rx_byte(&k, FEND);
	if (rx.count != 0) { printf("FAIL (oversized frame was delivered, count=%d)\n", rx.count); tests_fail++; return; }
	if (k.rx_dropped != 1) { printf("FAIL (rx_dropped=%u)\n", (unsigned)k.rx_dropped); tests_fail++; return; }
	/* The NEXT frame, within the size limit, must be received intact. */
	uint8_t ok[] = {1, 2, 3, 4, 5};
	kiss_rx_byte(&k, FEND);
	kiss_rx_byte(&k, CMD_DATA);
	for (size_t i = 0; i < sizeof(ok); i++) kiss_rx_byte(&k, ok[i]);
	kiss_rx_byte(&k, FEND);
	if (rx.count != 1) { printf("FAIL (post-resync frame missing, count=%d)\n", rx.count); tests_fail++; return; }
	if (rx.len != sizeof(ok) || memcmp(rx.data, ok, sizeof(ok)) != 0) { printf("FAIL (post-resync mismatch)\n"); tests_fail++; return; }
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
static void test_concurrent_scratch(void) {
	/* Two instances share the static TX scratch (guarded by a mutex).
	 * Interleaved sends must not corrupt either frame. */
	printf("  [TEST] KISS shared TX scratch integrity ... "); tests_run++;
	kiss_t a, b; tx_t txa = {0}, txb = {0};
	kiss_init(&a, tx_cb, &txa);
	kiss_init(&b, tx_cb, &txb);
	static uint8_t pa[600], pb[50];
	for (int i = 0; i < 600; i++) pa[i] = (uint8_t)(i * 7);
	for (int i = 0; i < 50; i++) pb[i] = (uint8_t)(i * 13 + 1);
	kiss_send_data(&a, pa, sizeof(pa));
	/* Immediately overwrite the scratch via the second instance; the first
	 * frame must already have been handed to its callback unchanged. */
	kiss_send_data(&b, pb, sizeof(pb));
	int ok_a = 1, ok_b = 1;
	rx_t ra = {0}, rb = {0};
	/* Re-parse each emitted wire frame through a fresh parser. */
	kiss_t pa2, pb2;
	kiss_init(&pa2, NULL, NULL); kiss_set_data_callback(&pa2, data_cb, &ra);
	kiss_init(&pb2, NULL, NULL); kiss_set_data_callback(&pb2, data_cb, &rb);
	for (size_t i = 0; i < txa.len; i++) kiss_rx_byte(&pa2, txa.data[i]);
	for (size_t i = 0; i < txb.len; i++) kiss_rx_byte(&pb2, txb.data[i]);
	if (ra.len != sizeof(pa) || memcmp(ra.data, pa, sizeof(pa)) != 0) ok_a = 0;
	if (rb.len != sizeof(pb) || memcmp(rb.data, pb, sizeof(pb)) != 0) ok_b = 0;
	if (!ok_a || !ok_b) { printf("FAIL (a=%d b=%d)\n", ok_a, ok_b); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}

static void test_battery_frame(void) {
	/* CMD_STAT_BAT (0x27) must satisfy two client conventions at once:
	 * stock RNS/Sideband read payload[0]=state, payload[1]=percent; Columba
	 * keeps the LAST payload byte as percent. Voltage rides in between in
	 * 10 mV units big-endian. Percent must clamp to 100. */
	printf("  [TEST] KISS battery frame (RNS + Columba compatible) ... "); tests_run++;
	kiss_t k; tx_t tx = {0};
	kiss_init(&k, tx_cb, &tx);
	kiss_send_battery(&k, 0x01, 72, 3940);
	uint8_t want[] = {FEND, CMD_STAT_BAT, 0x01, 72, 0x01, 0x8A, 72, FEND};
	if (tx.count != 1 || tx.len != sizeof(want) || memcmp(tx.data, want, sizeof(want)) != 0) {
		printf("FAIL (len=%zu)\n", tx.len); tests_fail++; return;
	}
	/* Clamp + escape: percent 150 -> 100; voltage 4750 mV -> dv 475
	 * = 0x01DB, low byte 0xDB must be escaped as FESC TFESC. */
	tx_t tx2 = {0};
	kiss_init(&k, tx_cb, &tx2);
	kiss_send_battery(&k, 0x02, 150, 4750);
	uint8_t want2[] = {FEND, CMD_STAT_BAT, 0x02, 100, 0x01, 0xDB, 0xDD, 100, FEND};
	if (tx2.len != sizeof(want2) || memcmp(tx2.data, want2, sizeof(want2)) != 0) {
		printf("FAIL (len=%zu)\n", tx2.len); tests_fail++; return;
	}
	/* Simulate both real parsers over the wire bytes. */
	tx_t tx3 = {0};
	kiss_init(&k, tx_cb, &tx3);
	kiss_send_battery(&k, 0x01, 84, 4020);
	int escape = 0; int cmd = -1; uint8_t buf[16]; int blen = 0;
	int rns_ok = 0, columba_bat = -1;
	for (size_t i = 0; i < tx3.len; i++) {
		uint8_t b = tx3.data[i];
		if (b == FEND) { cmd = -1; blen = 0; escape = 0; continue; }
		if (cmd == -1) { cmd = b; continue; }
		if (b == 0xDB) { escape = 1; continue; }
		if (escape) {
			if (b == 0xDC) b = FEND;
			else if (b == 0xDD) b = 0xDB;
			escape = 0;
		}
		if (blen < (int)sizeof(buf)) buf[blen++] = b;
		if (cmd == CMD_STAT_BAT) {
			/* RNS readLoop: sets state/percent exactly at payload len 2 */
			if (blen == 2 && buf[0] == 0x01 && buf[1] == 84)
				rns_ok = 1;
			/* Columba: r_stat_bat = every payload byte (last one wins) */
			columba_bat = b;
		}
	}
	if (!rns_ok) { printf("FAIL (rns view)\n"); tests_fail++; return; }
	if (columba_bat != 84) { printf("FAIL (columba view pct=%d)\n", columba_bat); tests_fail++; return; }
	tests_pass++; printf("OK\n");
}

int main(void) {
	printf("=== KISS framing unit tests (real src/kiss.c) ===\n");
	test_data_roundtrip();
	test_escape_roundtrip();
	test_full_rns_payload();
	test_all_fend_expansion();
	test_rx_overflow_drops_whole_frame();
	test_cmd_echo();
	test_concurrent_scratch();
	test_battery_frame();
	printf("\n=== Results: %d/%d passed, %d failed ===\n",
	       tests_pass, tests_run, tests_fail);
	return tests_fail > 0 ? 1 : 0;
}
