/* Central packet switch: three independent ring buffers, one per sink.
 *
 * A packet received from any interface is COPIED into the rings of the other
 * two. Each ring is a fixed-size circular buffer allocated in PSRAM.
 *
 * Drop policy is PER-SINK and reflects the physical reality of each link:
 *
 *   BLE, TCP  (drop_ok): overwrite-oldest when full. A slow or disconnected
 *               sink loses its oldest queued data. BLE is best-effort by GATT
 *               nature; TCP can always be retransmitted by Reticulum.
 *
 *   HaLow     (NO drops): packets MUST reach the radio -- losing one means a
 *               dropped Reticulum frame on the air. The HaLow ring NEVER
 *               overwrites: when full, switch_rx() BLOCKS the caller (back-
 *               pressure) until the dispatch task drains a slot. A stuck radio
 *               link therefore stalls BLE/TCP RX tasks, which is correct --
 *               there is no point accepting new packets the radio cannot send.
 *
 * Dispatch for HaLow uses a HELD-PACKET pattern: when halow_sink returns
 * false (radio link down), the packet is kept OUT of the ring (in a static
 * holding slot) and retried every loop until it goes out. This guarantees
 * no HaLow packet is ever lost, regardless of how long the link stays down.
 *
 * switch_rx() is always called from a FreeRTOS task (ble_rx_task,
 * rns_proxy_session_task, halow_link_task), so blocking on a full HaLow ring
 * is safe. Note: a packet FROM the radio (SW_SRC_HALOW) is never pushed to
 * the HaLow ring (it came from there), so the blocking path is only hit by
 * BLE->radio and TCP->radio traffic. */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "switch.h"
#include "rns_framing.h"

static const char *TAG = "switch";

#define SW_RING_SIZE  (32)
#define SW_PKT_MAX    (RNS_FRAMING_MAX_PACKET)   /* 1024 */

typedef struct {
	size_t  len;
	uint8_t data[SW_PKT_MAX];
} sw_slot_t;

typedef struct {
	sw_slot_t        *slots;    /* SW_RING_SIZE entries, PSRAM */
	SemaphoreHandle_t mu;
	int               head;     /* next write index (mod SIZE) */
	int               tail;     /* next read index (mod SIZE) */
	int               count;    /* items currently in ring */
	sw_sink_fn        sink;     /* set once at boot, NULL = no sink */
	bool              drop_ok;  /* true: overwrite-oldest on full;
	                            * false: block caller until space (HaLow) */
} sw_ring_t;

static sw_ring_t s_rings[3];

/* Held packet for the HaLow sink. When halow_sink returns false (radio link
 * down), the packet lives here -- OUT of the ring -- and is retried every
 * dispatch loop. Only the dispatch task touches this (single consumer). */
static sw_slot_t s_halow_held;
static bool      s_halow_holding = false;

/* ------------------------------------------------------------------ */
/* Ring primitives                                                     */
/* ------------------------------------------------------------------ */

/* Try to write @data/@len into ring @r. Returns true on success.
 * If the ring is full:
 *   - drop_ok rings: overwrite the oldest entry (always succeeds).
 *   - non-drop rings (HaLow): returns false -- caller must block & retry. */
static bool ring_push(sw_ring_t *r, const uint8_t *data, size_t len) {
	if (len == 0 || len > SW_PKT_MAX)
		return true;  /* reject silently, treat as "done" */

	xSemaphoreTake(r->mu, portMAX_DELAY);
	if (r->count >= SW_RING_SIZE) {
		if (r->drop_ok) {
			/* Full -- drop oldest by advancing tail. */
			r->tail = (r->tail + 1) % SW_RING_SIZE;
			r->count--;
		} else {
			/* Non-drop ring full -- caller must block. */
			xSemaphoreGive(r->mu);
			return false;
		}
	}
	memcpy(r->slots[r->head].data, data, len);
	r->slots[r->head].len = len;
	r->head = (r->head + 1) % SW_RING_SIZE;
	r->count++;
	xSemaphoreGive(r->mu);
	return true;
}

/* Blocking push for non-drop rings. Spins with vTaskDelay until space is
 * available, applying backpressure on the calling task. */
