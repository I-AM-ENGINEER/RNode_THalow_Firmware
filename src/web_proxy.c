#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_mac.h"
#include "esp_cpu.h"
#include "esp_freertos_hooks.h"
#include "driver/temperature_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "cJSON.h"

#include "web_proxy.h"
#include "thalow_config.h"
#include "rns_proxy.h"
#include "wifi.h"
#include "ble.h"
#include "gzip_inflate.h"
#include "battery.h"
#include "captive_portal.h"
#include "firewall.h"
#include "config/project_config.h"

static const char *TAG = "web_proxy";

#define HTML_BUF_CAP (4 * 1024 * 1024)
#define PROXY_CHUNK_SZ 4096

extern const unsigned char thalow_inject_js_start[] asm("_binary_thalow_inject_js_start");
extern const unsigned char thalow_inject_js_end[] asm("_binary_thalow_inject_js_end");

static const char *INJECT_SNIPPET = "<script src=\"/thalow_inject.js\"></script>";

static temperature_sensor_handle_t s_tsens = NULL;
static bool s_tsens_failed = false;

static uint64_t s_idle_cycles[2] = { 0, 0 };
static uint32_t s_idle_last[2] = { 0, 0 };
static bool s_idle_ready[2] = { false, false };

static bool stats_idle_hook(void) {
	int core = xPortGetCoreID();
	if (core < 0 || core > 1)
		core = 0;
	uint32_t now = esp_cpu_get_cycle_count();
	if (s_idle_ready[core])
		s_idle_cycles[core] += (uint32_t)(now - s_idle_last[core]);
	s_idle_ready[core] = true;
	s_idle_last[core] = now;
	return false;
}

static void stats_tsens_init(void) {
	if (s_tsens != NULL || s_tsens_failed)
		return;
	temperature_sensor_config_t cfg =
		TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
	if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
		ESP_LOGW(TAG, "temperature sensor install failed");
		s_tsens_failed = true;
		return;
	}
	if (temperature_sensor_enable(s_tsens) != ESP_OK) {
		ESP_LOGW(TAG, "temperature sensor enable failed");
		temperature_sensor_uninstall(s_tsens);
		s_tsens = NULL;
		s_tsens_failed = true;
		return;
	}
	ESP_LOGI(TAG, "temperature sensor ready");
}

static const char *wifi_mode_str(wifi_mode_t mode) {
	switch (mode) {
	case WIFI_MODE_STA: return "STA";
	case WIFI_MODE_AP: return "AP";
	case WIFI_MODE_APSTA: return "APSTA";
	case WIFI_MODE_NULL:
	default: return "OFF";
	}
}

static const char *ble_state_str(ble_state_t s) {
	switch (s) {
	case BLE_STATE_ON: return "on";
	case BLE_STATE_PAIRING: return "pairing";
	case BLE_STATE_CONNECTED: return "connected";
	case BLE_STATE_OFF:
	default: return "off";
	}
}

static const char *chip_model_str(esp_chip_model_t m) {
	switch (m) {
	case CHIP_ESP32: return "ESP32";
	case CHIP_ESP32S2: return "ESP32-S2";
	case CHIP_ESP32S3: return "ESP32-S3";
	case CHIP_ESP32C2: return "ESP32-C2";
	case CHIP_ESP32C3: return "ESP32-C3";
	case CHIP_ESP32C5: return "ESP32-C5";
	case CHIP_ESP32C6: return "ESP32-C6";
	case CHIP_ESP32H2: return "ESP32-H2";
	case CHIP_ESP32P4: return "ESP32-P4";
	default: return "UNKNOWN";
	}
}

