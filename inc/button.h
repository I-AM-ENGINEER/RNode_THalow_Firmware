#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	BUTTON_EVENT_LONG_PRESS,
} button_event_t;

typedef void (*button_callback_t)(button_event_t event);

void button_init(void);
void button_set_callback(button_callback_t cb);

#ifdef __cplusplus
}
#endif
