#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Central packet switch: three independent ring buffers, one per sink.
 *
 * The ESP32 has three "client" interfaces -- BLE (NUS/KISS), TCP (a single
 * Reticulum client over WiFi) and the HaLow radio link (RNS-framed over a
 * persistent SLIP socket). A packet received from any one of them is COPIED
 * into the rings of the other two.
 *
 * Each sink owns its own pre-allocated circular buffer in PSRAM (no shared
 * pool, no refcounting, no cross-sink coupling). Drop policy is PER-SINK and
 * reflects the physical reality of each link:
 *
 *   BLE, TCP  (drop-ok): overwrite the oldest entry when the ring is full. A
 *               slow or disconnected sink loses its oldest queued data -- it
 *               never blocks its siblings or the source. BLE is best-effort
 *               by GATT nature; TCP can always be retransmitted by Reticulum.
 *
 *   HaLow     (NO drops):  packets MUST reach the radio. The HaLow ring never
 *               overwrites: when full, switch_rx() BLOCKS the caller
 *               (back-pressure) until the dispatch task drains a slot. A stuck
 *               radio link therefore stalls the BLE and TCP RX tasks -- which
 *               is correct, because there is no point accepting new packets
 *               the radio cannot send. On the dispatch side, a packet that
 *               halow_sink rejects (returns false, radio link down) is held
 *               out of the ring in a static holding slot and retried every
 *               loop until it is sent. No HaLow packet is ever lost. */

typedef enum {
	SW_SRC_BLE,    /* packet came in from a BLE client */
	SW_SRC_TCP,    /* packet came in from the TCP client */
	SW_SRC_HALOW,  /* packet came in from the radio over the HaLow link */
} sw_src_t;

/* One-time init: allocates the three PSRAM rings and spawns the dispatch
 * task. Idempotent -- safe to call once at boot. */
void switch_init(void);

/* Hand a received RNS *payload* (already de-framed -- NOT the raw HDLC
 * stream) to the switch. The switch fans it out to every interface EXCEPT
 * @src by COPYING it into each target sink's ring.
 *
 * @data is copied, so the caller keeps ownership.
 *
 * BLOCKING: if the HaLow ring is full (radio link has been down for a while
 * and 32 packets are queued), this function blocks until a slot drains.
 * Therefore it must be called from a FreeRTOS task (not an ISR). BLE/TCP
 * rings never block -- they overwrite their oldest entry instead. */
void switch_rx(sw_src_t src, const uint8_t *data, size_t len);

/* --- Sink registrations -----------------------------------------------
 *
 * A sink is called from the switch's dispatch task with an RNS payload to
 * push out on its interface. The sink returns true once it has taken
 * responsibility for delivery (so the switch drops its copy); false to keep
 * the packet queued.
 *
 * For BLE/TCP: a false return pushes the packet back to the front of the
 * sink's ring (it becomes the next overwrite victim if the ring fills).
 * For HaLow: a false return RETAINS the packet in a holding slot and retries
 * every loop -- it is NEVER dropped.
 *
 * Sinks are registered once at boot from system.c (BLE + HaLow) and
 * rns_proxy.c (TCP). Registration is not thread-safe against switch_rx --
 * callers must register before the interfaces start producing packets. */

typedef bool (*sw_sink_fn)(const uint8_t *data, size_t len);

void switch_register_ble_sink(sw_sink_fn sink);
void switch_register_tcp_sink(sw_sink_fn sink);
void switch_register_halow_sink(sw_sink_fn sink);

#ifdef __cplusplus
}
#endif
