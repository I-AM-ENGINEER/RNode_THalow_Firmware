#include <string.h>

#include "rns_framing.h"

#define HDLC_FLAG       0x7E
#define HDLC_ESC        0x7D
#define HDLC_MASK       0x20

int rns_framing_encode( const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max ) {
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

void rns_framing_init( rns_framing_t *f, rns_framing_frame_cb cb, void *user ) {
	f->cb      = cb;
	f->user    = user;
	f->len     = 0;
	f->escaped = false;
}

void rns_framing_rx_reset( rns_framing_t *f ) {
	f->len     = 0;
	f->escaped = false;
}

void rns_framing_rx_byte( rns_framing_t *f, uint8_t b ) {
	if (b == HDLC_FLAG) {
		if (f->len > 0 && f->cb)
			f->cb(f->user, f->buf, f->len);
		f->len = 0;
		f->escaped = false;
		return;
	}

	if (f->escaped) {
		b ^= HDLC_MASK;
		f->escaped = false;
	} else if (b == HDLC_ESC) {
		f->escaped = true;
		return;
	}

	if (f->len < RNS_FRAMING_MAX_PACKET)
		f->buf[f->len++] = b;
}
