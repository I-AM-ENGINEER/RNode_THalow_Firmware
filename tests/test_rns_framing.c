/*
 * test_rns_framing.c — Unit tests for HDLC framing encode/decode.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define RNS_FRAMING_MAX_PACKET 1024
typedef void (*rns_framing_frame_cb)(void *user, const uint8_t *data, size_t len);
typedef struct rns_framing {
	rns_framing_frame_cb cb;
	void *user;
	uint8_t buf[RNS_FRAMING_MAX_PACKET];
	int len;
	bool escaped;
} rns_framing_t;

/* ---- rns_framing.c inlined ---- */
#define HDLC_FLAG 0x7E
#define HDLC_ESC  0x7D
#define HDLC_MASK 0x20

static int rns_framing_encode(const uint8_t *in, size_t in_len,
                              uint8_t *out, size_t out_max) {
	int pos = 0;
	if (pos >= (int)out_max) return -1;
	out[pos++] = HDLC_FLAG;
	for (size_t i = 0; i < in_len; i++) {
		if (in[i] == HDLC_FLAG || in[i] == HDLC_ESC) {
			if (pos + 2 > (int)out_max) return -1;
			out[pos++] = HDLC_ESC;
			out[pos++] = in[i] ^ HDLC_MASK;
		} else {
			if (pos + 1 > (int)out_max) return -1;
			out[pos++] = in[i];
		}
	}
	if (pos + 1 > (int)out_max) return -1;
	out[pos++] = HDLC_FLAG;
	return pos;
}
static void rns_framing_init(rns_framing_t *f, rns_framing_frame_cb cb, void *user) {
	f->cb = cb; f->user = user; f->len = 0; f->escaped = false;
}
static void rns_framing_rx_byte(rns_framing_t *f, uint8_t b) {
	if (b == HDLC_FLAG) {
		if (f->len > 0 && f->cb) f->cb(f->user, f->buf, f->len);
		f->len = 0; f->escaped = false;
		return;
	}
	if (f->escaped) { b ^= HDLC_MASK; f->escaped = false; }
	else if (b == HDLC_ESC) { f->escaped = true; return; }
	if (f->len < RNS_FRAMING_MAX_PACKET) f->buf[f->len++] = b;
}

/* ---- harness ---- */
static int g_pass = 0, g_fail = 0;

typedef struct { uint8_t data[RNS_FRAMING_MAX_PACKET]; size_t len; int count; } dr_t;
static void decode_cb(void *user, const uint8_t *data, size_t len) {
	dr_t *r = (dr_t*)user;
	if (len > RNS_FRAMING_MAX_PACKET) len = RNS_FRAMING_MAX_PACKET;
	memcpy(r->data, data, len); r->len = len; r->count++;
}

static void roundtrip(const uint8_t *payload, size_t plen, const char *name) {
	printf("  [TEST] %s ... ", name);
	uint8_t enc[RNS_FRAMING_MAX_PACKET * 2 + 4];
	int el = rns_framing_encode(payload, plen, enc, sizeof(enc));
	if (el <= 0) { printf("FAIL (encode=%d)\n", el); g_fail++; return; }
	rns_framing_t f; dr_t r = {0};
	rns_framing_init(&f, decode_cb, &r);
	for (int i = 0; i < el; i++) rns_framing_rx_byte(&f, enc[i]);
	if (r.count != 1) { printf("FAIL (call_count=%d)\n", r.count); g_fail++; return; }
	if (r.len != plen) { printf("FAIL (len=%zu want %zu)\n", r.len, plen); g_fail++; return; }
	if (memcmp(r.data, payload, plen) != 0) { printf("FAIL (data mismatch)\n"); g_fail++; return; }
	printf("OK\n"); g_pass++;
}

