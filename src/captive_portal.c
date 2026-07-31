#include <string.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "captive_portal.h"
#include "wifi.h"
#include "thalow_config.h"
#include "config/project_config.h"

static const char *TAG = "captive";

/* Known captive-portal probe hostnames (case-insensitive). */
static const char *PROBE_HOSTS[] = {
	"captive.apple.com",
	"www.apple.com",
	"apple.com",
	"connectivitycheck.gstatic.com",
	"www.google.com",
	"clients3.google.com",
	"play.googleapis.com",
	"connectivitycheck.android.com",
	"www.msftconnecttest.com",
	"www.msftncsi.com",
	"msftncsi.com",
	"detectportal.firefox.com",
	NULL
};

/* Known captive-portal probe paths.
 *
 * NOTE: "/redirect" is intentionally NOT here — on Windows it is the NCSI
 * landing page that the Network Login assistant opens AFTER detection, not
 * a connectivity probe. Landing pages need a real HTTP 302 (their WebView
 * ignores <meta http-equiv=refresh>); see captive_portal_is_landing(). */
static const char *PROBE_PATHS[] = {
	"/generate_204",
	"/gen_204",
	"/hotspot-detect.html",
	"/success.html",
	"/ncsi.txt",
	"/connecttest.txt",
	"/check_network_status.txt",
	"/kindle-wifi/wifistub.html",
	NULL
};

/* Landing pages: opened by the OS's captive-portal browser AFTER it has
 * detected a captive portal via a probe. These must be answered with a real
 * HTTP 302 to the setup page (the OS-provided mini-browser does not run
 * JavaScript or honour <meta http-equiv=refresh> the way a full browser
 * does, so a 200-with-refresh body leaves the user stuck on the landing
 * URL). Windows opens http://www.msftconnecttest.com/redirect here. */
static const char *LANDING_PATHS[] = {
	"/redirect",
	NULL
};

bool captive_portal_is_active(void) {
	if (!thalow_config_get_wifi_ap_enabled())
		return false;
	esp_netif_ip_info_t ip;
	if (wifi_get_ap_ip_info(&ip) != ESP_OK || ip.ip.addr == 0)
		return false;
	return true;
}

/* Extract the path (without query string) from the request URI. Returns
 * false if the request is NULL or the path doesn't fit. */
static bool req_path(const httpd_req_t *req, char *out, size_t outsz) {
	if (!req || !out || outsz == 0)
		return false;
	const char *uri = req->uri;
	const char *q = strchr(uri, '?');
	size_t plen = q ? (size_t)(q - uri) : strlen(uri);
	if (plen >= outsz)
		plen = outsz - 1;
	memcpy(out, uri, plen);
	out[plen] = '\0';
	return true;
}

/* True if the request's Host header matches a known captive-portal probe
 * hostname (case-insensitive, port stripped). */
static bool req_probe_host(const httpd_req_t *req) {
	char host[160] = {0};
	if (httpd_req_get_hdr_value_str((httpd_req_t *)req, "Host", host,
	                                sizeof(host)) != ESP_OK || !host[0])
		return false;
	char *colon = strchr(host, ':');
	if (colon)
		*colon = '\0';
	for (int i = 0; PROBE_HOSTS[i]; i++) {
		if (strcasecmp(host, PROBE_HOSTS[i]) == 0)
			return true;
	}
	return false;
}

static bool path_in_list(const char *path, const char *const *list) {
	for (int i = 0; list[i]; i++) {
		if (strcasecmp(path, list[i]) == 0)
			return true;
	}
	return false;
}

bool captive_portal_match(httpd_req_t *req) {
	if (!req)
		return false;
	char path[160];
	if (!req_path(req, path, sizeof(path)))
		return false;
	/* A probe is a known probe HOST carrying a known probe PATH. Require
	 * both — matching on host OR path alone is too greedy and would funnel
	 * real traffic into the probe handler. */
	if (!req_probe_host(req))
		return false;
	return path_in_list(path, PROBE_PATHS);
}

bool captive_portal_is_landing(httpd_req_t *req) {
	if (!req)
		return false;
	char path[160];
	if (!req_path(req, path, sizeof(path)))
		return false;
	if (!req_probe_host(req))
		return false;
	return path_in_list(path, LANDING_PATHS);
}

/* Build the setup page URL "http://<ap-ip>/" for the current SoftAP IP. */
static void build_portal_url(char *out, size_t outsz) {
	esp_netif_ip_info_t ipinfo;
	if (wifi_get_ap_ip_info(&ipinfo) == ESP_OK && ipinfo.ip.addr != 0) {
		snprintf(out, outsz, "http://" IPSTR "/", IP2STR(&ipinfo.ip));
	} else {
		ip4_addr_t def = { .addr = WIFI_AP_IP };
		snprintf(out, outsz, "http://" IPSTR "/", IP2STR(&def));
	}
}

