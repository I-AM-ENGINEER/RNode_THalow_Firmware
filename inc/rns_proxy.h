#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the RNS (Reticulum) TCP proxy.
 *
 * Spawns a listener task that accepts TCP connections on RNS_PROXY_PORT
 * (bound to all interfaces) and forwards each one as a raw byte-stream pipe
 * to the radio's RNS TCP interface at SLIP_PEER_IP:RNS_PROXY_UPSTREAM_PORT.
 * This lets a WiFi/STA client use the RNode as a Reticulum TCP interface,
 * alongside the existing BLE path.
 *
 * Up to RNS_PROXY_MAX_CLIENTS are served concurrently; idle connections are
 * closed after RNS_PROXY_IDLE_TIMEOUT_MS. Idempotent: safe to call once at
 * boot. */
void rns_proxy_init(void);

/* Re-read config (enable / port / whitelist) and re-launch the listener with
 * the new settings WITHOUT a reboot. If the proxy is being disabled, the
 * listener is stopped; if it is being enabled, it is started. Existing
 * in-flight client sessions drain on their own idle timeout. */
void rns_proxy_restart(void);

/* Number of currently-connected RNS proxy clients (for stats display). */
int rns_proxy_client_count(void);

/* Most recent (or current) inbound client as "IP:port", or "no connection".
 * Updated on every accepted connection; persists after disconnect so the
 * web stats panel has something to show. Writes dst which must be >= 32. */
void rns_proxy_last_client(char *dst, size_t sz);

#ifdef __cplusplus
}
#endif
