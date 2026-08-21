/*
 * test_main.c — Level-2 (QEMU ESP32-S3) Unity tests.
 *
 * Runs the same production sources as the host suite (Level 1) but with:
 *   - real xtensa esp32s3 codegen,
 *   - the real FreeRTOS scheduler and heap_caps allocator,
 *   - real PSRAM-backed ring allocation when QEMU provides SPIRAM.
 *
 * The runner prints a machine-readable final marker so run_qemu.py can
 * assert success without parsing fragile Unity text:
 *     QEMU_TESTS_RESULT PASS|FAIL
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "esp_log.h"

#include "rns_framing.h"
#include "kiss.h"
#include "gzip_inflate.h"
#include "switch.h"

#include "gzip_vectors.h"

static const char *TAG = "qemu_tests";

/* ------------------------------------------------------------------ */
/* rns_framing                                                          */
/* ------------------------------------------------------------------ */

TEST_CASE("rns_encode_size", "[rns]") {
	uint8_t payload[512];
	for (int i = 0; i < 512; i++)
		payload[i] = (uint8_t)(i * 13 + 7);

	uint8_t enc[RNS_FRAMING_MAX_PACKET * 2 + 4];
	int el = rns_framing_encode(payload, sizeof(payload), enc, sizeof(enc));
	/* pattern contains two control bytes (0x7D at i=78, 0x7E at i=147):
	 * frame = FEND + 510 plain + 2*2 escaped + FEND */
	/* 4 control bytes total: 13*i+7 hits 0x7D/0x7E at i=78,147,334,403 */
	TEST_ASSERT_EQUAL_INT(512 + 2 + 4, el);
}

/* Simple decode collector */
static struct { uint8_t buf[2048]; size_t len; int n; } s_dec;
static void dec_cb(void *user, const uint8_t *data, size_t len) {
	(void)user;
	s_dec.len = len < sizeof(s_dec.buf) ? len : sizeof(s_dec.buf);
	memcpy(s_dec.buf, data, s_dec.len);
	s_dec.n++;
}

TEST_CASE("rns_full_roundtrip", "[rns]") {
	uint8_t payload[] = { 0x7E, 0x01, 0x7D, 0x02, 0xFF };
	uint8_t enc[64];
	int el = rns_framing_encode(payload, sizeof(payload), enc, sizeof(enc));
	TEST_ASSERT_TRUE(el > 0);

	memset(&s_dec, 0, sizeof(s_dec));
	rns_framing_t f;
	rns_framing_init(&f, dec_cb, NULL);
	for (int i = 0; i < el; i++)
		rns_framing_rx_byte(&f, enc[i]);

	TEST_ASSERT_EQUAL_INT(1, s_dec.n);
	TEST_ASSERT_EQUAL_UINT32(sizeof(payload), s_dec.len);
	TEST_ASSERT_EQUAL_MEMORY(payload, s_dec.buf, sizeof(payload));
}

#define FEND 0xC0
#define FESC 0xDB

/* ------------------------------------------------------------------ */
/* kiss                                                                 */
/* ------------------------------------------------------------------ */

static struct { uint8_t buf[4096]; size_t len; int n; } s_tx;
static void tx_cb(void *user, const uint8_t *buf, size_t len) {
	(void)user;
	s_tx.len = len;
	memcpy(s_tx.buf, buf, len);
	s_tx.n++;
}

TEST_CASE("kiss_roundtrip", "[kiss]") {
	kiss_t k;
	memset(&s_tx, 0, sizeof(s_tx));
	memset(&s_dec, 0, sizeof(s_dec));   /* reset shared decode collector */
	kiss_init(&k, tx_cb, NULL);
	kiss_set_data_callback(&k, dec_cb, NULL);

	uint8_t payload[] = { FEND, 0xAA, FESC, 0xBB };
	kiss_send_data(&k, payload, sizeof(payload));
	TEST_ASSERT_EQUAL_INT(1, s_tx.n);
	TEST_ASSERT_TRUE(s_tx.len > sizeof(payload));

	for (size_t i = 0; i < s_tx.len; i++)
		kiss_rx_byte(&k, s_tx.buf[i]);
	TEST_ASSERT_EQUAL_INT(1, s_dec.n);
	TEST_ASSERT_EQUAL_UINT32(sizeof(payload), s_dec.len);
	TEST_ASSERT_EQUAL_MEMORY(payload, s_dec.buf, sizeof(payload));
}

/* ------------------------------------------------------------------ */
/* gzip_inflate                                                         */
/* ------------------------------------------------------------------ */

