#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_http_server.h"

/* Request-policy verdict returned by the firewall. The caller (web_proxy)
 * MUST honour it before touching the upstream radio. */
typedef enum {
	/* Request is allowed; proceed to normal proxy/stat handling. */
	FW_ALLOW = 0,
	/* Request was already answered by the firewall (rate-limit 429 or
	 * concurrency-cap 503). The handler must return immediately without
	 * doing any further work. */
	FW_DENY,
} fw_verdict_t;

/* Idempotent init. Must be called once before firewall_check(). */
void firewall_init(void);

/* Inspect an incoming httpd request and decide whether to serve it.
 *
 * Layers (evaluated in order, cheapest first):
 *   1. Bypass for our own management traffic (ESP32 statistics panel,
 *      captive-portal probe/landing handlers, injected JS, scan/connect
 *      config endpoints). These never hit the radio and must always work.
 *   2. Per-client request rate limit. A single client that floods the
 *      proxy (e.g. a browser tab left open polling every second while the
 *      radio is down) gets 429s instead of spawning dozens of upstream
 *      connections that each hang for the full timeout.
 *   3. Global concurrency cap on upstream-bound requests. The radio is a
 *      single SLIP link; parallel requests only contend and pile up. Past
 *      the cap, excess requests get an instant 503 so the client retries
 *      later instead of queuing behind N stuck ones.
 *
 * On FW_DENY the response has already been sent to the client; the handler
 * must just return. */
fw_verdict_t firewall_check(httpd_req_t *req);

/* Must bracket any upstream-bound request that passed firewall_check()
 * with FW_ALLOW, so the concurrency counter is released when the request
 * finishes (success or failure). For management/local requests this is a
 * no-op. */
void firewall_upstream_release(void);

#ifdef __cplusplus
}
#endif
