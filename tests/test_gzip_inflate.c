/*
 * test_gzip_inflate.c — Unit tests for the pure-C DEFLATE/gzip decompressor.
 *
 * Uses pre-generated gzip streams (gzip_vectors.h, produced from zlib) so
 * the host test needs no zlib dependency. Covers: fixed/dynamic Huffman,
 * handcrafted stored blocks, truncation rejection, and output-cap
 * enforcement (canary check proves no write past dst_cap).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "gzip_inflate.h"
#include "gzip_vectors.h"

static int g_pass = 0, g_fail = 0;
#define TEST_BEGIN(name) do { printf("  [TEST] %s ... ", name); } while(0)
#define TEST_END(ok, why) do { \
	if (ok) { printf("OK\n"); g_pass++; } \
	else    { printf("FAIL (%s)\n", why ? why : "?"); g_fail++; } \
} while(0)

/* Rebuild the expected plaintext of an embedded vector */
static size_t fill_expected(uint8_t *dst, const uint8_t *pat, size_t pat_len,
                            size_t total) {
	size_t off = 0;
	while (off < total) {
		size_t n = total - off < pat_len ? total - off : pat_len;
		memcpy(dst + off, pat, n);
		off += n;
	}
	return total;
}

static void roundtrip_vec(const char *name, const unsigned char *gz,
                          size_t gz_len, const uint8_t *pattern,
                          size_t pat_len, size_t src_len) {
	TEST_BEGIN(name);
	static uint8_t dst[1 << 21]; /* 2 MB scratch */
	static uint8_t exp[1 << 21];
	fill_expected(exp, pattern, pat_len, src_len);

	size_t got = 0;
	int rc = gzip_inflate(gz, gz_len, dst, sizeof(dst), &got);
	if (rc != 0) { TEST_END(0, "inflate failed"); return; }
	if (got != src_len) { TEST_END(0, "length mismatch"); return; }
	if (memcmp(dst, exp, src_len) != 0) { TEST_END(0, "data mismatch"); return; }
	TEST_END(1, NULL);
}

/* Handcraft a gzip member with STORED deflate blocks (BTYPE=00). */
static size_t craft_gzip_stored(const uint8_t *src, size_t len, uint8_t *out,
                                size_t cap) {
	size_t o = 0;
	/* gzip header: magic, deflate, no flags, mtime=0, XFL=0, OS=255 */
	out[o++] = 0x1F; out[o++] = 0x8B; out[o++] = 0x08; out[o++] = 0x00;
	out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
	out[o++] = 0; out[o++] = 0xFF;

	size_t pos = 0;
	while (pos < len) {
		size_t chunk = len - pos > 65535 ? 65535 : len - pos;
		int final = (pos + chunk == len);
		if (o + 5 + chunk > cap) return 0;
		out[o] = final ? 0x01 : 0x00; /* BFINAL + BTYPE=00 stored */
		o += 1;
		out[o++] = (uint8_t)(chunk & 0xFF);
		out[o++] = (uint8_t)(chunk >> 8);
		uint16_t nlen = (uint16_t)(~chunk & 0xFFFF);
		out[o++] = (uint8_t)(nlen & 0xFF);
		out[o++] = (uint8_t)(nlen >> 8);
		memcpy(out + o, src + pos, chunk); o += chunk;
		pos += chunk;
	}
	/* gzip trailer: CRC32 + ISIZE (module ignores them; zeros are fine) */
	memset(out + o, 0, 8); o += 8;
	return o;
}

