#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	BLE_STATE_OFF,
	BLE_STATE_ON,
	BLE_STATE_PAIRING,
	BLE_STATE_CONNECTED,
} ble_state_t;

void ble_init( void );
void ble_enable_pairing( void );
void ble_disable_pairing( void );

ble_state_t ble_get_state( void );
uint32_t ble_get_passkey( void );
bool ble_connected( void );

int ble_available( void );
int ble_read( void );
size_t ble_read_bytes( uint8_t *buf, size_t len );
size_t ble_write( const uint8_t *buf, size_t len );
void ble_flush( void );

#ifdef __cplusplus
}
#endif