static void mac_to_str(uint8_t *mac, char *out) {
	snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* CPU usage is sampled by a background timer (cpu_sample_cb) every 2 s and
 * cached here. The stats endpoint just reads the last value. Previously the
 * stats handler measured on demand with a 1 s vTaskDelay, which blocked an
 * httpd worker for a full second per stats poll and starved request handling
 * when the browser polled /api/thalow_stats every second. */
static volatile int s_cpu_usage_cached = -1;
static esp_timer_handle_t s_cpu_timer = NULL;

static void cpu_sample_cb(void *arg) {
	(void)arg;
	uint64_t before0 = s_idle_cycles[0];
	uint64_t before1 = s_idle_cycles[1];
	int64_t t0 = esp_timer_get_time();
	vTaskDelay(pdMS_TO_TICKS(250));
	int64_t t1 = esp_timer_get_time();
	uint64_t after0 = s_idle_cycles[0];
	uint64_t after1 = s_idle_cycles[1];

	uint64_t idle_total = (after0 - before0) + (after1 - before1);
	uint64_t window_us = (uint64_t)(t1 - t0);
	if (window_us == 0)
		return;
	uint64_t cpu_freq = (uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ *
	                    1000000ULL;
	uint64_t window_cycles_total = 2ULL * window_us * cpu_freq /
	                               1000000ULL;
	if (window_cycles_total == 0)
		return;
	if (idle_total >= window_cycles_total) {
		s_cpu_usage_cached = 0;
		return;
	}
	s_cpu_usage_cached = (int)(100 - (100 * idle_total) / window_cycles_total);
}

static void cpu_sampler_init(void) {
	if (s_cpu_timer)
		return;
	const esp_timer_create_args_t a = {
		.callback = cpu_sample_cb,
		.name = "cpu_samp",
	};
	if (esp_timer_create(&a, &s_cpu_timer) != ESP_OK)
		return;
	esp_timer_start_periodic(s_cpu_timer, 2 * 1000 * 1000);
}

/* Returns the last cached CPU-usage sample, or -1 if none yet. No blocking. */
static int measure_cpu_usage(void) {
	cpu_sampler_init();
	return s_cpu_usage_cached;
}

#define CACHE_MAX_ENTRIES 16
#define CACHE_BODY_CAP (512 * 1024)

typedef struct {
	bool used;
	char path[80];
	char content_type[56];
	bool gzip;
	uint8_t *body;
	size_t len;
} cache_entry_t;

static cache_entry_t s_cache[CACHE_MAX_ENTRIES];

static const char *thalow_auth_str(thalow_auth_t m);
static const char *thalow_led_mode_str(led_mode_t m);

static cache_entry_t *cache_lookup(const char *path) {
	for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
		if (s_cache[i].used && strcmp(s_cache[i].path, path) == 0)
			return &s_cache[i];
	}
	return NULL;
}

static void cache_store(const char *path, const char *ct, bool gzip,
                        const uint8_t *body, size_t len) {
	if (len == 0 || len > CACHE_BODY_CAP)
		return;
	if (strlen(path) >= sizeof(((cache_entry_t *)0)->path))
		return;

	int slot = -1;
	for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
		if (!s_cache[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
			if (strcmp(s_cache[i].path, path) == 0) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0)
		return;

	uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
	if (!copy)
		return;
	memcpy(copy, body, len);

	if (s_cache[slot].used && s_cache[slot].body)
		heap_caps_free(s_cache[slot].body);

	s_cache[slot].used = true;
	strncpy(s_cache[slot].path, path, sizeof(s_cache[slot].path) - 1);
	s_cache[slot].path[sizeof(s_cache[slot].path) - 1] = '\0';
	strncpy(s_cache[slot].content_type, ct,
	        sizeof(s_cache[slot].content_type) - 1);
	s_cache[slot].content_type[sizeof(s_cache[slot].content_type) - 1] = '\0';
	s_cache[slot].gzip = gzip;
	s_cache[slot].body = copy;
	s_cache[slot].len = len;
}

static const char *status_reason(int code) {
	switch (code) {
	case 200: return "OK";
	case 201: return "Created";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	default: return "OK";
	}
}

static esp_err_t send_502(httpd_req_t *req) {
	httpd_resp_set_status(req, "502 Bad Gateway");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_send(req, "Upstream radio unreachable", -1);
	return ESP_FAIL;
}

static char *rfind_body_tag(char *buf, size_t len) {
	const char *tag = "</body>";
	size_t tlen = 7;
	if (len < tlen)
		return NULL;
	for (size_t i = len - tlen + 1; i-- > 0; ) {
		int ok = 1;
		for (size_t j = 0; j < tlen; j++) {
			char c = buf[i + j];
			if (c >= 'A' && c <= 'Z') c += 32;
			char t = tag[j];
			if (t >= 'A' && t <= 'Z') t += 32;
			if (c != t) { ok = 0; break; }
		}
		if (ok)
			return buf + i;
	}
	return NULL;
}

typedef struct {
	char ct[64];
	char enc[32];
	bool ct_set;
	bool enc_set;
} resp_hdrs_t;

static esp_err_t proxy_event_handler(esp_http_client_event_t *evt) {
	if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data) {
		resp_hdrs_t *h = (resp_hdrs_t *)evt->user_data;
		if (strcasecmp(evt->header_key, "Content-Type") == 0) {
			strncpy(h->ct, evt->header_value, sizeof(h->ct) - 1);
			h->ct[sizeof(h->ct) - 1] = '\0';
			h->ct_set = true;
		} else if (strcasecmp(evt->header_key, "Content-Encoding") == 0) {
			strncpy(h->enc, evt->header_value, sizeof(h->enc) - 1);
			h->enc[sizeof(h->enc) - 1] = '\0';
			h->enc_set = true;
		}
	}
	return ESP_OK;
}

static cJSON *build_thalow_cfg_json(void) {
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;
	cJSON_AddBoolToObject(root, "wifi_ap_enabled",
	                       thalow_config_get_wifi_ap_enabled());
	cJSON_AddStringToObject(root, "wifi_ap_ssid", thalow_config_get_ssid());
	cJSON_AddStringToObject(root, "wifi_ap_authmode",
	                        thalow_auth_str(thalow_config_get_wifi_ap_authmode()));
	cJSON_AddStringToObject(root, "wifi_ap_ip",
	                        thalow_config_get_wifi_ap_ip());
	cJSON_AddStringToObject(root, "wifi_ap_netmask",
	                        thalow_config_get_wifi_ap_netmask());
	cJSON_AddBoolToObject(root, "wifi_ap_password_set",
	                       strlen(thalow_config_get_password()) > 0);
	cJSON_AddBoolToObject(root, "wifi_sta_enabled",
	                       thalow_config_get_wifi_sta_enabled());
	cJSON_AddStringToObject(root, "wifi_sta_ssid",
	                         thalow_config_get_wifi_sta_ssid());
	cJSON_AddBoolToObject(root, "wifi_sta_password_set",
	                       strlen(thalow_config_get_wifi_sta_password()) > 0);
	cJSON_AddBoolToObject(root, "wifi_sta_dhcp",
	                       thalow_config_get_wifi_sta_dhcp());

	esp_netif_ip_info_t sta_ip;
	char ip_str[16] = "", nm_str[16] = "", gw_str[16] = "";
	if (wifi_get_sta_ip_info(&sta_ip) == ESP_OK && sta_ip.ip.addr != 0) {
		esp_ip4addr_ntoa(&sta_ip.ip, ip_str, sizeof(ip_str));
		esp_ip4addr_ntoa(&sta_ip.netmask, nm_str, sizeof(nm_str));
		esp_ip4addr_ntoa(&sta_ip.gw, gw_str, sizeof(gw_str));
	}
	cJSON_AddStringToObject(root, "wifi_sta_ip", ip_str);
	cJSON_AddStringToObject(root, "wifi_sta_netmask", nm_str);
	cJSON_AddStringToObject(root, "wifi_sta_gw", gw_str);
	cJSON_AddBoolToObject(root, "ble_enabled",
	                       thalow_config_get_ble_enabled());
	cJSON_AddStringToObject(root, "ble_name", thalow_config_get_ble_name());

	cJSON_AddStringToObject(root, "led_mode",
	                        thalow_led_mode_str(thalow_config_get_led_mode()));
	cJSON_AddBoolToObject(root, "led_heartbeat",
	                      thalow_config_get_led_heartbeat());

	/* RNS TCP proxy. Port 0 means "compile default (4242)"; the frontend
	 * shows the effective value and warns that a change needs a reboot.
	 * Whitelist is a free-text CIDR list (empty = allow all). */
	cJSON_AddBoolToObject(root, "rns_proxy_enabled",
	                      thalow_config_get_rns_proxy_enabled());
	cJSON_AddNumberToObject(root, "rns_proxy_port",
	                      (double)thalow_config_get_rns_proxy_port());
	cJSON_AddStringToObject(root, "rns_proxy_whitelist",
	                      thalow_config_get_rns_proxy_whitelist());
	return root;
}

static esp_err_t send_thalow_err(httpd_req_t *req, const char *msg) {
	httpd_resp_set_status(req, "400 Bad Request");
	httpd_resp_set_type(req, "application/json");
	char rbuf[160];
	snprintf(rbuf, sizeof(rbuf), "{\"error\":\"%s\"}", msg ? msg : "invalid");
	return httpd_resp_send(req, rbuf, -1);
}

static esp_err_t handle_thalow_cfg_get(httpd_req_t *req) {
	cJSON *root = build_thalow_cfg_json();
	if (!root) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!out) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t e = httpd_resp_send(req, out, -1);
	free(out);
	return e;
}

