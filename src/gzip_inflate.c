#include <string.h>

#include "gzip_inflate.h"

#define INF_MAXBITS 15
#define INF_MAXLCODES 288
#define INF_MAXDCODES 30
#define INF_MAXCODES (INF_MAXLCODES + INF_MAXDCODES)

typedef struct {
	const uint8_t *in;
	size_t in_len;
	size_t in_pos;
	uint64_t bitbuf;
	int bitcnt;
	uint8_t *out;
	size_t out_cap;
	size_t out_len;
	int error;
} inf_state_t;

static int inf_need(inf_state_t *s, size_t extra) {
	if (s->out_len + extra > s->out_cap) {
		s->error = 1;
		return -1;
	}
	return 0;
}

static uint32_t inf_getbits(inf_state_t *s, int n) {
	while (s->bitcnt < n) {
		if (s->in_pos >= s->in_len) {
			s->error = 1;
			return 0;
		}
		s->bitbuf |= (uint64_t)s->in[s->in_pos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	uint32_t v = (uint32_t)(s->bitbuf & (((uint64_t)1 << n) - 1));
	s->bitbuf >>= n;
	s->bitcnt -= n;
	return v;
}

typedef struct {
	int16_t counts[INF_MAXBITS + 1];
	int16_t symbols[INF_MAXCODES];
} huff_t;

static void huff_build(huff_t *h, const uint8_t *lengths, int n) {
	for (int len = 0; len <= INF_MAXBITS; len++)
		h->counts[len] = 0;
	for (int i = 0; i < n; i++)
		h->counts[lengths[i]]++;
	h->counts[0] = 0;

	int16_t offsets[INF_MAXBITS + 2];
	offsets[1] = 0;
	for (int len = 1; len <= INF_MAXBITS; len++)
		offsets[len + 1] = offsets[len] + h->counts[len];

	for (int i = 0; i < n; i++)
		if (lengths[i] != 0)
			h->symbols[offsets[lengths[i]]++] = (int16_t)i;
}

static int huff_decode(inf_state_t *s, const huff_t *h) {
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= INF_MAXBITS; len++) {
		code |= inf_getbits(s, 1);
		int count = h->counts[len];
		if (code - first < count)
			return h->symbols[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->error = 1;
	return -1;
}

static const uint16_t LEN_BASE[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int inflate_codes(inf_state_t *s, const huff_t *lit, const huff_t *dist) {
	for (;;) {
		int sym = huff_decode(s, lit);
		if (sym < 0)
			return -1;
		if (sym == 256)
			return 0;
		if (sym < 256) {
			if (inf_need(s, 1))
				return -1;
			s->out[s->out_len++] = (uint8_t)sym;
			continue;
		}
		sym -= 257;
		if (sym >= 29) {
			s->error = 1;
			return -1;
		}
		int length = LEN_BASE[sym] + (int)inf_getbits(s, LEN_EXTRA[sym]);
		int dsym = huff_decode(s, dist);
		if (dsym < 0)
			return -1;
		int distance = DIST_BASE[dsym] + (int)inf_getbits(s, DIST_EXTRA[dsym]);
		if (inf_need(s, length))
			return -1;
		if ((size_t)distance > s->out_len)
			return -1;
		for (int i = 0; i < length; i++) {
			s->out[s->out_len] = s->out[s->out_len - distance];
			s->out_len++;
		}
	}
}

static int inflate_stored(inf_state_t *s) {
	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->in_pos + 4 > s->in_len) {
		s->error = 1;
		return -1;
	}
	uint32_t len = (uint32_t)s->in[s->in_pos] | ((uint32_t)s->in[s->in_pos + 1] << 8);
	s->in_pos += 4;
	if (s->in_pos + len > s->in_len) {
		s->error = 1;
		return -1;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (inf_need(s, 1))
			return -1;
		s->out[s->out_len++] = s->in[s->in_pos++];
	}
	return 0;
}

static int inflate_fixed(inf_state_t *s) {
	uint8_t lengths[INF_MAXLCODES];
	for (int i = 0; i < 144; i++)
		lengths[i] = 8;
	for (int i = 144; i < 256; i++)
		lengths[i] = 9;
	for (int i = 256; i < 280; i++)
		lengths[i] = 7;
	for (int i = 280; i < INF_MAXLCODES; i++)
		lengths[i] = 8;

	huff_t lit;
	huff_build(&lit, lengths, INF_MAXLCODES);

	uint8_t dlens[INF_MAXDCODES];
	for (int i = 0; i < INF_MAXDCODES; i++)
		dlens[i] = 5;
	huff_t dist;
	huff_build(&dist, dlens, INF_MAXDCODES);

	return inflate_codes(s, &lit, &dist);
}

static const int CODE_ORDER[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inflate_dynamic(inf_state_t *s) {
	int hlit = inf_getbits(s, 5) + 257;
	int hdist = inf_getbits(s, 5) + 1;
	int hclen = inf_getbits(s, 4) + 4;

	uint8_t cl_lengths[19];
	for (int i = 0; i < 19; i++)
		cl_lengths[i] = 0;
	for (int i = 0; i < hclen; i++)
		cl_lengths[CODE_ORDER[i]] = (uint8_t)inf_getbits(s, 3);

	huff_t cl;
	huff_build(&cl, cl_lengths, 19);

	uint8_t lengths[INF_MAXCODES];
	int n = hlit + hdist;
	if (n > INF_MAXCODES) {
		s->error = 1;
		return -1;
	}
	int idx = 0;
	while (idx < n) {
		int sym = huff_decode(s, &cl);
		if (sym < 0)
			return -1;
		if (sym < 16) {
			lengths[idx++] = (uint8_t)sym;
		} else if (sym == 16) {
			int rep = 3 + (int)inf_getbits(s, 2);
			if (idx == 0) {
				s->error = 1;
				return -1;
			}
			uint8_t prev = lengths[idx - 1];
			while (rep-- > 0 && idx < n)
				lengths[idx++] = prev;
		} else if (sym == 17) {
			int rep = 3 + (int)inf_getbits(s, 3);
			while (rep-- > 0 && idx < n)
				lengths[idx++] = 0;
		} else if (sym == 18) {
			int rep = 11 + (int)inf_getbits(s, 7);
			while (rep-- > 0 && idx < n)
				lengths[idx++] = 0;
		} else {
			s->error = 1;
			return -1;
		}
	}

	huff_t lit;
	huff_build(&lit, lengths, hlit);
	huff_t dist;
	huff_build(&dist, lengths + hlit, hdist);

	return inflate_codes(s, &lit, &dist);
}

static int inflate_block(inf_state_t *s, int *last) {
	int bfinal = inf_getbits(s, 1);
	int btype = inf_getbits(s, 2);
	*last = bfinal;
	if (btype == 0)
		return inflate_stored(s);
	if (btype == 1)
		return inflate_fixed(s);
	if (btype == 2)
		return inflate_dynamic(s);
	s->error = 1;
	return -1;
}

int gzip_inflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap, size_t *out_len) {
	inf_state_t s;
	memset(&s, 0, sizeof(s));
	s.in = src;
	s.in_len = src_len;
	s.out = dst;
	s.out_cap = dst_cap;
	s.out_len = 0;
	s.error = 0;

	size_t pos = 0;
	if (src_len >= 2 && src[0] == 0x1f && src[1] == 0x8b) {
		if (src_len < 10) {
			s.error = 1;
			return -1;
		}
		uint8_t flags = src[3];
		pos = 10;
		if (flags & 0x04) {
			if (pos + 2 > src_len) {
				s.error = 1;
				return -1;
			}
			uint16_t xlen = (uint16_t)(src[pos] | (src[pos + 1] << 8));
			pos += 2 + xlen;
		}
		if (flags & 0x08) {
			while (pos < src_len && src[pos] != 0)
				pos++;
			pos++;
		}
		if (flags & 0x10) {
			while (pos < src_len && src[pos] != 0)
				pos++;
			pos++;
		}
		if (flags & 0x02)
			pos += 2;
	} else {
		pos = 0;
	}

	s.in_pos = pos;

	int last = 0;
	while (!last && !s.error) {
		if (inflate_block(&s, &last) != 0)
			break;
	}

	if (s.error)
		return -1;

	*out_len = s.out_len;
	return 0;
}
