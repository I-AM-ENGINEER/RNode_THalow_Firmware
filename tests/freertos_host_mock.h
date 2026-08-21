/*
 * freertos_host_mock.h — Minimal FreeRTOS + heap_caps shims so that
 * src/switch.c compiles and runs on the host for unit testing.
 *
 * The host tests are single-threaded, so mutexes become no-ops and
 * vTaskDelay becomes a no-op. heap_caps_malloc maps to malloc (the
 * MALLOC_CAP_SPIRAM distinction is irrelevant on host).
 */
#ifndef FREERTOS_HOST_MOCK_H
#define FREERTOS_HOST_MOCK_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* --- FreeRTOS kernel types -------------------------------------------- */
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdPASS      1
#define pdFAIL      0
#define pdTRUE      1
#define pdFALSE     0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1

static inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
	/* Single-threaded host test: any non-NULL token works. */
	return (SemaphoreHandle_t)1;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
	(void)s; (void)t; return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
	(void)s; return pdTRUE;
}
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                                     int stack, void *arg, int prio,
                                     TaskHandle_t *handle) {
	/* Do not run the dispatch task in unit tests. */
	(void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
	return pdPASS;
}
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

/* --- esp_heap_caps shim ------------------------------------------------ */
#define MALLOC_CAP_SPIRAM 0x01
#define MALLOC_CAP_8BIT   0x02
static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
	(void)caps; return malloc(size);
}
static inline void heap_caps_free(void *p) { free(p); }

#endif /* FREERTOS_HOST_MOCK_H */