int main(void) {
	printf("=== gzip_inflate unit tests ===\n");

	roundtrip_vec("dynamic-huffman 200 KB repetitive",
	              vec_dynamic, vec_dynamic_len,
	              (const uint8_t *)"RNodeHaLowSwitchPacketRingBufferOverwrite",
	              sizeof("RNodeHaLowSwitchPacketRingBufferOverwrite") - 1,
	              vec_dynamic_src_len);

	roundtrip_vec("small text stream",
	              vec_small, vec_small_len,
	              (const uint8_t *)"The quick brown fox jumps over the lazy dog! ",
	              sizeof("The quick brown fox jumps over the lazy dog! ") - 1,
	              vec_small_src_len);

	TEST_BEGIN("binary framing-control bytes 8 KB");
	{
		static uint8_t exp[8192];
		size_t off = 0; uint32_t ctr = 0;
		while (off < sizeof(exp)) {
			uint8_t quad[4] = { 0xC0, 0xDB, 0x7E,
			                    (uint8_t)((ctr >> 3) & 0xFF) };
			for (int k = 0; k < 4 && off < sizeof(exp); k++)
				exp[off++] = quad[k];
			ctr++;
		}
		static uint8_t dst[1 << 20];
		size_t got = 0;
		int rc = gzip_inflate(vec_binary, vec_binary_len, dst,
		                      sizeof(dst), &got);
		if (rc != 0 || got != sizeof(exp) ||
		    memcmp(dst, exp, sizeof(exp)) != 0)
			TEST_END(0, "mismatch");
		else
			TEST_END(1, NULL);
	}

	/* --- Stored blocks (handcrafted, incompressible payload) --- */
	TEST_BEGIN("stored blocks, handcrafted gzip");
	{
		static uint8_t src[70000];
		for (size_t i = 0; i < sizeof(src); i++)
			src[i] = (uint8_t)(i ^ 0x5A);
		static uint8_t gz[80000];
		size_t gz_len = craft_gzip_stored(src, sizeof(src), gz, sizeof(gz));
		if (!gz_len) { TEST_END(0, "craft failed"); goto done_stored; }
		static uint8_t dst[1 << 20];
		size_t got = 0;
		if (gzip_inflate(gz, gz_len, dst, sizeof(dst), &got) != 0) {
			TEST_END(0, "inflate failed"); goto done_stored;
		}
		if (got != sizeof(src) || memcmp(dst, src, sizeof(src)) != 0) {
			TEST_END(0, "mismatch"); goto done_stored;
		}
		TEST_END(1, NULL);
done_stored:;
	}

	/* --- Truncated stream must be rejected --- */
	TEST_BEGIN("truncated gzip returns -1");
	{
		int ok = 0;
		static uint8_t dst[1 << 20];
		size_t got = 0;
		for (size_t cut = 20; cut + 10 < vec_dynamic_len && !ok; cut += 37) {
			if (gzip_inflate(vec_dynamic, cut, dst, sizeof(dst), &got) == -1)
				ok = 1;
		}
		/* Also truncated stored-block stream */
		static uint8_t src[5000];
		memset(src, 'A', sizeof(src));
		static uint8_t gzs[6000];
		size_t gzs_len = craft_gzip_stored(src, sizeof(src), gzs, sizeof(gzs));
		for (size_t cut = 15; cut < gzs_len - 4 && !ok; cut += 13) {
			if (gzip_inflate(gzs, cut, dst, sizeof(dst), &got) == -1)
				ok = 1;
		}
		TEST_END(ok, "no truncation rejected");
	}

	/* --- Output cap enforcement with canary BEYOND dst_cap --- */
	TEST_BEGIN("output cap enforced (no write past dst_cap)");
	{
		size_t cap = 100000; /* far below the 200 KB inflated size */
		size_t pad = 4096;
		uint8_t *mem = malloc(cap + pad);
		uint8_t *dst = mem;
		memset(mem, 0xCC, cap + pad);
		size_t got = 0;
		int rc = gzip_inflate(vec_dynamic, vec_dynamic_len, dst, cap, &got);
		int ok = (rc == -1);
		ok = ok && (got <= cap);           /* reported length within cap   */
		ok = ok && (got > cap / 2);        /* actually filled to the brim  */
		for (size_t i = 0; i < pad && ok; i++)      /* guard zone untouched */
			if (dst[cap + i] != 0xCC) { ok = 0; break; }
		free(mem);
		TEST_END(ok, rc == 0 ? "unexpected success" : "guard clobbered");
	}

	/* --- Garbage input must not crash --- */
	TEST_BEGIN("garbage input fails gracefully");
	{
		uint8_t junk[512];
		for (int i = 0; i < 512; i++) junk[i] = (uint8_t)(i * 37 + 11);
		static uint8_t dst[4096];
		size_t got = 0;
		int rc = gzip_inflate(junk, sizeof(junk), dst, sizeof(dst), &got);
		TEST_END(rc == -1 || rc == 0, "nonsense code");
	}

	printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
	return g_fail > 0 ? 1 : 0;
}