static void ring_push_blocking(sw_ring_t *r, const uint8_t *data, size_t len) {
	while (!ring_push(r, data, len)) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

/* Pop oldest into @out. Returns false if empty. */
static bool ring_pop(sw_ring_t *r, sw_slot_t *out) {
	xSemaphoreTake(r->mu, portMAX_DELAY);
	if (r->count == 0) {
		xSemaphoreGive(r->mu);
		return false;
	}
	*out = r->slots[r->tail];
	r->tail = (r->tail + 1) % SW_RING_SIZE;
	r->count--;
	xSemaphoreGive(r->mu);
	return true;
}

/* Push back to the tail (front of queue) for drop_ok sinks. Used when a sink
 * reports not-ready: the packet becomes the oldest entry and thus the next
 * overwrite victim when new traffic arrives. If the ring is full we just drop
 * it (it was going to be overwritten anyway). */
static void ring_push_front_drop(sw_ring_t *r, const sw_slot_t *in) {
	xSemaphoreTake(r->mu, portMAX_DELAY);
	if (r->count >= SW_RING_SIZE) {
		xSemaphoreGive(r->mu);
		return;
	}
	r->tail = (r->tail + SW_RING_SIZE - 1) % SW_RING_SIZE;
	r->slots[r->tail] = *in;
	r->count++;
	xSemaphoreGive(r->mu);
}

/* ------------------------------------------------------------------ */
/* Dispatch task                                                       */
/* ------------------------------------------------------------------ */

static void sw_dispatch_task(void *arg) {
	(void)arg;
	/* We ALWAYS vTaskDelay, even after successful work. taskYIELD() is NOT
	 * sufficient: it only round-robins among equal-or-higher priority tasks,
	 * so the priority-0 IDLE task never gets a tick -- and the task-wdt (which
	 * monitors IDLE0) fires after 5 s. One tick = 10 ms at CONFIG_FREERTOS_HZ=100. */
	const TickType_t step = pdMS_TO_TICKS(10);
	for (;;) {
		/* --- HaLow: held-packet pattern (never drops) --- */
		if (s_rings[SW_SRC_HALOW].sink) {
			/* If we're not holding, pull the next packet from the ring. */
			if (!s_halow_holding) {
				if (ring_pop(&s_rings[SW_SRC_HALOW], &s_halow_held))
					s_halow_holding = true;
			}
			/* Try to deliver whatever we're holding (or just pulled). */
			if (s_halow_holding) {
				if (s_rings[SW_SRC_HALOW].sink(s_halow_held.data,
				                               s_halow_held.len)) {
					s_halow_holding = false;  /* gone to the radio */
				}
				/* else: keep holding, retry next loop. The ring has one
				 * fewer packet now, so switch_rx blocking on a full ring
				 * will eventually unblock. */
			}
		}

		/* --- BLE and TCP: best-effort with overwrite --- */
		for (int s = (int)SW_SRC_BLE; s <= (int)SW_SRC_TCP; s++) {
			sw_ring_t *r = &s_rings[s];
			if (!r->sink)
				continue;
			sw_slot_t pkt;
			if (!ring_pop(r, &pkt))
				continue;
			if (r->sink(pkt.data, pkt.len)) {
				/* delivered */
			} else {
				/* not ready: put back at FRONT (may drop if ring filled
				 * while we were calling the sink). */
				ring_push_front_drop(r, &pkt);
			}
		}

		vTaskDelay(step);
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void switch_init(void) {
	static bool inited = false;
	if (inited)
		return;
	inited = true;

	for (int i = 0; i < 3; i++) {
		s_rings[i].slots = heap_caps_malloc(sizeof(sw_slot_t) * SW_RING_SIZE,
		                                    MALLOC_CAP_SPIRAM);
		if (!s_rings[i].slots) {
			ESP_LOGE(TAG, "PSRAM alloc failed for ring %d", i);
			return;
		}
		s_rings[i].mu      = xSemaphoreCreateMutex();
		s_rings[i].head    = 0;
		s_rings[i].tail    = 0;
		s_rings[i].count   = 0;
		s_rings[i].sink    = NULL;
		/* Only HaLow is non-drop: BLE and TCP may lose their oldest packet
		 * when their rings fill up. HaLow blocks the producer instead. */
		s_rings[i].drop_ok = (i != (int)SW_SRC_HALOW);
	}
	s_halow_holding = false;

	/* 8 KB stack: each iteration may hold a full sw_slot_t (~1 KB) on the
	 * stack, plus the sink call chain (kiss_send_data -> ble_write,
	 * system_halow_send -> SLIP encode + send()) consumes more. */
	xTaskCreate(sw_dispatch_task, "sw_dispatch", 8192, NULL, 5, NULL);
	ESP_LOGI(TAG, "ready (3 rings x %d slots; halow=no-drop/backpressure)",
	         SW_RING_SIZE);
}

void switch_rx(sw_src_t src, const uint8_t *data, size_t len) {
	/* Copy into every sink's ring EXCEPT the source's own, and only into
	 * rings that actually have a sink registered (e.g. skip the BLE ring
	 * when BLE is disabled). */
	for (int s = 0; s < 3; s++) {
		if (s == (int)src)
			continue;
		if (!s_rings[s].sink)
			continue;
		if (s_rings[s].drop_ok) {
			ring_push(&s_rings[s], data, len);
		} else {
			/* HaLow: block until space -- backpressure on the caller. */
			ring_push_blocking(&s_rings[s], data, len);
		}
	}
}

void switch_register_ble_sink(sw_sink_fn sink)   { s_rings[SW_SRC_BLE].sink   = sink; }
void switch_register_tcp_sink(sw_sink_fn sink)   { s_rings[SW_SRC_TCP].sink   = sink; }
void switch_register_halow_sink(sw_sink_fn sink) { s_rings[SW_SRC_HALOW].sink = sink; }
