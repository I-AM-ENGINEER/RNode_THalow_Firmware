/* RNS (Reticulum) TCP client listener.
 *
 * Listens on a configurable port (default RNS_PROXY_PORT = 4242, all
 * interfaces) for a single inbound TCP Reticulum client at a time. Each
 * accepted connection is fed through an RNS HDLC deframer and complete
 * frames are handed to the central switch (switch_rx(SW_SRC_TCP, ...)) which
 * fans them out to the BLE and radio interfaces. Outbound packets for this
 * client (coming from the switch via the tcp_sink) are RNS-encoded and
 * written on the same accepted socket.
 *
 * IMPORTANT: this module no longer opens an upstream connection to the
 * radio. The radio's own TCP bridge at 192.168.7.2:4242 is served exclusively
 * by system.c's halow_link_task -- the ESP32 acts as a pure switch between
 * its three interfaces, not a TCP relay. This avoids the radio's
 * single-client tcp_server being contended by two independent sockets, and
 * means a packet from the radio needs to traverse the switch only once.
 *
 * The TCP client slot is a single global (s_tcp_client_fd). A second
 * connecting client replaces the first -- the radio link is single-client
 * anyway, and the switch's per-sink FIFO retains any undelivered packets
 * for delivery to the next client. */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

#include "rns_proxy.h"
#include "rns_framing.h"
#include "switch.h"
#include "thalow_config.h"
#include "config/project_config.h"

static const char *TAG = "rns_proxy";

/* Single TCP client slot. -1 when nobody is connected. Protected by
 * s_client_mu. The accept loop updates it; the tcp_sink reads it. */
static int s_tcp_client_fd = -1;
static SemaphoreHandle_t s_client_mu;

/* Last accepted client as a "IP:port" string. Persists after disconnect so the
 * stats panel has something to show between connections. "no connection"
 * until the first client ever connects. */
static char s_last_client[32] = "no connection";

int rns_proxy_client_count(void) {
	int fd;
	xSemaphoreTake(s_client_mu, portMAX_DELAY);
	fd = s_tcp_client_fd;
	xSemaphoreGive(s_client_mu);
	return fd >= 0 ? 1 : 0;
}