static void test_roundtrip(void) {
	uint8_t p[256];
	for (int i = 0; i < 256; i++) p[i] = (uint8_t)(i * 7 + 3);
	roundtrip(p, sizeof(p), "roundtrip random data");
}
static void test_flag_bytes(void) {
	uint8_t p[] = {0x7E,0x00,0x7E,0x7E,0xFF,0x7E};
	roundtrip(p, sizeof(p), "payload with 0x7E bytes");
}
static void test_esc_bytes(void) {
	uint8_t p[] = {0x7D,0x00,0x7D,0x7E,0x7D,0x7D};
	roundtrip(p, sizeof(p), "payload with 0x7D bytes");
}
static void test_max_payload(void) {
	uint8_t p[RNS_FRAMING_MAX_PACKET];
	for (int i = 0; i < RNS_FRAMING_MAX_PACKET; i++) p[i] = (uint8_t)(i & 0xFF);
	roundtrip(p, sizeof(p), "max-size payload (1024 bytes)");
}
static void test_empty(void) {
	printf("  [TEST] empty payload ... ");
	uint8_t enc[16];
	int el = rns_framing_encode(NULL, 0, enc, sizeof(enc));
	if (el != 2) { printf("FAIL (enc_len=%d expected 2)\n", el); g_fail++; return; }
	rns_framing_t f; dr_t r = {0};
	rns_framing_init(&f, decode_cb, &r);
	rns_framing_rx_byte(&f, enc[0]);
	rns_framing_rx_byte(&f, enc[1]);
	if (r.count != 0) { printf("FAIL (expected 0 calls got %d)\n", r.count); g_fail++; return; }
	printf("OK\n"); g_pass++;
}
static void test_encode_overflow(void) {
	printf("  [TEST] encode overflow returns -1 ... ");
	uint8_t p[100]; memset(p, 0x41, sizeof(p));
	uint8_t small[50];
	if (rns_framing_encode(p, sizeof(p), small, sizeof(small)) != -1) { printf("FAIL (expected -1)\n"); g_fail++; return; }
	printf("OK\n"); g_pass++;
}
static void test_decode_truncation(void) {
	printf("  [TEST] partial frame no callback ... ");
	uint8_t p[] = {1,2,3,4,5};
	uint8_t enc[64];
	int el = rns_framing_encode(p, sizeof(p), enc, sizeof(enc));
	rns_framing_t f; dr_t r = {0};
	rns_framing_init(&f, decode_cb, &r);
	for (int i = 0; i < el - 1; i++) rns_framing_rx_byte(&f, enc[i]);
	if (r.count != 0) { printf("FAIL (callback fired %d times)\n", r.count); g_fail++; return; }
	printf("OK\n"); g_pass++;
}
static void test_back_to_back(void) {
	printf("  [TEST] back-to-back frames ... ");
	uint8_t p1[] = {0xAA,0xBB,0xCC}, p2[] = {0x11,0x22,0x33,0x44};
	uint8_t enc[128];
	int e1 = rns_framing_encode(p1, sizeof(p1), enc, sizeof(enc));
	int e2 = rns_framing_encode(p2, sizeof(p2), enc+e1, sizeof(enc)-e1);
	rns_framing_t f; dr_t r = {0};
	rns_framing_init(&f, decode_cb, &r);
	for (int i = 0; i < e1+e2; i++) rns_framing_rx_byte(&f, enc[i]);
	if (r.count != 2) { printf("FAIL (call_count=%d expected 2)\n", r.count); g_fail++; return; }
	if (r.len != sizeof(p2) || memcmp(r.data, p2, sizeof(p2)) != 0) { printf("FAIL (last frame mismatch)\n"); g_fail++; return; }
	printf("OK\n"); g_pass++;
}
static void test_worst_case_expansion(void) {
	printf("  [TEST] worst-case all-0x7E expansion ... ");
	uint8_t p[100]; memset(p, 0x7E, sizeof(p));
	uint8_t enc[256];
	int el = rns_framing_encode(p, sizeof(p), enc, sizeof(enc));
	if (el != 202) { printf("FAIL (enc_len=%d expected 202)\n", el); g_fail++; return; }
	printf("OK\n"); g_pass++;
}
static void test_decode_oversize(void) {
	printf("  [TEST] decode oversize truncates ... ");
	rns_framing_t f; dr_t r = {0};
	rns_framing_init(&f, decode_cb, &r);
	for (int i = 0; i < RNS_FRAMING_MAX_PACKET + 100; i++)
		rns_framing_rx_byte(&f, 0x41);
	rns_framing_rx_byte(&f, HDLC_FLAG);
	if (r.count != 1) { printf("FAIL (count=%d)\n", r.count); g_fail++; return; }
	if (r.len != RNS_FRAMING_MAX_PACKET) { printf("FAIL (len=%zu want %d)\n", r.len, RNS_FRAMING_MAX_PACKET); g_fail++; return; }
	printf("OK\n"); g_pass++;
}

int main(void) {
	printf("=== rns_framing unit tests ===\n");
	test_roundtrip();
	test_flag_bytes();
	test_esc_bytes();
	test_max_payload();
	test_empty();
	test_encode_overflow();
	test_decode_truncation();
	test_back_to_back();
	test_worst_case_expansion();
	test_decode_oversize();
	printf("\n=== Results: %d/%d passed, %d failed ===\n",
	       g_pass, g_pass+g_fail, g_fail);
	return g_fail > 0 ? 1 : 0;
}