TEST_CASE("gzip_dynamic", "[gzip]") {
	size_t cap = vec_dynamic_src_len + 4096; /* heap: too big for .bss */
	uint8_t *dst = malloc(cap);
	TEST_ASSERT_NOT_NULL(dst);
	size_t got = 0;
	int rc = gzip_inflate(vec_dynamic, vec_dynamic_len, dst, cap, &got);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_UINT32(vec_dynamic_src_len, got);
	/* verify first / middle / last pattern bytes */
	const char *pat = "RNodeHaLowSwitch";
	{
		printf("DBG got=%zu dst0..7:", got);
		for (int i = 0; i < 8; i++)
			printf(" %02x", dst[i]);
		printf(" | pat0..7:");
		for (int i = 0; i < 8; i++)
			printf(" %02x", (uint8_t)pat[i]);
		}
	TEST_ASSERT_EQUAL_MEMORY(pat, dst, 16);
		{
		size_t off = got / 2;
		const char *full = "RNodeHaLowSwitchPacketRingBufferOverwrite";
		size_t plen = strlen(full);
		/* compare 16 bytes against the phase-shifted pattern */
		for (int k = 0; k < 16; k++) {
			uint8_t want = (uint8_t)full[(off + k) % plen];
			TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, dst[off + k],
			                                 "mid-stream mismatch");
		}
	}
	free(dst);
}

TEST_CASE("gzip_cap_reject", "[gzip]") {
	static uint8_t dst[1024];
	size_t got = 12345;
	int rc = gzip_inflate(vec_dynamic, vec_dynamic_len, dst,
	                      sizeof(dst), &got);
	TEST_ASSERT_EQUAL_INT(-1, rc);
	TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof(dst), got);
}

/* ------------------------------------------------------------------ */
/* switch — real FreeRTOS mutexes/queues under QEMU                     */
/* ------------------------------------------------------------------ */

static int s_ble_calls, s_tcp_calls, s_halow_calls;
static bool s_halow_ready;
static uint8_t s_last[64]; static size_t s_last_len;

static bool t_ble_sink(const uint8_t *d, size_t l) {
	s_ble_calls++; (void)d; (void)l; return true;
}
static bool t_tcp_sink(const uint8_t *d, size_t l) {
	s_tcp_calls++; (void)d; (void)l; return true;
}
static int s_halow_retries;
static bool t_halow_sink(const uint8_t *d, size_t l) {
	if (!s_halow_ready) { s_halow_retries++; return false; }
	s_halow_calls++;
	s_last_len = l < sizeof(s_last) ? l : sizeof(s_last);
	memcpy(s_last, d, s_last_len);
	return true;
}

TEST_CASE("switch_fanout_held", "[switch]") {
	switch_init();
	switch_register_ble_sink(t_ble_sink);
	switch_register_tcp_sink(t_tcp_sink);
	switch_register_halow_sink(t_halow_sink);

	const uint8_t msg[] = "radio-uplink-test";

	/* Radio link down: packet must be retained (held), never dropped. */
	s_halow_ready = false;
	switch_rx(SW_SRC_BLE, msg, sizeof(msg));

	/* Pump dispatch manually via short sleeps: sw_dispatch runs at prio 5
	 * on core 0/1; give it CPU time. */
	int guard = 0;
	while (s_tcp_calls == 0 && guard++ < 100)
		vTaskDelay(pdMS_TO_TICKS(10));
	TEST_ASSERT_TRUE_MESSAGE(s_tcp_calls >= 1, "TCP copy not delivered");

	/* Bring radio up; held packet must drain exactly once. */
	s_halow_ready = true;
	guard = 0;
	while (s_halow_calls == 0 && guard++ < 200)
		vTaskDelay(pdMS_TO_TICKS(10));
	TEST_ASSERT_TRUE_MESSAGE(s_halow_calls == 1, "held packet lost/dup");
	TEST_ASSERT_EQUAL_UINT32(sizeof(msg), s_last_len);
	TEST_ASSERT_EQUAL_MEMORY(msg, s_last, sizeof(msg));

	/* TCP-sourced packet flows to radio AND BLE now. */
	const uint8_t msg2[] = "downlink-test";
	switch_rx(SW_SRC_TCP, msg2, sizeof(msg2));
	guard = 0;
	while ((s_halow_calls < 2 || s_ble_calls < 1) && guard++ < 300)
		vTaskDelay(pdMS_TO_TICKS(10));
	TEST_ASSERT_EQUAL_INT(2, s_halow_calls);
	TEST_ASSERT_EQUAL_INT(1, s_ble_calls);
}

void app_main(void)
{
	ESP_LOGI(TAG, "=== QEMU ESP32-S3 module tests ===");
	unity_run_all_tests();
	bool ok = (Unity.TestFailures == 0);
	ESP_LOGI(TAG, "QEMU_TESTS_RESULT %s", ok ? "PASS" : "FAIL");
	fflush(stdout);
	vTaskDelay(pdMS_TO_TICKS(200));
}