static esp_err_t handle_thalow_cfg_post(httpd_req_t *req) {
	int len = req->content_len;
	if (len <= 0 || len > 1024) {
		return send_thalow_err(req, "empty or too large body");
	}

	char *body = malloc((size_t)len + 1);
	if (!body) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	int got = 0;
	while (got < len) {
		int r = httpd_req_recv(req, body + got, len - got);
		if (r <= 0) {
			free(body);
			httpd_resp_send_500(req);
			return ESP_FAIL;
		}
		got += r;
	}
	body[len] = '\0';

	cJSON *root = cJSON_Parse(body);
	free(body);
	if (!root) {
		return send_thalow_err(req, "invalid JSON");
	}

	esp_err_t rc = ESP_OK;
	cJSON *it = NULL;
	const char *err = NULL;
	bool wifi_changed = false;
	bool rns_changed = false;

#define THALOW_BOOL(key, setter) \
	it = cJSON_GetObjectItem(root, key); \
	if (it) { \
		if (!cJSON_IsBool(it)) { \
			err = key " must be a boolean"; \
			goto fail; \
		} \
		(void)setter(cJSON_IsTrue(it)); \
	}

#define THALOW_STR(key, setter, badsize, badarg) \
	it = cJSON_GetObjectItem(root, key); \
	if (it) { \
		if (!cJSON_IsString(it) || !it->valuestring) { \
			err = key " must be a string"; \
			goto fail; \
		} \
		esp_err_t _e = setter(it->valuestring); \
		if (_e == ESP_ERR_INVALID_SIZE) { \
			err = badsize; \
			goto fail; \
		} \
		if (_e != ESP_OK) { \
			err = badarg; \
			goto fail; \
		} \
	}

	THALOW_BOOL("wifi_ap_enabled", thalow_config_set_wifi_ap_enabled)
	if (cJSON_GetObjectItem(root, "wifi_ap_enabled")) wifi_changed = true;
	THALOW_STR("wifi_ap_ssid", thalow_config_set_ssid,
	           "wifi_ap_ssid must be 1..32 bytes",
	           "wifi_ap_ssid must not have leading/trailing spaces")
	if (cJSON_GetObjectItem(root, "wifi_ap_ssid")) wifi_changed = true;
	it = cJSON_GetObjectItem(root, "wifi_ap_authmode");
	if (it) {
		if (!cJSON_IsString(it) || !it->valuestring) {
			err = "wifi_ap_authmode must be a string";
			goto fail;
		}
		thalow_auth_t am;
		if (strcmp(it->valuestring, "OPEN") == 0)
			am = THALOW_AUTH_OPEN;
		else if (strcmp(it->valuestring, "WPA2") == 0)
			am = THALOW_AUTH_WPA2;
		else if (strcmp(it->valuestring, "WPA2_WPA3") == 0)
			am = THALOW_AUTH_WPA2_WPA3;
		else if (strcmp(it->valuestring, "WPA3") == 0)
			am = THALOW_AUTH_WPA3;
		else {
			err = "Invalid authmode";
			goto fail;
		}
		esp_err_t _e = thalow_config_set_wifi_ap_authmode(am);
		if (_e != ESP_OK) {
			err = "Invalid authmode";
			goto fail;
		}
		wifi_changed = true;
	}
	THALOW_STR("wifi_ap_ip", thalow_config_set_wifi_ap_ip,
	           "wifi_ap_ip too long", "Invalid IP/netmask")
	if (cJSON_GetObjectItem(root, "wifi_ap_ip")) wifi_changed = true;
	THALOW_STR("wifi_ap_netmask", thalow_config_set_wifi_ap_netmask,
	           "wifi_ap_netmask too long", "Invalid IP/netmask")
	if (cJSON_GetObjectItem(root, "wifi_ap_netmask")) wifi_changed = true;
	it = cJSON_GetObjectItem(root, "wifi_ap_password");
	if (it) {
		if (!cJSON_IsString(it) || !it->valuestring) {
			err = "wifi_ap_password must be a string";
			goto fail;
		}
		esp_err_t _e = thalow_config_set_password(it->valuestring);
		if (_e == ESP_ERR_INVALID_SIZE) {
			err = "wifi_ap_password must be 0..63 bytes";
			goto fail;
		}
		if (_e != ESP_OK) {
			err = "wifi_ap_password must be empty or 8..63 bytes";
			goto fail;
		}
		wifi_changed = true;
	}
	THALOW_BOOL("wifi_sta_enabled", thalow_config_set_wifi_sta_enabled)
	if (cJSON_GetObjectItem(root, "wifi_sta_enabled")) wifi_changed = true;
	THALOW_STR("wifi_sta_ssid", thalow_config_set_wifi_sta_ssid,
	           "wifi_sta_ssid must be 0..32 bytes",
	           "wifi_sta_ssid must not have leading/trailing spaces")
	if (cJSON_GetObjectItem(root, "wifi_sta_ssid")) wifi_changed = true;
	it = cJSON_GetObjectItem(root, "wifi_sta_password");
	if (it) {
		if (!cJSON_IsString(it) || !it->valuestring) {
			err = "wifi_sta_password must be a string";
			goto fail;
		}
		esp_err_t _e = thalow_config_set_wifi_sta_password(it->valuestring);
		if (_e == ESP_ERR_INVALID_SIZE) {
			err = "wifi_sta_password must be 0..63 bytes";
			goto fail;
		}
		if (_e != ESP_OK) {
			err = "wifi_sta_password must be empty or 8..63 bytes";
			goto fail;
		}
		wifi_changed = true;
	}
	THALOW_BOOL("wifi_sta_dhcp", thalow_config_set_wifi_sta_dhcp)
	if (cJSON_GetObjectItem(root, "wifi_sta_dhcp")) wifi_changed = true;
	THALOW_STR("wifi_sta_ip", thalow_config_set_wifi_sta_ip,
	           "wifi_sta_ip too long", "invalid wifi_sta_ip")
	if (cJSON_GetObjectItem(root, "wifi_sta_ip")) wifi_changed = true;
	THALOW_STR("wifi_sta_netmask", thalow_config_set_wifi_sta_netmask,
	           "wifi_sta_netmask too long", "invalid wifi_sta_netmask")
	if (cJSON_GetObjectItem(root, "wifi_sta_netmask")) wifi_changed = true;
	THALOW_STR("wifi_sta_gw", thalow_config_set_wifi_sta_gw,
	           "wifi_sta_gw too long", "invalid wifi_sta_gw")
	if (cJSON_GetObjectItem(root, "wifi_sta_gw")) wifi_changed = true;
	THALOW_BOOL("ble_enabled", thalow_config_set_ble_enabled)
	THALOW_STR("ble_name", thalow_config_set_ble_name,
	           "ble_name must be 1..39 bytes", "invalid ble_name")

	it = cJSON_GetObjectItem(root, "led_mode");
	if (it) {
		if (!cJSON_IsString(it) || !it->valuestring) {
			err = "led_mode must be a string";
			goto fail;
		}
		led_mode_t m;
		if (strcmp(it->valuestring, "BLE") == 0)
			m = LED_MODE_BLE;
		else if (strcmp(it->valuestring, "TCP") == 0)
			m = LED_MODE_TCP;
		else if (strcmp(it->valuestring, "WIFI_STA") == 0)
			m = LED_MODE_WIFI_STA;
		else if (strcmp(it->valuestring, "WIFI_AP") == 0)
			m = LED_MODE_WIFI_AP;
		else if (strcmp(it->valuestring, "TRAFFIC") == 0)
			m = LED_MODE_TRAFFIC;
		else {
			err = "Invalid led_mode";
			goto fail;
		}
		if (thalow_config_set_led_mode(m) != ESP_OK) {
			err = "Invalid led_mode";
			goto fail;
		}
	}
	it = cJSON_GetObjectItem(root, "led_heartbeat");
	if (it) {
		if (!cJSON_IsBool(it)) {
			err = "led_heartbeat must be a boolean";
			goto fail;
		}
		thalow_config_set_led_heartbeat(cJSON_IsTrue(it));
	}

	it = cJSON_GetObjectItem(root, "rns_proxy_enabled");
	if (it) {
		if (!cJSON_IsBool(it)) {
			err = "rns_proxy_enabled must be a boolean";
			goto fail;
		}
		thalow_config_set_rns_proxy_enabled(cJSON_IsTrue(it));
		rns_changed = true;
	}

	it = cJSON_GetObjectItem(root, "rns_proxy_port");
	if (it) {
		if (!cJSON_IsNumber(it)) {
			err = "rns_proxy_port must be a number";
			goto fail;
		}
		int port = (int)it->valuedouble;
		if (port < 0 || port > 65535) {
			err = "rns_proxy_port out of range";
			goto fail;
		}
		thalow_config_set_rns_proxy_port((uint16_t)port);
		rns_changed = true;
	}

	it = cJSON_GetObjectItem(root, "rns_proxy_whitelist");
	if (it) {
		if (!cJSON_IsString(it) || !it->valuestring) {
			err = "rns_proxy_whitelist must be a string";
			goto fail;
		}
		thalow_config_set_rns_proxy_whitelist(it->valuestring);
		rns_changed = true;
	}

#undef THALOW_BOOL
#undef THALOW_STR

	cJSON_Delete(root);

	if (wifi_changed) {
		esp_err_t app_err = wifi_apply();
		if (app_err != ESP_OK) {
			ESP_LOGE(TAG, "wifi_apply failed: %s",
			         esp_err_to_name(app_err));
		}
	} else {
		ESP_LOGI(TAG, "no WiFi fields changed, skipping wifi_apply");
	}

	if (rns_changed) {
		ESP_LOGI(TAG, "RNS proxy config changed, restarting listener");
		rns_proxy_restart();
	}

	cJSON *resp = build_thalow_cfg_json();
	if (!resp) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	char *out = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!out) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	rc = httpd_resp_send(req, out, -1);
	free(out);
	return rc;

fail:
	cJSON_Delete(root);
	return send_thalow_err(req, err);
}

