#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void system_run( void *arg );

/* Send a packet to the HaLow radio over the RNS framing link. */
void system_halow_send( const uint8_t *data, size_t len );

#ifdef __cplusplus
}
#endif
