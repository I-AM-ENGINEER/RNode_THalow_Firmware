/*
 * test_switch_ring.c — Unit tests for the central packet switch ring logic.
 *
 * Compiles the REAL src/switch.c against host FreeRTOS stubs
 * (freertos_host_mock.h). Verifies:
 *   1. switch_rx copies into both non-source rings
 *   2. source ring stays empty (no self-delivery)
 *   3. overwrite-oldest on full drop-ok rings (BLE/TCP)
 *   4. HaLow ring: blocking push applies backpressure — but in the
 *      single-threaded test we verify it accepts up to capacity and that
 *      dispatch (simulated via direct pop) frees slots again
 *   5. held-packet semantics: halow_sink false keeps the packet, retry
 *      delivers it exactly once
 *   6. no-sink rings are skipped by switch_rx
 *
 * Because switch_init() spawns no tasks under the mock and mutexes are
 * no-ops, we drive the internals deterministically: call switch_rx(), then
 * simulate the dispatch loop by popping from each ring ourselves.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Pull in the module under test with host shims. switch.c includes:
 *   string.h, FreeRTOS headers (mocked), esp_log/heap_caps (mocked),
 *   switch.h (real), rns_framing.h (real) */
#include "freertos_host_mock.h"

/* esp_log shim */
#define ESP_LOGI(tag, fmt...) do { printf("[I %s] ", tag); printf(fmt); printf("\n"); } while(0)
#define ESP_LOGW(tag, fmt...) do { printf("[W %s] ", tag); printf(fmt); printf("\n"); } while(0)
#define ESP_LOGE(tag, fmt...) do { printf("[E %s] ", tag); printf(fmt); printf("\n"); } while(0)

/* Include the real implementation directly to access statics for
 * white-box assertions (ring counts). */
#include "../src/switch.c"

static int g_pass = 0, g_fail = 0;
#define TEST_BEGIN(name) do { printf("  [TEST] %s ... ", name); } while(0)
#define TEST_END(ok, why) do { \
	if (ok) { printf("OK\n"); g_pass++; } \
	else    { printf("FAIL (%s)\n", why ? why : "?"); g_fail++; } \
} while(0)

/* ---- Test sink bookkeeping ------------------------------------------- */

typedef struct {
	int calls;
	uint8_t last_data[SW_PKT_MAX];
	size_t last_len;
	bool deliver;   /* what the sink returns */
} sink_rec_t;

static sink_rec_t rec_ble, rec_tcp, rec_halow;

static void reset_recs(void) {
	memset(&rec_ble, 0, sizeof(rec_ble));   rec_ble.deliver = true;
	memset(&rec_tcp, 0, sizeof(rec_tcp));   rec_tcp.deliver = true;
	memset(&rec_halow, 0, sizeof(rec_halow)); rec_halow.deliver = true;
}

static bool ble_sink(const uint8_t *d, size_t l) {
	rec_ble.calls++;
	rec_ble.last_len = l < sizeof(rec_ble.last_data) ? l : sizeof(rec_ble.last_data);
	memcpy(rec_ble.last_data, d, rec_ble.last_len);
	return rec_ble.deliver;
}
static bool tcp_sink(const uint8_t *d, size_t l) {
	rec_tcp.calls++;
	rec_tcp.last_len = l < sizeof(rec_tcp.last_data) ? l : sizeof(rec_tcp.last_data);
	memcpy(rec_tcp.last_data, d, rec_tcp.last_len);
	return rec_tcp.deliver;
}
static bool halow_sink(const uint8_t *d, size_t l) {
	rec_halow.calls++;
	rec_halow.last_len = l < sizeof(rec_halow.last_data) ? l : sizeof(rec_halow.last_data);
	memcpy(rec_halow.last_data, d, rec_halow.last_len);
	return rec_halow.deliver;
}

/* Simulate one pass of the dispatch task's BLE/TCP portion + HaLow held
 * handling, mirroring sw_dispatch_task() body (single-threaded test). */
static int dispatch_once(void) {
	int delivered = 0;

	/* HaLow: held-packet pattern */
	if (s_rings[SW_SRC_HALOW].sink) {
		if (!s_halow_holding) {
			if (ring_pop(&s_rings[SW_SRC_HALOW], &s_halow_held))
				s_halow_holding = true;
		}
		if (s_halow_holding) {
			if (s_rings[SW_SRC_HALOW].sink(s_halow_held.data,
			                               s_halow_held.len)) {
				s_halow_holding = false;
				delivered++;
			}
		}
	}
	/* BLE + TCP: pop / try / push-front-drop */
	for (int s = SW_SRC_BLE; s <= SW_SRC_TCP; s++) {
		sw_ring_t *r = &s_rings[s];
		if (!r->sink) continue;
		sw_slot_t pkt;
		if (!ring_pop(r, &pkt)) continue;
		if (r->sink(pkt.data, pkt.len)) delivered++;
		else ring_push_front_drop(r, &pkt);
	}
	return delivered;
}

