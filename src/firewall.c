/* In-process firewall for the ESP32 web proxy.
 *
 * Problem this solves: the proxy forwards every httpd request to the radio
 * (192.168.7.2 over SLIP), holding TWO sockets (httpd + http_client) per
 * request for up to the upstream timeout. When the radio is slow/unreachable
 * (bcn_timeout, SLIP stall), a browser polling /api/get_stat every second
 * spawns dozens of stuck requests, exhausts the socket pool, and the whole
 * HTTP server collapses with "error in accept (113)" until TCP times out.
 *
 * Bigger socket pools don't fix this — the queue to a dead upstream is
 * effectively unbounded. This firewall caps it at the source instead:
 *
 *   1. Management/OS-probe traffic is always served locally and never
 *      forwarded, so captive-portal + stats + config keep working even
 *      while the radio is down.
 *   2. A per-client rate limit turns a runaway poller into 429s instead
 *      of into a pile of 60-second hangs.
 *   3. A global concurrency cap on upstream-bound requests means at most
 *      N requests talk to the radio at once; the rest get an instant 503
 *      and the client backs off, instead of queuing behind stuck peers.
 *
 * All of this runs inside httpd's worker, BEFORE any upstream socket is
 * opened, so a denied request costs only the accept'd socket — which httpd
 * recycles immediately when the handler returns. */

#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"   /* getpeername, sockaddr_in */

#include "firewall.h"

static const char *TAG = "firewall";

/* --- Tunables ---------------------------------------------------------- */

/* Max simultaneous in-flight upstream (radio-bound) requests. The radio is
 * a SINGLE-THREADED TCP server over SLIP, so we serialize strictly: only one
 * request talks to it at a time. A second request that arrives while one is
 * in flight gets an instant 503 (Retry-After), instead of piling up behind
 * it and burning sockets on both sides. With concurrency=1 a stuck/slow
 * request can never starve the pool — at most ONE upstream socket is held. */
#define FW_MAX_UPSTREAM_CONCURRENCY 1

/* Per-client request rate limit. A client may issue up to this many
 * requests inside the window; further ones get 429. Tuned to absorb a
 * browser's normal burst (stats poll + a few asset fetches) while killing
 * a runaway 1 Hz poller when the radio is down. */
#define FW_PER_CLIENT_MAX      10
#define FW_PER_CLIENT_WINDOW   3000  /* ms */

/* Number of tracked clients. Each entry is small; track the most recent. */
#define FW_CLIENT_SLOTS 8

/* --- Concurrency counter ---------------------------------------------- */

static portMUX_TYPE s_conc_spin = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_upstream_inflight = 0;

void firewall_upstream_release(void) {
	/* Matches a FW_ALLOW for an upstream request. Decrement exactly once;
	 * this is called from the finally-path of handle_proxy. */
	portENTER_CRITICAL(&s_conc_spin);
	if (s_upstream_inflight > 0)
		s_upstream_inflight--;
	portEXIT_CRITICAL(&s_conc_spin);
}

/* --- Per-client rate limiter ------------------------------------------ */

typedef struct {
	bool used;
	bool valid;        /* a hit in this slot */
	uint8_t mac[6];    /* client identity (IP is less stable across captive) */
	int count;         /* requests in current window */
	int64_t window_start; /* esp_timer_get_time() at window start */
} client_slot_t;

static client_slot_t s_clients[FW_CLIENT_SLOTS];
static portMUX_TYPE s_client_spin = portMUX_INITIALIZER_UNLOCKED;

/* Derive a stable-enough per-client key for rate limiting. Cross-referencing
 * the AP association table to a real MAC needs DHCP-lease lookups that aren't
 * worth it here; the client IPv4 is stable for the few-second window we care
 * about (a client's address doesn't roam mid-session on a SoftAP). We pack it
 * into 6 bytes so the slot comparison is uniform. */
static bool peer_mac(httpd_req_t *req, uint8_t mac[6]) {
	int fd = httpd_req_to_sockfd(req);
	if (fd < 0)
		return false;
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);
	if (lwip_getpeername(fd, (struct sockaddr *)&sa, &sl) != 0)
		return false;
	if (sa.sin_family != AF_INET)
		return false;
	memcpy(mac, &sa.sin_addr.s_addr, 4);
	mac[4] = 0xFF;
	mac[5] = 0xFE;
	return true;
}

