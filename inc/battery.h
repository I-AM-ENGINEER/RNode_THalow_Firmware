#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Battery charge state, wire-compatible with official RNode firmware
 * (CMD_STAT_BAT payload byte 0). */
#define BATTERY_STATE_UNKNOWN     0x00
#define BATTERY_STATE_DISCHARGING 0x01
#define BATTERY_STATE_CHARGING    0x02
#define BATTERY_STATE_CHARGED     0x03

void battery_init(void);

uint8_t battery_get_percent(void);

int battery_get_voltage_mv(void);

uint8_t battery_get_state(void);

#ifdef __cplusplus
}
#endif