esp_err_t captive_portal_redirect(httpd_req_t *req) {
	/* Answer an OS connectivity probe WITHOUT an HTTP 3xx redirect.
	 *
	 * Replying 302 here is what causes ERR_TOO_MANY_REDIRECTS on Android:
	 * the CaptivePortalLogin WebView chases our 302 to the setup page, then
	 * NetworkMonitor re-validates by hitting the probe again, gets another
	 * 302, and the browser gives up after ~20 hops.
	 *
	 * Every OS treats its probe as "online" only for one specific verdict:
	 *   Android/Chrome  -> 204 No Content on /generate_204
	 *   Apple           -> body "<HTML>...<TITLE>Success</TITLE>..." on
	 *                       /hotspot-detect.html
	 *   Windows (NCSI)  -> body "Microsoft Connect Test" on /connecttest.txt
	 * Returning anything else (a 200 with arbitrary body) is the universal
	 * "you are behind a captive portal" signal, and it carries no Location
	 * header so there is no HTTP redirect to loop on. DHCP option 114
	 * (published in wifi.c) is the primary trigger on Win10+/Android11+/
	 * iOS14+, which open the setup page at http://<ap-ip>/ with no probing.
	 * For older clients the body below <meta http-refresh>-es the WebView to
	 * that same URL in a single client-side navigation. */
	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_type(req, "text/html");

	char url[64];
	build_portal_url(url, sizeof(url));

	httpd_resp_set_hdr(req, "Cache-Control",
	                   "no-store, no-cache, must-revalidate");
	httpd_resp_set_hdr(req, "Pragma", "no-cache");

	char body[384];
	snprintf(body, sizeof(body),
	         "<!DOCTYPE html><html><head>"
	         "<meta charset=\"utf-8\">"
	         "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
	         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	         "<title>Network setup</title></head>"
	         "<body>Redirecting to network setup…</body></html>", url);

	ESP_LOGI(TAG, "probe %s -> 200 (portal %s)", req->uri, url);
	return httpd_resp_send(req, body, -1);
}

esp_err_t captive_portal_landing_redirect(httpd_req_t *req) {
	/* A landing page is what the OS opens in its captive-portal
	 * mini-browser AFTER it has already detected a captive portal via a
	 * probe. Windows NCSI opens http://www.msftconnecttest.com/redirect
	 * here. The OS-provided mini-browser does NOT run JavaScript or honour
	 * <meta http-equiv=refresh>, so a 200-with-refresh body leaves the user
	 * stuck on the landing URL — we must answer with a real HTTP 302.
	 *
	 * This is loop-safe: the OS only re-evaluates captivity by re-probing
	 * (/connecttest.txt etc.), which is handled by captive_portal_match()
	 * and answered with a 200. A landing URL is fetched once per detection,
	 * never re-probed, so the single 302 below cannot loop. */
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_type(req, "text/html");

	char url[64];
	build_portal_url(url, sizeof(url));

	httpd_resp_set_hdr(req, "Location", url);
	httpd_resp_set_hdr(req, "Cache-Control",
	                   "no-store, no-cache, must-revalidate");
	httpd_resp_set_hdr(req, "Pragma", "no-cache");

	char body[320];
	snprintf(body, sizeof(body),
	         "<!DOCTYPE html><html><head>"
	         "<meta charset=\"utf-8\">"
	         "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
	         "<title>Network setup</title></head>"
	         "<body>Redirecting… <a href=\"%s\">Network setup</a></body></html>",
	         url, url);

	ESP_LOGI(TAG, "landing %s -> 302 %s", req->uri, url);
	return httpd_resp_send(req, body, -1);
}

/* Get the request's Host header (port stripped) into `out`. Returns false
 * if absent or too long. */
static bool req_host(const httpd_req_t *req, char *out, size_t outsz) {
	if (httpd_req_get_hdr_value_str((httpd_req_t *)req, "Host", out, outsz)
	    != ESP_OK || !out[0])
		return false;
	char *colon = strchr(out, ':');
	if (colon)
		*colon = '\0';
	return true;
}

bool captive_portal_is_foreign_host(httpd_req_t *req) {
	if (!req || !captive_portal_is_active())
		return false;

	char host[160];
	if (!req_host(req, host, sizeof(host)))
		return false; /* no Host — treat as ours, let normal handling deal */

	/* Our own addresses: the AP IP, the STA IP, and the mDNS hostname.
	 * Anything else is foreign. */
	char ipbuf[16];
	esp_netif_ip_info_t ipinfo;
	if (wifi_get_ap_ip_info(&ipinfo) == ESP_OK && ipinfo.ip.addr != 0) {
		esp_ip4addr_ntoa(&ipinfo.ip, ipbuf, sizeof(ipbuf));
		if (strcasecmp(host, ipbuf) == 0)
			return false;
	}
	/* The STA uplink IP (e.g. 192.168.1.59): a request addressed here
	 * arrives from the upstream network, where the client already HAS
	 * internet through the router. It is not behind our SoftAP and must
	 * never be captive-intercepted — redirecting it to the AP IP
	 * (10.10.0.2, an unroutable subnet from there) just breaks the UI. */
	if (wifi_get_sta_ip_info(&ipinfo) == ESP_OK && ipinfo.ip.addr != 0) {
		esp_ip4addr_ntoa(&ipinfo.ip, ipbuf, sizeof(ipbuf));
		if (strcasecmp(host, ipbuf) == 0)
			return false;
	}
	const char *hn = wifi_get_hostname();
	if (hn[0] && strcasecmp(host, hn) == 0)
		return false;
	return true;
}

