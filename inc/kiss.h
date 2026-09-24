#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max size of a KISS frame payload (NOT counting the KISS command byte,
 * FEND framing or escaping). rx_frame holds the command byte PLUS the
 * payload, so this must be >= RNS_FRAMING_MAX_PACKET (1024) + 1; the old
 * value of exactly 1024 silently truncated every full-size RNS packet by
 * its last byte. +7 margin. */
#define KISS_FRAME_MAX (1032)

typedef void (*kiss_tx_cb)( void *user, const uint8_t *buf, size_t len );
typedef void (*kiss_data_cb)( void *user, const uint8_t *data, size_t len );

typedef struct kiss {
	kiss_tx_cb    tx_cb;
	void          *tx_user;
	kiss_data_cb  data_cb;
	void          *data_user;
	uint8_t       rx_frame[KISS_FRAME_MAX];
	int           rx_len;
	bool          in_frame;
	bool          escape;
	/* Set when an incoming frame exceeds rx_frame: the frame is dropped
	 * whole (never delivered truncated) and bytes are discarded until the
	 * next FEND so the stream resynchronizes on a frame boundary. */
	bool          rx_overflow;
	uint32_t      rx_dropped;
} kiss_t;

/* Initialize a KISS instance bound to a TX transport. @k must point to
 * storage owned by the caller. */
void kiss_init( kiss_t *k, kiss_tx_cb tx_cb, void *tx_user );

/* Register a handler for incoming KISS data frames. */
void kiss_set_data_callback( kiss_t *k, kiss_data_cb cb, void *user );

/* Feed a received byte from the transport. */
void kiss_rx_byte( kiss_t *k, uint8_t b );

/* Send a data frame through the bound TX transport. */
void kiss_send_data( kiss_t *k, const uint8_t *data, size_t len );

#ifdef __cplusplus
}
#endif
