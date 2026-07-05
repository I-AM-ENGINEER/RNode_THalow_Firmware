#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_FRAMING_MAX_PACKET (1024)

typedef void (*rns_framing_frame_cb)( void *user, const uint8_t *data, size_t len );

typedef struct rns_framing {
	rns_framing_frame_cb cb;
	void                 *user;
	uint8_t              buf[RNS_FRAMING_MAX_PACKET];
	int                  len;
	bool                 escaped;
} rns_framing_t;

/* Encode a packet into an HDLC-framed stream (stateless).
 * Returns the encoded length, or -1 on overflow. */
int rns_framing_encode( const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_max );

/* Initialize a deframer instance. @f must point to storage that outlives
 * the use of the instance (e.g. a static or heap variable owned by the
 * caller). @cb is invoked with @user for each decoded frame. */
void rns_framing_init( rns_framing_t *f, rns_framing_frame_cb cb, void *user );

/* Feed a received byte to the deframer. */
void rns_framing_rx_byte( rns_framing_t *f, uint8_t b );

/* Reset the deframer state (e.g. after a link reconnect). */
void rns_framing_rx_reset( rns_framing_t *f );

#ifdef __cplusplus
}
#endif
