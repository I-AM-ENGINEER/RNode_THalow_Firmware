#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { THALOW_AUTH_OPEN=0, THALOW_AUTH_WPA2=1, THALOW_AUTH_WPA3=2, THALOW_AUTH_WPA2_WPA3=3 } thalow_auth_t;

typedef enum {
	LED_MODE_BLE = 0,
	LED_MODE_TCP = 1,
	LED_MODE_WIFI_STA = 2,
	LED_MODE_WIFI_AP = 3,
	LED_MODE_TRAFFIC = 4,
} led_mode_t;

void thalow_config_init(void);

/* Reset a settings section to factory defaults ("ap"|"sta"|"ble"|"led").
 * Erases the section's NVS keys and reloads defaults into memory. */
esp_err_t thalow_config_reset(const char *section);

/* MAC suffix (last 3 bytes of base MAC, 6 uppercase hex chars) */
void thalow_config_mac_suffix(char *dst, size_t sz);

/* --- WiFi AP --- */
bool thalow_config_get_wifi_ap_enabled(void);
esp_err_t thalow_config_set_wifi_ap_enabled(bool v);

const char *thalow_config_get_wifi_ap_ip(void);
esp_err_t thalow_config_set_wifi_ap_ip(const char *ip);

const char *thalow_config_get_wifi_ap_netmask(void);
esp_err_t thalow_config_set_wifi_ap_netmask(const char *nm);

thalow_auth_t thalow_config_get_wifi_ap_authmode(void);
esp_err_t thalow_config_set_wifi_ap_authmode(thalow_auth_t m);

const char *thalow_config_get_ssid(void);
esp_err_t thalow_config_set_ssid(const char *ssid);

const char *thalow_config_get_password(void);
esp_err_t thalow_config_set_password(const char *pw);

/* --- WiFi STA --- */
bool thalow_config_get_wifi_sta_enabled(void);
esp_err_t thalow_config_set_wifi_sta_enabled(bool v);

const char *thalow_config_get_wifi_sta_ssid(void);
esp_err_t thalow_config_set_wifi_sta_ssid(const char *ssid);

const char *thalow_config_get_wifi_sta_password(void);
esp_err_t thalow_config_set_wifi_sta_password(const char *pw);

bool thalow_config_get_wifi_sta_dhcp(void);
esp_err_t thalow_config_set_wifi_sta_dhcp(bool v);

const char *thalow_config_get_wifi_sta_ip(void);
esp_err_t thalow_config_set_wifi_sta_ip(const char *ip);

const char *thalow_config_get_wifi_sta_netmask(void);
esp_err_t thalow_config_set_wifi_sta_netmask(const char *nm);

const char *thalow_config_get_wifi_sta_gw(void);
esp_err_t thalow_config_set_wifi_sta_gw(const char *gw);

/* --- BLE --- */
bool thalow_config_get_ble_enabled(void);
esp_err_t thalow_config_set_ble_enabled(bool v);

const char *thalow_config_get_ble_name(void);
esp_err_t thalow_config_set_ble_name(const char *name);

/* --- Status LED --- */
led_mode_t thalow_config_get_led_mode(void);
esp_err_t thalow_config_set_led_mode(led_mode_t m);

bool thalow_config_get_led_heartbeat(void);
esp_err_t thalow_config_set_led_heartbeat(bool en);

/* --- RNS (Reticulum) TCP proxy --- */
bool thalow_config_get_rns_proxy_enabled(void);
esp_err_t thalow_config_set_rns_proxy_enabled(bool v);

uint16_t thalow_config_get_rns_proxy_port(void);
esp_err_t thalow_config_set_rns_proxy_port(uint16_t port);

/* CIDR whitelist (e.g. "10.10.0.0/24, 192.168.1.5/32"). Empty = allow all. */
const char *thalow_config_get_rns_proxy_whitelist(void);
esp_err_t thalow_config_set_rns_proxy_whitelist(const char *wl);

#ifdef __cplusplus
}
#endif
