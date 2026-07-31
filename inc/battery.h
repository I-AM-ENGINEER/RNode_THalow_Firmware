#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void battery_init(void);

uint8_t battery_get_percent(void);

int battery_get_voltage_mv(void);

#ifdef __cplusplus
}
#endif