void rns_proxy_last_client(char *dst, size_t sz) {
	if (!dst || sz == 0)
		return;
	/* strncpy is fine: s_last_client is always NUL-terminated and < 32. */
	strncpy(dst, s_last_client, sz - 1);
	dst[sz - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* CIDR whitelist                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t addr;  /* network byte order */
	uint32_t mask;  /* host byte order, contiguous 1-bits */
	bool valid;
} cidr_t;

#define RNS_WL_MAX_ENTRIES 8

/* Parse "addr/prefix" into network-byte-order addr + host-byte-order mask.
 * Accepts bare IPs (treated as /32). Returns true on success. */
static bool parse_cidr(const char *s, cidr_t *out) {
	if (!s || !*s || !out)
		return false;

	char tmp[48];
	size_t sl = strlen(s);
	if (sl >= sizeof(tmp))
		return false;
	memcpy(tmp, s, sl + 1);

	char *slash = strchr(tmp, '/');
	uint8_t prefix = 32;
	if (slash) {
		*slash = '\0';
		long p = strtol(slash + 1, NULL, 10);
		if (p < 0 || p > 32)
			return false;
		prefix = (uint8_t)p;
	}

	ip4_addr_t ip;
	if (ip4addr_aton(tmp, &ip) != 1)
		return false;

	uint32_t host_mask = (prefix == 0) ? 0
	                   : (prefix >= 32) ? 0xFFFFFFFFu
	                   : (0xFFFFFFFFu << (32 - prefix));

	out->addr = ip.addr;          /* network byte order */
	out->mask = host_mask;        /* host byte order */
	out->valid = true;
	return true;
}

static bool ip_in_cidr(uint32_t peer_nbo, const cidr_t *c) {
	if (!c || !c->valid)
		return false;
	uint32_t mask_nbo = lwip_htonl(c->mask);
	return ((peer_nbo ^ c->addr) & mask_nbo) == 0;
}

static bool peer_allowed(uint32_t peer_nbo) {
	const char *wl = thalow_config_get_rns_proxy_whitelist();
	if (!wl || !*wl)
		return true;  /* empty = allow all */

	cidr_t entries[RNS_WL_MAX_ENTRIES];
	int n = 0;

	const char *p = wl;
	while (*p && n < RNS_WL_MAX_ENTRIES) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',' && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - start);
		if (len > 0 && len < 48) {
			char tok[48];
			memcpy(tok, start, len);
			tok[len] = '\0';
			if (parse_cidr(tok, &entries[n]))
				n++;
		}
	}

	for (int i = 0; i < n; i++) {
		if (ip_in_cidr(peer_nbo, &entries[i]))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Per-session RNS deframer                                            */
/* ------------------------------------------------------------------ */

/* The session task owns its own rns_framing_t and feeds every byte the
 * socket returns into it; each complete HDLC frame becomes a switch_rx
 * call. This is what the radio would have done had the TCP client been
 * connected directly to it. */
typedef struct {
	int           client_fd;
	rns_framing_t deframer;
} session_ctx_t;

/* rns_framing frame callback: pushes the decoded payload into the switch as
 * a TCP-origin packet. The switch then fans it out to BLE + radio. */
static void on_session_frame(void *user, const uint8_t *data, size_t len) {
	(void)user;
	switch_rx(SW_SRC_TCP, data, len);
}

/* ------------------------------------------------------------------ */
/* TCP sink                                                            */
/* ------------------------------------------------------------------ */

/* Outbound path: the switch invokes this with an RNS payload destined for
 * the TCP client. We RNS-encode it and write it on the current client_fd.
 * Returns true if the bytes were pushed onto the socket (TCP's in-window
 * delivery then handles reliability); false if there is no client or the
 * write failed, in which case the switch keeps the packet queued for the
 * next dispatch attempt. */
static bool tcp_sink(const uint8_t *data, size_t len) {
	int fd;
	xSemaphoreTake(s_client_mu, portMAX_DELAY);
	fd = s_tcp_client_fd;
	xSemaphoreGive(s_client_mu);
	if (fd < 0)
		return false;  /* no client -- keep queued */

	uint8_t enc[RNS_FRAMING_MAX_PACKET * 2 + 4];
	int enc_len = rns_framing_encode(data, len, enc, sizeof(enc));
	if (enc_len < 0)
		return true;  /* unencodable -- drop, don't retry forever */

	/* Short send timeout so a stuck client can't block the dispatch task. */
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	int sent = send(fd, enc, enc_len, 0);
	if (sent != enc_len) {
		ESP_LOGW(TAG, "tcp_sink send failed (sent=%d want=%d errno=%d) -- "
		         "client will be dropped by the recv loop",
		         sent, enc_len, errno);
		return false;  /* keep queued, retry after the dead client is gone */
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Session task (one per accepted client)                              */
/* ------------------------------------------------------------------ */

static void rns_proxy_session_task(void *arg) {
	session_ctx_t *ctx = (session_ctx_t *)arg;
	int client_fd = ctx->client_fd;

	ESP_LOGI(TAG, "tcp client accepted fd=%d [%s]", client_fd, s_last_client);

	uint8_t rx_buf[RNS_PROXY_BUF_SZ];
	for (;;) {
		int n = recv(client_fd, rx_buf, sizeof(rx_buf), 0);
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++)
			rns_framing_rx_byte(&ctx->deframer, rx_buf[i]);
	}

	/* Detach the slot so the switch stops handing packets to this fd. */
	xSemaphoreTake(s_client_mu, portMAX_DELAY);
	if (s_tcp_client_fd == client_fd)
		s_tcp_client_fd = -1;
	xSemaphoreGive(s_client_mu);

	close(client_fd);
	heap_caps_free(ctx);
	ESP_LOGI(TAG, "tcp client gone fd=%d", client_fd);
	vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Listener with live-restart support                                 */
/* ------------------------------------------------------------------ */

/* s_restart_req is set by rns_proxy_restart() to ask the running listener
 * task to exit so a new one can be spawned with fresh config. The listener
 * polls it every RESTART_POLL_MS via select() timeout on accept(). */
static volatile bool s_restart_req = false;
static TaskHandle_t s_listen_task = NULL;
#define RESTART_POLL_MS  250

static void rns_proxy_listen_task(void *arg) {
	(void)arg;

	int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_fd < 0) {
		ESP_LOGE(TAG, "listen socket failed");
		s_listen_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	int yes = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	uint16_t port = thalow_config_get_rns_proxy_port();
	if (port == 0)
		port = RNS_PROXY_PORT;

	struct sockaddr_in bind_addr;
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = PP_HTONS(port);
	bind_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
		ESP_LOGE(TAG, "bind :%u failed (errno %d)", port, errno);
		close(listen_fd);
		s_listen_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	if (listen(listen_fd, 1) != 0) {
		ESP_LOGE(TAG, "listen failed (errno %d)", errno);
		close(listen_fd);
		s_listen_task = NULL;
		vTaskDelete(NULL);
		return;
	}

	const char *wl = thalow_config_get_rns_proxy_whitelist();
	ESP_LOGI(TAG, "RNS TCP proxy listening on :%u (single client, wl=%s)",
	         port, (wl && *wl) ? wl : "ANY");

	for (;;) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(listen_fd, &rfds);
		struct timeval tv = { .tv_sec = 0, .tv_usec = RESTART_POLL_MS * 1000 };
		int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);

		if (s_restart_req) {
			ESP_LOGI(TAG, "restart requested, closing listener");
			break;
		}

		if (sel < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (sel == 0)
			continue;

		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		if (!peer_allowed(peer.sin_addr.s_addr)) {
			ESP_LOGW(TAG, "client %s:%d rejected by whitelist",
			         inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
			close(client_fd);
			continue;
		}

		/* If a client is already connected, drop it -- radio link is
		 * single-client, and the switch's FIFO will deliver any queued
		 * packets to the new client. */
		int prev_fd;
		xSemaphoreTake(s_client_mu, portMAX_DELAY);
		prev_fd = s_tcp_client_fd;
		s_tcp_client_fd = client_fd;
		xSemaphoreGive(s_client_mu);
		if (prev_fd >= 0) {
			ESP_LOGI(TAG, "replacing previous client fd=%d", prev_fd);
			/* shutdown() wakes the session task's recv(); it then closes
			 * the fd and detaches itself. */
			shutdown(prev_fd, SHUT_RDWR);
			close(prev_fd);
		}

		snprintf(s_last_client, sizeof(s_last_client), "%s:%d",
		         inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

		session_ctx_t *ctx = heap_caps_malloc(sizeof(*ctx),
		                                      MALLOC_CAP_SPIRAM);
		if (!ctx) {
			ESP_LOGE(TAG, "session ctx alloc failed");
			close(client_fd);
			xSemaphoreTake(s_client_mu, portMAX_DELAY);
			if (s_tcp_client_fd == client_fd)
				s_tcp_client_fd = -1;
			xSemaphoreGive(s_client_mu);
			continue;
		}
		ctx->client_fd = client_fd;
		rns_framing_init(&ctx->deframer, on_session_frame, NULL);

		char task_name[16];
		snprintf(task_name, sizeof(task_name), "rns_c%d", client_fd);
		if (xTaskCreate(rns_proxy_session_task, task_name, 4096, ctx, 5, NULL)
		    != pdPASS) {
			ESP_LOGE(TAG, "failed to create session task");
			heap_caps_free(ctx);
			close(client_fd);
			xSemaphoreTake(s_client_mu, portMAX_DELAY);
			if (s_tcp_client_fd == client_fd)
				s_tcp_client_fd = -1;
			xSemaphoreGive(s_client_mu);
		}
	}

	close(listen_fd);
	s_listen_task = NULL;
	vTaskDelete(NULL);
}

static void start_listener(void) {
	s_restart_req = false;
	if (xTaskCreate(rns_proxy_listen_task, "rns_listen", 4096, NULL, 5,
	                &s_listen_task) != pdPASS) {
		ESP_LOGE(TAG, "failed to create listener task");
		s_listen_task = NULL;
	}
}

void rns_proxy_init(void) {
	static bool started = false;
	if (started)
		return;
	started = true;

	s_client_mu = xSemaphoreCreateMutex();

	/* Tell the switch where to send packets destined for the TCP client. */
	switch_register_tcp_sink(tcp_sink);

	if (!thalow_config_get_rns_proxy_enabled()) {
		ESP_LOGI(TAG, "RNS TCP proxy disabled by config");
		return;
	}

	start_listener();
}

/* Re-apply the RNS proxy config (enable / port / whitelist) without a reboot.
 * See rns_proxy.h for details. */
void rns_proxy_restart(void) {
	TaskHandle_t prev = s_listen_task;

	if (prev) {
		s_restart_req = true;
		for (int i = 0; i < 50 && s_listen_task != NULL; i++) {
			vTaskDelay(pdMS_TO_TICKS(20));
		}
		if (s_listen_task != NULL) {
			ESP_LOGW(TAG, "listener did not exit in time; forcing restart anyway");
		}
	}

	if (!thalow_config_get_rns_proxy_enabled()) {
		ESP_LOGI(TAG, "RNS TCP proxy disabled by config (listener stopped)");
		return;
	}

	start_listener();
}
