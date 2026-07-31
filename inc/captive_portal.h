#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Start the captive-portal DNS responder (idempotent). */
void captive_portal_init(void);

/* True when the SoftAP is up and has an IP, i.e. we should serve a
 * captive-portal redirect to clients associated with our AP. */
bool captive_portal_is_active(void);

/* True when the incoming request looks like an OS captive-portal probe
 * (matches a known probe host and/or path). */
bool captive_portal_match(httpd_req_t *req);

/* True when the request is a captive-portal LANDING page (the URL the OS
 * opens in its captive-portal mini-browser after detection). These need a
 * real HTTP 302 to the setup page, unlike probes which must NOT redirect. */
bool captive_portal_is_landing(httpd_req_t *req);

/* Answer a captive-portal probe with a 200 (no HTTP redirect, to avoid
 * ERR_TOO_MANY_REDIRECTS on Android). */
esp_err_t captive_portal_redirect(httpd_req_t *req);

/* Answer a captive-portal landing page with a 302 to the setup page. */
esp_err_t captive_portal_landing_redirect(httpd_req_t *req);

/* True when the request is NOT addressed to our own AP IP / hostname — i.e.
 * it's an internet-bound request from a client app (analytics, Play
 * Services, ...). While the captive portal is active there is no internet,
 * so proxying these to the radio only wastes sockets and stalls the client.
 * Returns false (treat as "ours") if the portal is not active. */
bool captive_portal_is_foreign_host(httpd_req_t *req);

/* Universal handler for any foreign-host request while captive: probes get
 * 200 (reuse the probe body), everything else gets a 302 to the setup page.
 * Always returns ESP_OK (a response was sent). */
esp_err_t captive_portal_intercept(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
