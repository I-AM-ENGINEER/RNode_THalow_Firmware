#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void system_run( void *arg );

void system_halow_send( const uint8_t *data, size_t len );
bool system_halow_connected( void );

#ifdef __cplusplus
}
#endif
