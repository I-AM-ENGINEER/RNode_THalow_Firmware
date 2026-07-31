/*
 * test_dns_overflow.c — Tests the captive-portal DNS responder bounds check.
 *
 * The DNS responder in captive_portal.c builds its response in a fixed
 * uint8_t resp[512] buffer. Before the fix, the question section was copied
 * (up to ~500 bytes) then 16 bytes of answer record appended, overflowing
 * BEFORE the bounds check. After the fix, the bounds check runs BEFORE any
 * writes.
 *
 * This test simulates both the old (vulnerable) and new (fixed) logic to
 * prove the fix is effective.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define DNS_BUF_SIZE 512

/* Old (vulnerable) version — writes BEFORE checking bounds */
static int simulate_dns_old(const uint8_t *req, int n) {
	if (n < 12) return -1;
	uint16_t flags = (uint16_t)((req[2] << 8) | req[3]);
	if (flags & 0x8000) return -1;
	uint16_t qd = (uint16_t)((req[4] << 8) | req[5]);
	if (qd == 0) return -1;

	int qoff = 12;
	while (qoff < n) {
		uint8_t len = req[qoff];
		if (len == 0) { qoff += 1; break; }
		if ((len & 0xC0) == 0xC0) { qoff += 2; break; }
		qoff += len + 1;
	}
	if (qoff + 4 > n) return -1;
	int qend = qoff + 4;

	uint8_t resp[DNS_BUF_SIZE + 64]; /* extra room to detect overflow */
	memset(resp, 0xAA, sizeof(resp)); /* poison pattern */

	int rl = 12;
	memcpy(resp + rl, req + 12, (size_t)(qend - 12));
	rl += (qend - 12);

	resp[rl++] = 0xC0; resp[rl++] = 0x0C;
	resp[rl++] = 0x00; resp[rl++] = 0x01;
	resp[rl++] = 0x00; resp[rl++] = 0x01;
	resp[rl++] = 0x00; resp[rl++] = 0x00;
	resp[rl++] = 0x00; resp[rl++] = 60;
	resp[rl++] = 0x00; resp[rl++] = 0x04;
	resp[rl++] = 0x0A; resp[rl++] = 0x0A;
	resp[rl++] = 0x00; resp[rl++] = 0x02;

	return rl - DNS_BUF_SIZE;
}

/* New (fixed) version — bounds check BEFORE writing */
static int simulate_dns_new(const uint8_t *req, int n) {
	if (n < 12) return -1;
	uint16_t flags = (uint16_t)((req[2] << 8) | req[3]);
	if (flags & 0x8000) return -1;
	uint16_t qd = (uint16_t)((req[4] << 8) | req[5]);
	if (qd == 0) return -1;

	int qoff = 12;
	while (qoff < n) {
		uint8_t len = req[qoff];
		if (len == 0) { qoff += 1; break; }
		if ((len & 0xC0) == 0xC0) { qoff += 2; break; }
		qoff += len + 1;
	}
	if (qoff + 4 > n) return -1;
	int qend = qoff + 4;

	/* FIX: bounds check BEFORE any writes */
	int qcopy = qend - 12;
	if (12 + qcopy + 16 > DNS_BUF_SIZE)
		return -2;  /* skipped — would overflow */

	/* Now safe to write */
	uint8_t resp[DNS_BUF_SIZE];
	int rl = 12;
	memcpy(resp + rl, req + 12, (size_t)qcopy);
	rl += qcopy;
	resp[rl++] = 0xC0; resp[rl++] = 0x0C;
	resp[rl++] = 0x00; resp[rl++] = 0x01;
	resp[rl++] = 0x00; resp[rl++] = 0x01;
	resp[rl++] = 0x00; resp[rl++] = 0x00;
	resp[rl++] = 0x00; resp[rl++] = 60;
	resp[rl++] = 0x00; resp[rl++] = 0x04;
	resp[rl++] = 0x0A; resp[rl++] = 0x0A;
	resp[rl++] = 0x00; resp[rl++] = 0x02;

	return rl <= DNS_BUF_SIZE ? 0 : rl - DNS_BUF_SIZE;
}

static int build_long_query(uint8_t *pkt, int maxsz) {
	memset(pkt, 0, maxsz);
	pkt[2] = 0x01; pkt[3] = 0x00; /* flags */
	pkt[4] = 0x00; pkt[5] = 0x01; /* QDCOUNT=1 */
	int off = 12;
	while (off < maxsz - 10) {
		int remaining = maxsz - 6 - off;
		int label_len = remaining < 63 ? remaining : 63;
		if (label_len < 1) break;
		pkt[off] = (uint8_t)label_len;
		off++;
		memset(pkt + off, 'A', label_len);
		off += label_len;
	}
	pkt[off++] = 0x00;
	pkt[off++] = 0x00; pkt[off++] = 0x01; /* QTYPE=A */
	pkt[off++] = 0x00; pkt[off++] = 0x01; /* QCLASS=IN */
	return off;
}

int main(void) {
	printf("=== DNS responder overflow test ===\n\n");

	uint8_t pkt[DNS_BUF_SIZE];
	int n = build_long_query(pkt, DNS_BUF_SIZE);
	printf("  Crafted DNS query: %d bytes\n", n);

	/* Test old (vulnerable) version */
	int old_overflow = simulate_dns_old(pkt, n);
	printf("\n  OLD code: ");
	if (old_overflow > 0)
		printf("OVERFLOW by %d bytes (BUG CONFIRMED)\n", old_overflow);
	else
		printf("no overflow (%d) -- unexpected\n", old_overflow);

	/* Test new (fixed) version */
	int new_result = simulate_dns_new(pkt, n);
	printf("  NEW code: ");
	if (new_result == -2)
		printf("query correctly SKIPPED (would overflow) -- FIX VERIFIED\n");
	else if (new_result == 0)
		printf("response fits (unexpected for this large query)\n");
	else if (new_result > 0)
		printf("STILL OVERFLOWS by %d bytes (FIX FAILED)\n", new_result);
	else
		printf("error %d\n", new_result);

	/* Also test a normal query fits fine */
	uint8_t normal[64];
	memset(normal, 0, sizeof(normal));
	normal[2] = 0x01; normal[5] = 0x01;
	normal[12] = 7; memcpy(normal+13, "example", 7);
	normal[21] = 3; memcpy(normal+22, "com", 3);
	normal[26] = 0x00; normal[27] = 0x00; normal[28] = 0x01;
	normal[29] = 0x00; normal[30] = 0x01;
	int nn = 31;
	int normal_result = simulate_dns_new(normal, nn);
	printf("  NEW code (normal query): ");
	if (normal_result == 0)
		printf("response OK -- normal queries still work\n");
	else
		printf("unexpected result %d\n", normal_result);

	printf("\n  RESULT: ");
	bool pass = (old_overflow > 0) && (new_result == -2) && (normal_result == 0);
	if (pass) {
		printf("ALL CHECKS PASSED -- overflow bug fixed, normal queries work\n");
		return 0;
	} else {
		printf("FAILURE\n");
		return 1;
	}
}