static esp_err_t handle_thalow_reset(httpd_req_t *req) {
	int len = req->content_len;
	if (len <= 0 || len > 256)
		return send_thalow_err(req, "empty or too large body");
	char *body = malloc((size_t)len + 1);
	if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
	int got = 0;
	while (got < len) {
		int r = httpd_req_recv(req, body + got, len - got);
		if (r <= 0) { free(body); httpd_resp_send_500(req); return ESP_FAIL; }
		got += r;
	}
	body[len] = '\0';
	cJSON *root = cJSON_Parse(body);
	free(body);
	if (!root)
		return send_thalow_err(req, "invalid JSON");
	cJSON *sec = cJSON_GetObjectItem(root, "section");
	const char *section = (sec && cJSON_IsString(sec) && sec->valuestring) ? sec->valuestring : NULL;
	if (!section) {
		cJSON_Delete(root);
		return send_thalow_err(req, "missing 'section'");
	}
	esp_err_t rr = thalow_config_reset(section);
	cJSON_Delete(root);
	if (rr != ESP_OK)
		return send_thalow_err(req, "invalid section");
	if (strcmp(section, "ap") == 0 || strcmp(section, "sta") == 0)
		wifi_apply();
	cJSON *resp = build_thalow_cfg_json();
	if (!resp) { httpd_resp_send_500(req); return ESP_FAIL; }
	char *out = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!out) { httpd_resp_send_500(req); return ESP_FAIL; }
	httpd_resp_set_type(req, "application/json");
	esp_err_t rc = httpd_resp_send(req, out, -1);
	free(out);
	return rc;
}

static esp_err_t handle_thalow_stats(httpd_req_t *req) {
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	stats_tsens_init();

	const char *wmode = "OFF";
	wifi_mode_t mode;
	if (esp_wifi_get_mode(&mode) == ESP_OK)
		wmode = wifi_mode_str(mode);
	cJSON_AddStringToObject(root, "wifi_mode", wmode);

	cJSON_AddStringToObject(root, "ble_state",
	                        ble_state_str(ble_get_state()));

	uint64_t uptime = (uint64_t)(esp_timer_get_time() / 1000000ULL);
	cJSON_AddNumberToObject(root, "uptime_sec", (double)uptime);

	cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
	cJSON_AddStringToObject(root, "fw_version", FW_VERSION);

	esp_chip_info_t info;
	esp_chip_info(&info);
	cJSON_AddStringToObject(root, "chip_model",
	                        chip_model_str(info.model));
	int rev_major = info.revision / 100;
	int rev_minor = info.revision % 100;
	char rev_buf[16];
	snprintf(rev_buf, sizeof(rev_buf), "v%d.%d", rev_major, rev_minor);
	cJSON_AddStringToObject(root, "chip_revision", rev_buf);
	cJSON_AddNumberToObject(root, "chip_cores", info.cores);

	uint32_t flash_size = 0;
	if (esp_flash_get_size(NULL, &flash_size) != ESP_OK)
		flash_size = 0;
	cJSON_AddNumberToObject(root, "flash_size", (double)flash_size);

	uint32_t flash_speed_hz = 0;
#if defined(CONFIG_ESPTOOLPY_FLASHFREQ_120M)
	flash_speed_hz = 120000000;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_80M)
	flash_speed_hz = 80000000;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_48M)
	flash_speed_hz = 48000000;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_40M)
	flash_speed_hz = 40000000;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_26M)
	flash_speed_hz = 26000000;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_20M)
	flash_speed_hz = 20000000;
#endif
	cJSON_AddNumberToObject(root, "flash_speed_hz",
	                        (double)flash_speed_hz);

	size_t psram_size = esp_psram_get_size();
	cJSON_AddNumberToObject(root, "psram_size", (double)psram_size);
	cJSON_AddNumberToObject(root, "psram_total",
	                        (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
	cJSON_AddNumberToObject(root, "psram_free",
	                        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

	cJSON_AddNumberToObject(root, "free_heap",
	                        (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
	cJSON_AddNumberToObject(root, "min_free_heap",
	                        (double)esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(root, "total_heap",
	                        (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

	if (s_tsens) {
		float t = 0;
		if (temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK)
			cJSON_AddNumberToObject(root, "temperature_c", (double)t);
		else
			cJSON_AddNullToObject(root, "temperature_c");
	} else {
		cJSON_AddNullToObject(root, "temperature_c");
	}

	uint8_t mac[6];
	char mac_buf[18];
	if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
		mac_to_str(mac, mac_buf);
		cJSON_AddStringToObject(root, "mac_wifi_ap", mac_buf);
	} else {
		cJSON_AddNullToObject(root, "mac_wifi_ap");
	}
	if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
		mac_to_str(mac, mac_buf);
		cJSON_AddStringToObject(root, "mac_wifi_sta", mac_buf);
	} else {
		cJSON_AddNullToObject(root, "mac_wifi_sta");
	}
	if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
		mac_to_str(mac, mac_buf);
		cJSON_AddStringToObject(root, "mac_bt", mac_buf);
	} else {
		cJSON_AddNullToObject(root, "mac_bt");
	}

	char ip_str[16] = "";
	esp_netif_ip_info_t ip_info;
	if (wifi_get_ap_ip_info(&ip_info) == ESP_OK &&
	    ip_info.ip.addr != 0) {
		esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
	}
	cJSON_AddStringToObject(root, "ip_ap", ip_str);

	ip_str[0] = '\0';
	if (wifi_get_sta_ip_info(&ip_info) == ESP_OK &&
	    ip_info.ip.addr != 0) {
		esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
	}
	cJSON_AddStringToObject(root, "ip_sta", ip_str);
	cJSON_AddStringToObject(root, "hostname", wifi_get_hostname());

	/* --- Access Point (this device's SoftAP) --- */
	cJSON_AddBoolToObject(root, "ap_enabled",
	                     thalow_config_get_wifi_ap_enabled());
	cJSON_AddStringToObject(root, "ap_ssid", thalow_config_get_ssid());
	cJSON_AddStringToObject(root, "ap_authmode",
			thalow_auth_str(thalow_config_get_wifi_ap_authmode()));
	cJSON_AddNumberToObject(root, "ap_clients",
			      (double)wifi_ap_station_count());

	/* --- Station (uplink to an external AP) --- */
	cJSON_AddBoolToObject(root, "sta_enabled",
	                     thalow_config_get_wifi_sta_enabled());
	cJSON_AddStringToObject(root, "sta_status", wifi_sta_status());
	cJSON_AddStringToObject(root, "sta_ssid",
	                      wifi_sta_configured_ssid());
	{
		int32_t rssi = wifi_sta_rssi();
		if (rssi == INT32_MIN)
			cJSON_AddNullToObject(root, "sta_rssi");
		else
			cJSON_AddNumberToObject(root, "sta_rssi", (double)rssi);
	}

	/* --- RNS (Reticulum) TCP proxy: inbound client count + last client --- */
	cJSON_AddNumberToObject(root, "tcp_clients",
	                      (double)rns_proxy_client_count());
	{
		char lc[32];
		rns_proxy_last_client(lc, sizeof(lc));
		cJSON_AddStringToObject(root, "tcp_client", lc);
	}

	int cpu = measure_cpu_usage();
	if (cpu < 0)
		cJSON_AddNullToObject(root, "cpu_usage");
	else
		cJSON_AddNumberToObject(root, "cpu_usage", (double)cpu);

	cJSON_AddNumberToObject(root, "battery_percent",
	                        (double)battery_get_percent());
	cJSON_AddNumberToObject(root, "battery_voltage_mv",
	                        (double)battery_get_voltage_mv());

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!out) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t e = httpd_resp_send(req, out, -1);
	free(out);
	return e;
}

static const char *thalow_auth_str(thalow_auth_t m) {
	switch (m) {
	case THALOW_AUTH_WPA2_WPA3: return "WPA2_WPA3";
	case THALOW_AUTH_WPA3: return "WPA3";
	case THALOW_AUTH_WPA2: return "WPA2";
	case THALOW_AUTH_OPEN:
	default: return "OPEN";
	}
}

static const char *thalow_led_mode_str(led_mode_t m) {
	switch (m) {
	case LED_MODE_TCP: return "TCP";
	case LED_MODE_WIFI_STA: return "WIFI_STA";
	case LED_MODE_WIFI_AP: return "WIFI_AP";
	case LED_MODE_TRAFFIC: return "TRAFFIC";
	case LED_MODE_BLE:
	default: return "BLE";
	}
}

static const char *scan_auth_str(wifi_auth_mode_t m) {
	switch (m) {
	case WIFI_AUTH_OPEN: return "OPEN";
	case WIFI_AUTH_WEP: return "WEP";
	case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
	case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
	case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
	case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
	case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
	case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
	case WIFI_AUTH_WAPI_PSK: return "WAPI_PSK";
	case WIFI_AUTH_OWE: return "OWE";
	case WIFI_AUTH_WPA3_ENT_192: return "WPA3_ENT_192";
	default: return "UNKNOWN";
	}
}

static esp_err_t handle_thalow_scan(httpd_req_t *req) {
	wifi_ap_record_t aps[32];
	uint16_t cap = 32;
	uint16_t n = 0;

	esp_err_t e = wifi_scan(aps, cap, &n);
	if (e != ESP_OK) {
		httpd_resp_set_status(req, "503 Service Unavailable");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req,
		                "{\"error\":\"Enable STA or AP+STA mode to scan\"}",
		                -1);
		return ESP_OK;
	}

	cJSON *root = cJSON_CreateArray();
	if (!root) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}

	char ssid_buf[33];
	char bssid_buf[18];
	for (uint16_t i = 0; i < n; i++) {
		memcpy(ssid_buf, aps[i].ssid, 32);
		ssid_buf[32] = '\0';
		snprintf(bssid_buf, sizeof(bssid_buf),
		         "%02X:%02X:%02X:%02X:%02X:%02X",
		         aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
		         aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);

		cJSON *o = cJSON_CreateObject();
		if (!o) {
			cJSON_Delete(root);
			httpd_resp_send_500(req);
			return ESP_FAIL;
		}
		cJSON_AddStringToObject(o, "ssid", ssid_buf);
		cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
		cJSON_AddNumberToObject(o, "channel", aps[i].primary);
		cJSON_AddStringToObject(o, "auth",
		                        scan_auth_str(aps[i].authmode));
		cJSON_AddStringToObject(o, "bssid", bssid_buf);
		cJSON_AddItemToArray(root, o);
	}

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!out) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t rc = httpd_resp_send(req, out, -1);
	free(out);
	return rc;
}

