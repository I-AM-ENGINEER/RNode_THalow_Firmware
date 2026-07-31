#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(void);
esp_err_t wifi_apply(void);

esp_err_t wifi_scan(wifi_ap_record_t *out, uint16_t cap, uint16_t *count);

esp_err_t wifi_get_sta_ip_info(esp_netif_ip_info_t *out);
esp_err_t wifi_get_ap_ip_info(esp_netif_ip_info_t *out);

esp_err_t wifi_sta_connect(const char *ssid, const char *password,
                           char *ip_out, size_t ip_sz,
                           char *reason_out, size_t reason_sz,
                           int timeout_ms);

bool wifi_sta_connected(void);
int wifi_ap_station_count(void);

/* Coarse STA connection state for stats display. One of:
 *   "idle"        - STA disabled or not started
 *   "connecting"  - a connect attempt is in progress
 *   "connected"   - associated and have an IP
 *   "failed"      - last attempt failed and we gave up */
const char *wifi_sta_status(void);

/* Current RSSI of the associated AP, or INT32_MIN if not connected. */
int32_t wifi_sta_rssi(void);

/* SSID the STA is configured for / associated with. May be empty. */
const char *wifi_sta_configured_ssid(void);

/* mDNS hostname (e.g. "rnode-halow-a75210.local"); empty if mDNS not inited. */
const char *wifi_get_hostname(void);

#ifdef __cplusplus
}
#endif
