#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void status_led_init( void );

void status_led_notify_traffic( void );

void status_led_show_battery( int percent );

/* Signal the status-LED task to run a 2-second fast-blink confirmation
 * sequence, then hold the LED on. Called from the factory-reset path
 * just before NVS is erased. The caller must delay long enough for the
 * sequence to complete before rebooting. */
void status_led_factory_reset_notify( void );

#ifdef __cplusplus
}
#endif