static esp_err_t handle_inject_js(httpd_req_t *req) {
	httpd_resp_set_type(req, "application/javascript");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	size_t len = (size_t)(thalow_inject_js_end - thalow_inject_js_start);
	if (len > 0 && thalow_inject_js_start[len - 1] == '\0') {
		len--;
	}
	httpd_resp_send(req, (const char *)thalow_inject_js_start, len);
	return ESP_OK;
}

static esp_err_t handle_proxy(httpd_req_t *req) {
	/* Captive-portal: answer OS connectivity probes with a 200 (not a 3xx)
	 * so the OS realises it is behind a captive portal and pops a
	 * "sign in to network" notification, without ever looping on an HTTP
	 * redirect (the old 302 reply caused ERR_TOO_MANY_REDIRECTS). DHCP
	 * option 114 (set in wifi.c) is what actually opens the setup page on
	 * Win10+/Android11+/iOS14+. */
	char host_dbg[96] = {0};
	httpd_req_get_hdr_value_str(req, "Host", host_dbg, sizeof(host_dbg));
	ESP_LOGI(TAG, "REQ %s Host=%s", req->uri, host_dbg);

	if (captive_portal_is_active() && captive_portal_match(req)) {
		return captive_portal_redirect(req);
	}

	/* Landing page (e.g. Windows /redirect): answer with a real 302 — its
	 * mini-browser ignores <meta http-refresh>, so it must be an HTTP
	 * redirect. Loop-safe: landings aren't re-probed. */
	if (captive_portal_is_active() && captive_portal_is_landing(req)) {
		return captive_portal_landing_redirect(req);
	}

	/* While the captive portal is active there is no internet, so any request
	 * addressed to a foreign host (analytics, Play Services, random apps) has
	 * no useful upstream to proxy to. Proxied to the radio over slow SLIP it
	 * just burns sockets (8 httpd + 8 client = the LWIP budget) and stalls
	 * the client until httpd starts refusing accept() — which is what caused
	 * the phone's "connection reset / no captive popup" after a while. Answer
	 * these locally and instantly instead. */
	if (captive_portal_is_active() && captive_portal_is_foreign_host(req)) {
		return captive_portal_intercept(req);
	}

	char *client_body = NULL;
	int client_body_len = 0;

	if (req->content_len > 0) {
		if (req->content_len > (4 * 1024 * 1024)) {
			httpd_resp_set_status(req, "413 Payload Too Large");
			httpd_resp_set_type(req, "text/plain");
			httpd_resp_send(req, "Payload too large", -1);
			return ESP_FAIL;
		}
		client_body = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
		if (!client_body)
			return httpd_resp_send_500(req);
		int got = 0;
		while (got < req->content_len) {
			int r = httpd_req_recv(req, client_body + got,
			                       req->content_len - got);
			if (r <= 0) {
				heap_caps_free(client_body);
				return send_502(req);
			}
			got += r;
		}
		client_body_len = req->content_len;
	}

	const char *full = req->uri;
	const char *q = strchr(full, '?');

	char cache_key[80];
	if (q) {
		size_t kl = (size_t)(q - full);
		if (kl >= sizeof(cache_key)) kl = sizeof(cache_key) - 1;
		memcpy(cache_key, full, kl);
		cache_key[kl] = '\0';
	} else {
		strncpy(cache_key, full, sizeof(cache_key) - 1);
		cache_key[sizeof(cache_key) - 1] = '\0';
	}

	char path_buf[256];
	const char *path;
	if (q) {
		size_t plen = (size_t)(q - full);
		if (plen >= sizeof(path_buf) - 1)
			plen = sizeof(path_buf) - 1;
		memcpy(path_buf, full, plen);
		path_buf[plen] = '\0';
		strncat(path_buf, q, sizeof(path_buf) - plen - 1);
		path = path_buf;
	} else {
		path = full;
	}

	bool is_api = (strncmp(cache_key, "/api/", 5) == 0);

	if (!is_api && req->method == HTTP_GET) {
		cache_entry_t *ce = cache_lookup(cache_key);
		if (ce) {
			httpd_resp_set_status(req, "200 OK");
			httpd_resp_set_type(req, ce->content_type);
			if (ce->gzip)
				httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
			httpd_resp_send(req, (const char *)ce->body, ce->len);
			if (client_body) heap_caps_free(client_body);
			return ESP_OK;
		}
	}

	/* Firewall: gate upstream-bound requests BEFORE we open a radio socket.
	 * Management/OS-probe traffic was already served above; everything that
	 * reaches here goes to the radio (192.168.7.2 over SLIP), which is a
	 * single link that stalls under load. The firewall caps concurrency and
	 * per-client rate, returning 429/503 itself so a stuck/slow radio can
	 * never exhaust the socket pool and kill httpd. On FW_DENY the response
	 * is already sent. */
	bool fw_upstream_held = false;
	if (firewall_check(req) == FW_DENY)
		return ESP_OK;   /* response already sent by the firewall */
	fw_upstream_held = true;

	if (is_api) {

	resp_hdrs_t resp_hdrs;
	memset(&resp_hdrs, 0, sizeof(resp_hdrs));

	esp_http_client_config_t http_config = {
		.host = HALOW_WEB_HOST,
		.port = HALOW_WEB_PORT,
		.path = path,
		.method = (req->method == HTTP_POST)
			? HTTP_METHOD_POST : HTTP_METHOD_GET,
		.transport_type = HTTP_TRANSPORT_OVER_TCP,
		.disable_auto_redirect = true,
		.timeout_ms = 4000,
		.event_handler = proxy_event_handler,
		.user_data = &resp_hdrs,
	};

	esp_http_client_handle_t client = esp_http_client_init(&http_config);
	if (!client) {
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return send_502(req);
	}

	char ctype_buf[64];
	if (httpd_req_get_hdr_value_str(req, "Content-Type",
	                                ctype_buf, sizeof(ctype_buf)) == ESP_OK)
		esp_http_client_set_header(client, "Content-Type", ctype_buf);

	if (client_body && client_body_len > 0)
		esp_http_client_set_post_field(client, client_body, client_body_len);

	memset(&resp_hdrs, 0, sizeof(resp_hdrs));

	esp_err_t open_err = esp_http_client_open(client, client_body_len);
	if (open_err != ESP_OK) {
		esp_http_client_cleanup(client);
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return send_502(req);
	}

	/* esp_http_client_open() only sends the request HEADERS (including the
	 * Content-Length it derived from write_len). For a POST it does NOT push
	 * the body -- that is left for either esp_http_client_perform() (which we
	 * do not use here, since we stream the response manually) or an explicit
	 * write. Without this, the upstream radio (config_page.c) sees
	 * "Content-Length: N" with zero body bytes following and replies
	 *   HTTP/1.1 400 "incomplete body\n"
	 * (16 bytes) which is exactly the 400 observed on POST /api/reset_stat etc.
	 * So write the captured body now, then fetch the response. */
	if (client_body && client_body_len > 0) {
		int written = esp_http_client_write(client, client_body,
		                                client_body_len);
		if (written < 0) {
			esp_http_client_close(client);
			esp_http_client_cleanup(client);
			if (client_body) heap_caps_free(client_body);
			if (fw_upstream_held) firewall_upstream_release();
			return send_502(req);
		}
	}

	int content_length = esp_http_client_fetch_headers(client);
	int status = esp_http_client_get_status_code(client);

	char ct_buf[64];
	char enc_buf[32];
	if (resp_hdrs.ct_set)
		strncpy(ct_buf, resp_hdrs.ct, sizeof(ct_buf) - 1);
	if (resp_hdrs.enc_set)
		strncpy(enc_buf, resp_hdrs.enc, sizeof(enc_buf) - 1);
	char *ct = resp_hdrs.ct_set ? ct_buf : NULL;
	char *enc = resp_hdrs.enc_set ? enc_buf : NULL;

	bool is_gzip = (enc && strcasecmp(enc, "gzip") == 0);
	bool is_html = (ct && strstr(ct, "text/html") != NULL)
		|| strcmp(path, "/") == 0
		|| strcmp(path, "/index.html") == 0;

	char status_str[32];
	snprintf(status_str, sizeof(status_str), "%d %s", status,
	         status_reason(status));
	httpd_resp_set_status(req, status_str);
	if (ct)
		httpd_resp_set_type(req, ct);
	else if (is_html)
		httpd_resp_set_type(req, "text/html");
	else
		httpd_resp_set_type(req, "application/octet-stream");

	uint8_t *body = NULL;
	size_t total = 0;
	esp_err_t ret = ESP_OK;

	if (!is_html) {
		if (enc)
			httpd_resp_set_hdr(req, "Content-Encoding", enc);

		uint8_t *chunk = heap_caps_malloc(PROXY_CHUNK_SZ, MALLOC_CAP_SPIRAM);
		if (!chunk) {
			esp_http_client_close(client);
			esp_http_client_cleanup(client);
			if (client_body) heap_caps_free(client_body);
			if (fw_upstream_held) firewall_upstream_release();
			return httpd_resp_send_500(req);
		}

		bool sent_any = false;
		bool upstream_err = false;
		for (;;) {
			int r = esp_http_client_read(client, (char *)chunk, PROXY_CHUNK_SZ);
			if (r == ESP_ERR_HTTP_EAGAIN) {
				int tries = 0;
				while (tries < 20 && r == ESP_ERR_HTTP_EAGAIN) {
					vTaskDelay(pdMS_TO_TICKS(50));
					r = esp_http_client_read(client, (char *)chunk, PROXY_CHUNK_SZ);
					tries++;
				}
			}
			if (r < 0) {
				upstream_err = true;
				break;
			}
			if (r == 0)
				break;
			esp_err_t e = httpd_resp_send_chunk(req, (const char *)chunk, r);
			if (e != ESP_OK) {
				heap_caps_free(chunk);
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
				if (client_body) heap_caps_free(client_body);
				if (fw_upstream_held) firewall_upstream_release();
				return ESP_FAIL;
			}
			sent_any = true;
		}

		if (upstream_err && !sent_any) {
			heap_caps_free(chunk);
			esp_http_client_close(client);
			esp_http_client_cleanup(client);
			if (client_body) heap_caps_free(client_body);
			if (fw_upstream_held) firewall_upstream_release();
			return send_502(req);
		}

		if (upstream_err)
			ret = ESP_FAIL;

		httpd_resp_send_chunk(req, NULL, 0);
		heap_caps_free(chunk);
	} else {
		if (is_gzip) {
			size_t bufcaps = (content_length > 0 &&
			                  (size_t)content_length < HTML_BUF_CAP)
				? (size_t)content_length + 1 : 8192;
			body = heap_caps_malloc(bufcaps, MALLOC_CAP_SPIRAM);
			if (!body) {
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
				if (client_body) heap_caps_free(client_body);
				if (fw_upstream_held) firewall_upstream_release();
				return httpd_resp_send_500(req);
			}

			total = 0;
			for (;;) {
				if (total >= bufcaps) {
					size_t newcap = bufcaps * 2;
					if (newcap > HTML_BUF_CAP) {
						ESP_LOGW(TAG, "HTML response exceeds %d, truncating",
						         HTML_BUF_CAP);
						break;
					}
					uint8_t *nb = heap_caps_realloc(body, newcap, MALLOC_CAP_SPIRAM);
					if (!nb) {
						ESP_LOGW(TAG, "OOM reading HTML response");
						break;
					}
					body = nb;
					bufcaps = newcap;
				}
				int toread = (int)(bufcaps - total);
				if (toread > PROXY_CHUNK_SZ)
					toread = PROXY_CHUNK_SZ;
				int r = esp_http_client_read(client, (char *)body + total, toread);
				if (r < 0) {
					if (r == ESP_ERR_HTTP_EAGAIN && total > 0) {
						break;
					}
					total = (size_t)-1;
					break;
				}
				if (r == 0)
					break;
				total += (size_t)r;
			}

			if (total == (size_t)-1) {
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
				if (client_body) heap_caps_free(client_body);
				heap_caps_free(body);
				if (fw_upstream_held) firewall_upstream_release();
				return send_502(req);
			}

			uint8_t *dec = heap_caps_malloc(HTML_BUF_CAP, MALLOC_CAP_SPIRAM);
			size_t declen = 0;
			if (dec && gzip_inflate(body, total, dec, HTML_BUF_CAP,
			                        &declen) == 0) {
				heap_caps_free(body);
				body = dec;
				total = declen;
				enc = NULL;
			} else {
				ESP_LOGW(TAG, "gzip inflate failed, passing through");
				if (dec)
					heap_caps_free(dec);
			}

			char *pos = rfind_body_tag((char *)body, total);
			size_t snippet_len = strlen(INJECT_SNIPPET);
			size_t newlen = total + snippet_len;
			uint8_t *nb = heap_caps_malloc(newlen + 1, MALLOC_CAP_SPIRAM);
			if (nb) {
				if (pos) {
					size_t before = (size_t)(pos - (char *)body);
					memcpy(nb, body, before);
					memcpy(nb + before, INJECT_SNIPPET, snippet_len);
					memcpy(nb + before + snippet_len, pos,
					       total - before);
				} else {
					memcpy(nb, body, total);
					memcpy(nb + total, INJECT_SNIPPET, snippet_len);
				}
				nb[newlen] = '\0';
				heap_caps_free(body);
				body = nb;
				total = newlen;
			}

			if (enc)
				httpd_resp_set_hdr(req, "Content-Encoding", enc);
			httpd_resp_send(req, (const char *)body, (ssize_t)total);
		} else {
			#define STREAM_HOLD 16
			size_t cap = STREAM_HOLD + PROXY_CHUNK_SZ;
			uint8_t *acc = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
			if (!acc) {
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
				if (client_body) heap_caps_free(client_body);
				if (fw_upstream_held) firewall_upstream_release();
				return httpd_resp_send_500(req);
			}

			size_t acc_len = 0;
			bool injected = false;
			bool sent_any = false;
			bool upstream_err = false;
			size_t snip_len = strlen(INJECT_SNIPPET);

			for (;;) {
				int toread = (int)(cap - acc_len);
				if (toread > PROXY_CHUNK_SZ)
					toread = PROXY_CHUNK_SZ;
				if (toread <= 0)
					toread = PROXY_CHUNK_SZ;
				int r = esp_http_client_read(client, (char *)acc + acc_len,
				                             toread);
				if (r == ESP_ERR_HTTP_EAGAIN) {
					int tries = 0;
					while (tries < 20 && r == ESP_ERR_HTTP_EAGAIN) {
						vTaskDelay(pdMS_TO_TICKS(50));
						r = esp_http_client_read(client,
						                         (char *)acc + acc_len,
						                         toread);
						tries++;
					}
				}
				if (r < 0) {
					upstream_err = true;
					break;
				}
				if (r == 0)
					break;
				acc_len += (size_t)r;

				if (!injected) {
					char *pos = rfind_body_tag((char *)acc, acc_len);
					if (pos) {
						size_t p = (size_t)(pos - (char *)acc);
						if (p > 0)
							httpd_resp_send_chunk(req, (const char *)acc, p);
						httpd_resp_send_chunk(req, INJECT_SNIPPET, snip_len);
						httpd_resp_send_chunk(req, (const char *)acc + p,
						                      acc_len - p);
						sent_any = true;
						injected = true;
						acc_len = 0;
						continue;
					}
					size_t flushable = (acc_len > STREAM_HOLD)
						? acc_len - STREAM_HOLD : 0;
					if (flushable > 0) {
						httpd_resp_send_chunk(req, (const char *)acc,
						                      flushable);
						sent_any = true;
						memmove(acc, acc + flushable,
						        acc_len - flushable);
						acc_len -= flushable;
					}
				} else {
					httpd_resp_send_chunk(req, (const char *)acc, acc_len);
					sent_any = true;
					acc_len = 0;
				}
			}

			if (upstream_err && !sent_any) {
				heap_caps_free(acc);
				esp_http_client_close(client);
				esp_http_client_cleanup(client);
				if (client_body) heap_caps_free(client_body);
				if (fw_upstream_held) firewall_upstream_release();
				return send_502(req);
			}

			if (!injected && acc_len > 0)
				httpd_resp_send_chunk(req, (const char *)acc, acc_len);
			if (!injected)
				httpd_resp_send_chunk(req, INJECT_SNIPPET, snip_len);
			httpd_resp_send_chunk(req, NULL, 0);
			if (upstream_err)
				ret = ESP_FAIL;
			heap_caps_free(acc);
			#undef STREAM_HOLD
		}
	}

	esp_http_client_close(client);
	esp_http_client_cleanup(client);
	if (client_body) heap_caps_free(client_body);
	if (body) heap_caps_free(body);
	if (fw_upstream_held) firewall_upstream_release();
	return ret;
	}

	resp_hdrs_t resp_hdrs;
	memset(&resp_hdrs, 0, sizeof(resp_hdrs));

	esp_http_client_config_t http_config = {
		.host = HALOW_WEB_HOST,
		.port = HALOW_WEB_PORT,
		.path = path,
		.method = HTTP_METHOD_GET,
		.transport_type = HTTP_TRANSPORT_OVER_TCP,
		.disable_auto_redirect = true,
		.timeout_ms = 4000,
		.event_handler = proxy_event_handler,
		.user_data = &resp_hdrs,
	};

	esp_http_client_handle_t client = esp_http_client_init(&http_config);
	if (!client) {
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return send_502(req);
	}

	memset(&resp_hdrs, 0, sizeof(resp_hdrs));

	esp_err_t open_err = esp_http_client_open(client, 0);
	if (open_err != ESP_OK) {
		esp_http_client_cleanup(client);
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return send_502(req);
	}

	int content_length = esp_http_client_fetch_headers(client);
	int status = esp_http_client_get_status_code(client);

	char ct_buf[64];
	char enc_buf[32];
	if (resp_hdrs.ct_set)
		strncpy(ct_buf, resp_hdrs.ct, sizeof(ct_buf) - 1);
	if (resp_hdrs.enc_set)
		strncpy(enc_buf, resp_hdrs.enc, sizeof(enc_buf) - 1);
	char *ct = resp_hdrs.ct_set ? ct_buf : NULL;
	char *enc = resp_hdrs.enc_set ? enc_buf : NULL;

	bool is_gzip = (enc && strcasecmp(enc, "gzip") == 0);
	bool is_html = (ct && strstr(ct, "text/html") != NULL)
		|| strcmp(cache_key, "/") == 0
		|| strcmp(cache_key, "/index.html") == 0;

	char status_str[32];
	snprintf(status_str, sizeof(status_str), "%d %s", status,
	         status_reason(status));
	httpd_resp_set_status(req, status_str);

	const char *serve_ct = ct ? ct
		: (is_html ? "text/html" : "application/octet-stream");
	httpd_resp_set_type(req, serve_ct);

	size_t bufcaps = (content_length > 0 &&
	                  (size_t)content_length < HTML_BUF_CAP)
		? (size_t)content_length + 1 : 8192;
	uint8_t *body = heap_caps_malloc(bufcaps, MALLOC_CAP_SPIRAM);
	size_t total = 0;
	if (!body) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return httpd_resp_send_500(req);
	}

	bool upstream_err = false;
	for (;;) {
		if (total >= bufcaps) {
			size_t newcap = bufcaps * 2;
			if (newcap > HTML_BUF_CAP) {
				upstream_err = true;
				break;
			}
			uint8_t *nb = heap_caps_realloc(body, newcap, MALLOC_CAP_SPIRAM);
			if (!nb) {
				upstream_err = true;
				break;
			}
			body = nb;
			bufcaps = newcap;
		}
		int toread = (int)(bufcaps - total);
		if (toread > PROXY_CHUNK_SZ)
			toread = PROXY_CHUNK_SZ;
		int r = esp_http_client_read(client, (char *)body + total, toread);
		if (r < 0) {
			if (r == ESP_ERR_HTTP_EAGAIN && total > 0)
				break;
			upstream_err = true;
			break;
		}
		if (r == 0)
			break;
		total += (size_t)r;
	}

	if (upstream_err) {
		heap_caps_free(body);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		if (client_body) heap_caps_free(client_body);
		if (fw_upstream_held) firewall_upstream_release();
		return send_502(req);
	}

	bool store_gzip = is_gzip;
	if (is_html) {
		if (is_gzip) {
			uint8_t *dec = heap_caps_malloc(HTML_BUF_CAP, MALLOC_CAP_SPIRAM);
			size_t declen = 0;
			if (dec && gzip_inflate(body, total, dec, HTML_BUF_CAP,
			                        &declen) == 0) {
				heap_caps_free(body);
				body = dec;
				total = declen;
				store_gzip = false;
			} else {
				ESP_LOGW(TAG, "gzip inflate failed, passing through");
				if (dec)
					heap_caps_free(dec);
			}
		}
		if (!store_gzip) {
			char *pos = rfind_body_tag((char *)body, total);
			size_t snip = strlen(INJECT_SNIPPET);
			size_t newlen = total + snip;
			uint8_t *nb = heap_caps_malloc(newlen + 1, MALLOC_CAP_SPIRAM);
			if (nb) {
				if (pos) {
					size_t before = (size_t)(pos - (char *)body);
					memcpy(nb, body, before);
					memcpy(nb + before, INJECT_SNIPPET, snip);
					memcpy(nb + before + snip, pos, total - before);
				} else {
					memcpy(nb, body, total);
					memcpy(nb + total, INJECT_SNIPPET, snip);
				}
				nb[newlen] = '\0';
				heap_caps_free(body);
				body = nb;
				total = newlen;
			}
		}
	}

	if (status == 200 && total > 0)
		cache_store(cache_key, serve_ct, store_gzip, body, total);

	if (store_gzip)
		httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_send(req, (const char *)body, (ssize_t)total);

	heap_caps_free(body);
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
	if (client_body) heap_caps_free(client_body);
	if (fw_upstream_held) firewall_upstream_release();
	return ESP_OK;
}