esp_err_t captive_portal_intercept(httpd_req_t *req) {
	/* A request to a non-our host while captive. Probes are answered with
	 * the loop-free 200 body (keeps OS detection consistent); everything
	 * else gets a single 302 to the setup page. Either way the response is
	 * instant and consumes no upstream socket. */
	if (captive_portal_match(req))
		return captive_portal_redirect(req);
	return captive_portal_landing_redirect(req);
}

/* --- Captive-portal DNS responder ----------------------------------------
 * Answers every A query with the SoftAP IP so that associated clients are
 * forced to our HTTP server, which then issues a captive-portal redirect.
 */
static void dns_task(void *arg) {
	(void)arg;

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		ESP_LOGE(TAG, "dns socket failed");
		return;
	}
	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(53);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		ESP_LOGE(TAG, "dns bind :53 failed");
		close(sock);
		return;
	}
	ESP_LOGI(TAG, "captive DNS responder on :53");

	uint8_t buf[512];
	struct sockaddr_in client;
	socklen_t clen = sizeof(client);

	for (;;) {
		int n = recvfrom(sock, buf, sizeof(buf), 0,
		                 (struct sockaddr *)&client, &clen);
		if (n <= 0) {
			/* Avoid busy-spin on persistent errors (ENOMEM etc). recvfrom
			 * is blocking by default so this is normally a no-op, but a
			 * non-fatal persistent error must not starve the watchdog. */
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		if (n < 12)
			continue;

		uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
		if (flags & 0x8000)
			continue; /* not a query */
		uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
		if (qd == 0)
			continue;

		/* Locate the end of the first question section. */
		int qoff = 12;
		while (qoff < n) {
			uint8_t len = buf[qoff];
			if (len == 0) {
				qoff += 1;
				break;
			}
			if ((len & 0xC0) == 0xC0) {
				qoff += 2;
				break;
			}
			qoff += len + 1;
		}
		if (qoff + 4 > n)
			continue;
		int qend = qoff + 4;

		uint32_t apip = WIFI_AP_IP; /* default 10.10.0.2 */
		esp_netif_ip_info_t ipinfo;
		if (wifi_get_ap_ip_info(&ipinfo) == ESP_OK &&
		    ipinfo.ip.addr != 0)
			apip = ipinfo.ip.addr;

		uint8_t resp[512];
		memcpy(resp, buf, 12); /* id + counts */

		uint16_t rflags = (uint16_t)((buf[2] << 8) | buf[3]);
		rflags |= 0x8400; /* QR=1, AA=1 */
		resp[2] = (uint8_t)(rflags >> 8);
		resp[3] = (uint8_t)(rflags & 0xFF);

		resp[4] = buf[4]; /* QDCOUNT */
		resp[5] = buf[5];
		resp[6] = 0;      /* ANCOUNT = 1 (first question) */
		resp[7] = 1;
		resp[8] = 0;      /* NSCOUNT */
		resp[9] = 0;
		resp[10] = 0;     /* ARCOUNT */
		resp[11] = 0;

		/* Bounds check BEFORE writing: question copy + 16-byte answer
		 * record must fit in resp[]. A crafted query with a long question
		 * section could overflow the 512-byte buffer. */
		int qcopy = qend - 12;
		if (12 + qcopy + 16 > (int)sizeof(resp))
			continue;  /* question too long, skip */

		int rl = 12;
		memcpy(resp + rl, buf + 12, (size_t)qcopy);
		rl += qcopy;

		resp[rl++] = 0xC0; /* name: pointer to 0x0C */
		resp[rl++] = 0x0C;
		resp[rl++] = 0x00; /* type A */
		resp[rl++] = 0x01;
		resp[rl++] = 0x00; /* class IN */
		resp[rl++] = 0x01;
		resp[rl++] = 0x00; /* TTL 60s */
		resp[rl++] = 0x00;
		resp[rl++] = 0x00;
		resp[rl++] = 60;
		resp[rl++] = 0x00; /* RDLENGTH 4 */
		resp[rl++] = 0x04;
		memcpy(resp + rl, &apip, 4); /* RDATA = AP IP */
		rl += 4;

		sendto(sock, resp, (size_t)rl, 0,
		       (struct sockaddr *)&client, clen);
	}
}

void captive_portal_init(void) {
	static bool started = false;
	if (started)
		return;
	started = true;
	if (xTaskCreate(dns_task, "captive_dns", 5120, NULL, 4, NULL) != pdPASS) {
		ESP_LOGE(TAG, "failed to create captive DNS task");
	}
}