static client_slot_t *find_or_alloc_slot(const uint8_t mac[6],
                                         int64_t now_us) {
	/* First pass: match. */
	int lru = 0;
	int64_t lru_ts = INT64_MAX;
	for (int i = 0; i < FW_CLIENT_SLOTS; i++) {
		if (s_clients[i].used && s_clients[i].valid &&
		    memcmp(s_clients[i].mac, mac, 6) == 0)
			return &s_clients[i];
		if (s_clients[i].used) {
			/* Track least-recent window start for eviction. */
			if (s_clients[i].window_start < lru_ts) {
				lru_ts = s_clients[i].window_start;
				lru = i;
			}
		} else {
			lru = i;
			lru_ts = -1; /* prefer a free slot */
		}
	}
	/* Evict/claim LRU. */
	client_slot_t *s = &s_clients[lru];
	s->used = true;
	s->valid = true;
	memcpy(s->mac, mac, 6);
	s->count = 0;
	s->window_start = now_us;
	return s;
}

/* --- Allow/deny for the request --------------------------------------- */

/* Local (non-upstream) endpoints. These never talk to the radio and must
 * always be served, even under load / when the radio is down. Match on the
 * URI path prefix. Order matters only for readability. */
static bool is_management_path(const char *uri) {
	/* /api/thalow_* are handled locally (cfg/stats/scan/connect/reset). */
	if (strncmp(uri, "/api/thalow_", 12) == 0)
		return true;
	/* Injected dashboard script. */
	if (strcmp(uri, "/thalow_inject.js") == 0)
		return true;
	return false;
}

static fw_verdict_t send_429(httpd_req_t *req) {
	httpd_resp_set_status(req, "429 Too Many Requests");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Retry-After", "2");
	httpd_resp_send(req, "{\"error\":\"rate_limited\"}", -1);
	return FW_DENY;
}

static fw_verdict_t send_503(httpd_req_t *req) {
	httpd_resp_set_status(req, "503 Service Unavailable");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Retry-After", "2");
	httpd_resp_send(req, "{\"error\":\"radio_busy\"}", -1);
	return FW_DENY;
}

fw_verdict_t firewall_check(httpd_req_t *req) {
	const char *uri = req ? req->uri : NULL;
	if (!uri)
		return FW_ALLOW; /* nothing we can do */

	/* Layer 1: management traffic bypasses the caps entirely. The stats
	 * panel and captive portal must keep working when the radio is down. */
	if (is_management_path(uri))
		return FW_ALLOW;

	/* Layer 2: per-client rate limit. */
	uint8_t key[6];
	bool have_key = peer_mac(req, key);
	if (have_key) {
		int64_t now = esp_timer_get_time();
		portENTER_CRITICAL(&s_client_spin);
		client_slot_t *s = find_or_alloc_slot(key, now);
		int64_t age_ms = (now - s->window_start) / 1000;
		if (age_ms > FW_PER_CLIENT_WINDOW) {
			/* New window. */
			s->window_start = now;
			s->count = 1;
		} else {
			s->count++;
		}
		int count = s->count;
		portEXIT_CRITICAL(&s_client_spin);

		if (count > FW_PER_CLIENT_MAX) {
			return send_429(req);
		}
	}

	/* Layer 3: global upstream concurrency cap. */
	int inflight;
	portENTER_CRITICAL(&s_conc_spin);
	if (s_upstream_inflight >= FW_MAX_UPSTREAM_CONCURRENCY) {
		portEXIT_CRITICAL(&s_conc_spin);
		return send_503(req);
	}
	s_upstream_inflight++;
	inflight = s_upstream_inflight;
	portEXIT_CRITICAL(&s_conc_spin);

	(void)inflight;
	return FW_ALLOW;
}

void firewall_init(void) {
	static bool inited = false;
	if (inited)
		return;
	inited = true;
	memset(s_clients, 0, sizeof(s_clients));
	s_upstream_inflight = 0;
	ESP_LOGI(TAG, "ready (max_upstream=%d, per_client=%d/%dms)",
	         FW_MAX_UPSTREAM_CONCURRENCY,
	         FW_PER_CLIENT_MAX, FW_PER_CLIENT_WINDOW);
}