static esp_err_t handle_thalow_connect(httpd_req_t *req) {
	int len = req->content_len;
	if (len <= 0 || len > 512) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req,
		                "{\"success\":false,\"reason\":\"Invalid SSID\"}",
		                -1);
		return ESP_OK;
	}

	char *body = malloc((size_t)len + 1);
	if (!body) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	int got = 0;
	while (got < len) {
		int r = httpd_req_recv(req, body + got, len - got);
		if (r <= 0) {
			free(body);
			httpd_resp_send_500(req);
			return ESP_FAIL;
		}
		got += r;
	}
	body[len] = '\0';

	cJSON *root = cJSON_Parse(body);
	free(body);

	cJSON *jssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
	cJSON *jpw = root ? cJSON_GetObjectItem(root, "password") : NULL;

	const char *ssid = (jssid && cJSON_IsString(jssid)) ? jssid->valuestring
	                                                     : NULL;
	const char *pw = (jpw && cJSON_IsString(jpw)) ? jpw->valuestring : "";

	size_t sl = ssid ? strlen(ssid) : 0;
	size_t pl = strlen(pw);
	if (!ssid || sl < 1 || sl > 32 || pl > 63) {
		if (root) cJSON_Delete(root);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_set_type(req, "application/json");
		httpd_resp_send(req,
		                "{\"success\":false,\"reason\":\"Invalid SSID\"}",
		                -1);
		return ESP_OK;
	}

	char ip[16] = "";
	char reason[48] = "";
	ESP_LOGI(TAG, "STA connect request ssid='%s'", ssid);
	/* 30 s budget: STA_CONNECT_TRIES=6 x ~5 s association timeout.
	 * On failure wifi_sta_connect() rolls back to the previous network. */
	esp_err_t e = wifi_sta_connect(ssid, pw, ip, sizeof(ip),
	                               reason, sizeof(reason), 30000);

	cJSON *resp = cJSON_CreateObject();
	if (e == ESP_OK) {
		cJSON_AddBoolToObject(resp, "success", true);
		cJSON_AddStringToObject(resp, "ssid", ssid);
		cJSON_AddStringToObject(resp, "ip", ip);
	} else {
		cJSON_AddBoolToObject(resp, "success", false);
		cJSON_AddStringToObject(resp, "reason",
		                        reason[0] ? reason : "Connection failed");
	}
	cJSON_Delete(root);

	char *out = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!out) {
		httpd_resp_send_500(req);
		return ESP_FAIL;
	}
	httpd_resp_set_type(req, "application/json");
	esp_err_t rc = httpd_resp_send(req, out, -1);
	free(out);
	return rc;
}