int main(void) {
	printf("=== switch ring unit tests ===\n");

	switch_init();
	switch_register_ble_sink(ble_sink);
	switch_register_tcp_sink(tcp_sink);
	switch_register_halow_sink(halow_sink);

	/* --- 1. fan-out: BLE packet reaches TCP + HaLow, not BLE --- */
	TEST_BEGIN("BLE packet fans out to TCP+HaLow only");
	reset_recs();
	uint8_t pkt1[] = "hello-radio";
	switch_rx(SW_SRC_BLE, pkt1, sizeof(pkt1));
	int ok = (rec_tcp.calls == 0 && rec_halow.calls == 0);
	ok = ok && dispatch_once() == 2;
	ok = ok && rec_tcp.calls == 1 && rec_halow.calls == 1;
	ok = ok && rec_ble.calls == 0;
	ok = ok && rec_tcp.last_len == sizeof(pkt1);
	ok = ok && memcmp(rec_tcp.last_data, pkt1, sizeof(pkt1)) == 0;
	TEST_END(ok, "fan-out wrong");

	/* --- 2. radio packet fans out to BLE + TCP --- */
	TEST_BEGIN("HaLow packet fans out to BLE+TCP only");
	reset_recs();
	uint8_t pkt2[] = "from-the-air";
	switch_rx(SW_SRC_HALOW, pkt2, sizeof(pkt2));
	ok = dispatch_once() == 2;
	ok = ok && rec_ble.calls == 1 && rec_tcp.calls == 1 && rec_halow.calls == 0;
	TEST_END(ok, "radio fan-out wrong");

	/* --- 3. overwrite-oldest on drop-ok ring when full ---
	 * NOTE: HaLow sink is temporarily unregistered so the burst does not
	 * hit the blocking push (single-threaded test has no dispatcher to
	 * drain it — that backpressure is exercised separately). */
	TEST_BEGIN("TCP ring overwrites oldest at capacity");
	reset_recs();
	s_rings[SW_SRC_HALOW].sink = NULL;
	rec_tcp.deliver = false; /* TCP client gone: nothing drains */
	for (int i = 0; i < SW_RING_SIZE + 10; i++) {
		char buf[32];
		int n = snprintf(buf, sizeof(buf), "pkt-%03d", i);
		switch_rx(SW_SRC_BLE, (uint8_t *)buf, (size_t)n + 1);
	}
	ok = (s_rings[SW_SRC_TCP].count == SW_RING_SIZE);
	/* oldest surviving packet must be #10 (indices 0..9 overwritten) */
	ok = ok && s_rings[SW_SRC_TCP].count == SW_RING_SIZE;
	rec_tcp.deliver = true;
	dispatch_once();
	ok = ok && rec_tcp.last_len == strlen("pkt-010") + 1 &&
	     strcmp((char *)rec_tcp.last_data, "pkt-010") == 0;
	s_rings[SW_SRC_HALOW].sink = halow_sink;
	TEST_END(ok, "overwrite-oldest wrong");

	/* --- 4. HaLow backpressure: capacity bounded, no drops --- */
	TEST_BEGIN("HaLow ring holds all packets up to capacity");
	reset_recs();
	rec_halow.deliver = false;
	for (int i = 0; i < SW_RING_SIZE; i++) {
		char buf[32];
		int n = snprintf(buf, sizeof(buf), "air-%03d", i);
		switch_rx(SW_SRC_TCP, (uint8_t *)buf, (size_t)n + 1);
	}
	ok = (s_rings[SW_SRC_HALOW].count == SW_RING_SIZE);
	TEST_END(ok, "halow count != capacity");
	/* NOTE: pushing beyond capacity would block forever single-threaded
	 * (dispatch not running), which is the intended backpressure behavior;
	 * covered indirectly here by checking capacity is reached exactly. */

	/* --- 5. held-packet: sink false retains, later retry delivers --- */
	TEST_BEGIN("held-packet retained until radio ready");
	/* First dispatch attempt: radio still down -> held */
	dispatch_once();
	ok = (s_halow_holding == true && s_rings[SW_SRC_HALOW].count == SW_RING_SIZE - 1);
	ok = ok && rec_halow.calls == 1;
	/* Radio comes up */
	rec_halow.deliver = true;
	dispatch_once();
	ok = ok && s_halow_holding == false;
	ok = ok && rec_halow.calls == 2;
	ok = ok && strncmp((char *)rec_halow.last_data, "air-", 4) == 0;
	TEST_END(ok, "held-packet flow broken");

	/* --- 6. drain remaining halow queue FIFO --- */
	TEST_BEGIN("halow queue drains FIFO after link up");
	for (int i = 0; i < SW_RING_SIZE - 1; i++)
		dispatch_once();
	ok = (s_rings[SW_SRC_HALOW].count == 0 && !s_halow_holding);
	/* first delivered after 'air-000' was 'air-001'... last should be air-031 */
	ok = ok && strcmp((char *)rec_halow.last_data, "air-031") == 0;
	TEST_END(ok, "FIFO order broken");

	/* --- 7. unregistered sink skipped --- */
	TEST_BEGIN("switch_rx skips unregistered sink");
	s_rings[SW_SRC_BLE].sink = NULL;
	int ble_count_before = s_rings[SW_SRC_BLE].count;
	switch_rx(SW_SRC_HALOW, (uint8_t *)"x", 2);
	ok = (s_rings[SW_SRC_BLE].count == ble_count_before);
	s_rings[SW_SRC_BLE].sink = ble_sink;
	TEST_END(ok, "unregistered sink got packets");

	printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail > 0 ? 1 : 0;
}