esp_err_t web_proxy_init(void) {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = WEB_PROXY_PORT;
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.max_uri_handlers = 12;
	config.stack_size = 8192;
	config.max_resp_headers = 16;
	config.recv_wait_timeout = 30;
	config.send_wait_timeout = 30;
	/* Each proxied request consumes an httpd socket AND an
	 * esp_http_client socket to the radio, so we budget httpd generously
	 * (16) while LWIP_MAX_SOCKETS=48 covers both halves with headroom for
	 * Firefox's 6 parallel conns, OS captive probes, mDNS and the DNS
	 * responder. lru_purge_enable makes httpd evict the oldest idle socket
	 * when the table is full instead of refusing accept() — that was the
	 * "error in accept (113)" / Firefox-stalls-then-dies failure. */
	config.max_open_sockets = 16;
	config.backlog_conn = 32;
	config.lru_purge_enable = true;

	httpd_handle_t server = NULL;
	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
		return ret;
	}

	httpd_uri_t cfg_get = {
		.uri = "/api/thalow_cfg",
		.method = HTTP_GET,
		.handler = handle_thalow_cfg_get,
		.user_ctx = NULL,
	};
	httpd_uri_t cfg_post = {
		.uri = "/api/thalow_cfg",
		.method = HTTP_POST,
		.handler = handle_thalow_cfg_post,
		.user_ctx = NULL,
	};
	httpd_uri_t inject = {
		.uri = "/thalow_inject.js",
		.method = HTTP_GET,
		.handler = handle_inject_js,
		.user_ctx = NULL,
	};
	httpd_uri_t scan = {
		.uri = "/api/thalow_scan",
		.method = HTTP_GET,
		.handler = handle_thalow_scan,
		.user_ctx = NULL,
	};
	httpd_uri_t connect = {
		.uri = "/api/thalow_connect",
		.method = HTTP_POST,
		.handler = handle_thalow_connect,
		.user_ctx = NULL,
	};
	httpd_uri_t stats = {
		.uri = "/api/thalow_stats",
		.method = HTTP_GET,
		.handler = handle_thalow_stats,
		.user_ctx = NULL,
	};
	httpd_uri_t reset = {
		.uri = "/api/thalow_reset",
		.method = HTTP_POST,
		.handler = handle_thalow_reset,
		.user_ctx = NULL,
	};
	httpd_uri_t proxy_get = {
		.uri = "/*",
		.method = HTTP_GET,
		.handler = handle_proxy,
		.user_ctx = NULL,
	};
	httpd_uri_t proxy_post = {
		.uri = "/*",
		.method = HTTP_POST,
		.handler = handle_proxy,
		.user_ctx = NULL,
	};

	httpd_register_uri_handler(server, &cfg_get);
	httpd_register_uri_handler(server, &cfg_post);
	httpd_register_uri_handler(server, &inject);
	httpd_register_uri_handler(server, &scan);
	httpd_register_uri_handler(server, &connect);
	httpd_register_uri_handler(server, &stats);
	httpd_register_uri_handler(server, &reset);
	httpd_register_uri_handler(server, &proxy_get);
	httpd_register_uri_handler(server, &proxy_post);

	for (int c = 0; c < portNUM_PROCESSORS; c++) {
		if (esp_register_freertos_idle_hook_for_cpu(
		        stats_idle_hook, c) != ESP_OK)
			ESP_LOGW(TAG, "failed to register idle hook cpu%d", c);
	}

	captive_portal_init();
	firewall_init();

	ESP_LOGI(TAG, "reverse-proxy listening on port %d -> %s:%d",
	         WEB_PROXY_PORT, HALOW_WEB_HOST, HALOW_WEB_PORT);
	return ESP_OK;
}
