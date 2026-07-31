#include <string.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "lwip/ip4_addr.h"

#include "thalow_config.h"
#include "config/project_config.h"

static const char *TAG = "thalow_cfg";

static nvs_handle_t nvs;
static bool nvs_ok = false;

static char ap_ssid_buf[33];
static char ap_pw_buf[64];
static char ap_ip_buf[16];
static char ap_nm_buf[16];
static char sta_ssid_buf[33];
static char sta_pw_buf[64];
static char sta_ip_buf[16];
static char sta_nm_buf[16];
static char sta_gw_buf[16];
static char ble_name_buf[40];

static bool wifi_ap_enabled = true;
static thalow_auth_t wifi_ap_authmode = THALOW_AUTH_OPEN;
static bool wifi_sta_enabled = false;
static bool wifi_sta_dhcp = true;
static bool ble_enabled = true;
static led_mode_t led_mode = LED_MODE_BLE;
static bool led_heartbeat = false;
static uint16_t rns_proxy_port = 0;  /* 0 = use RNS_PROXY_PORT compile default */
static bool rns_proxy_enabled = true;
static char rns_proxy_wl_buf[80] = "0.0.0.0/0";  /* CIDR whitelist; default 0.0.0.0/0 = allow all (matches radio default) */

static char mac_suffix[7];

static void load_str(const char *key, char *dst, size_t dstsz, const char *def) {
	size_t len = dstsz;
	esp_err_t err = nvs_get_str(nvs, key, dst, &len);
	if (err != ESP_OK) {
		strncpy(dst, def, dstsz - 1);
		dst[dstsz - 1] = '\0';
		ESP_LOGI(TAG, "%s not set, using default", key);
	}
}

static bool load_bool(const char *key, bool def) {
	int8_t v = def ? 1 : 0;
	esp_err_t err = nvs_get_i8(nvs, key, &v);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "%s not set, using default %d", key, def ? 1 : 0);
		return def;
	}
	return v ? true : false;
}

static int8_t load_i8(const char *key, int8_t def) {
	int8_t v = def;
	esp_err_t err = nvs_get_i8(nvs, key, &v);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "%s not set, using default %d", key, def);
		return def;
	}
	return v;
}

static uint16_t load_u16(const char *key, uint16_t def) {
	uint16_t v = def;
	esp_err_t err = nvs_get_u16(nvs, key, &v);
	if (err != ESP_OK) {
		ESP_LOGI(TAG, "%s not set, using default %u", key, def);
		return def;
	}
	return v;
}

static esp_err_t save_i8(const char *key, int8_t v) {	if (!nvs_ok)
		return ESP_ERR_INVALID_STATE;
	esp_err_t err = nvs_set_i8(nvs, key, v);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_i8(%s): %s", key, esp_err_to_name(err));
		return err;
	}
	err = nvs_commit(nvs);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "nvs_commit(%s): %s", key, esp_err_to_name(err));
	return err;
}

static esp_err_t save_u16(const char *key, uint16_t v) {
	if (!nvs_ok)
		return ESP_ERR_INVALID_STATE;
	esp_err_t err = nvs_set_u16(nvs, key, v);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_u16(%s): %s", key, esp_err_to_name(err));
		return err;
	}
	err = nvs_commit(nvs);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "nvs_commit(%s): %s", key, esp_err_to_name(err));
	return err;
}

static esp_err_t save_str(const char *key, const char *val) {
	if (!nvs_ok)
		return ESP_ERR_INVALID_STATE;
	esp_err_t err = nvs_set_str(nvs, key, val);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_str(%s): %s", key, esp_err_to_name(err));
		return err;
	}
	err = nvs_commit(nvs);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "nvs_commit(%s): %s", key, esp_err_to_name(err));
	return err;
}

void thalow_config_mac_suffix(char *dst, size_t sz) {
	strncpy(dst, mac_suffix, sz - 1);
	dst[sz - 1] = '\0';
}

static bool parse_ip4(const char *str, ip4_addr_t *out) {
	if (!str || !out)
		return false;
	return ip4addr_aton(str, out) == 1;
}

void thalow_config_init(void) {
	uint8_t mac[6];
	esp_err_t err = esp_efuse_mac_get_default(mac);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s",
		         esp_err_to_name(err));
		mac[3] = 0; mac[4] = 0; mac[5] = 0;
	}
	snprintf(mac_suffix, sizeof(mac_suffix), "%02X%02X%02X",
	         mac[3], mac[4], mac[5]);

	err = nvs_open("thalow", NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
		strncpy(ap_ssid_buf, WIFI_AP_SSID, sizeof(ap_ssid_buf) - 1);
		ap_ssid_buf[sizeof(ap_ssid_buf) - 1] = '\0';
		ap_pw_buf[0] = '\0';
		return;
	}
	nvs_ok = true;

	char def_ap_ssid[40];
	snprintf(def_ap_ssid, sizeof(def_ap_ssid), "RNode-Halow-%s", mac_suffix);
	load_str("ap_ssid", ap_ssid_buf, sizeof(ap_ssid_buf), def_ap_ssid);

	load_str("ap_pass", ap_pw_buf, sizeof(ap_pw_buf),
	         WIFI_AP_PASSWORD);

	load_str("ap_ip", ap_ip_buf, sizeof(ap_ip_buf), "10.10.0.2");
	load_str("ap_mask", ap_nm_buf, sizeof(ap_nm_buf),
	         "255.255.255.0");

	load_str("sta_ssid", sta_ssid_buf, sizeof(sta_ssid_buf), "");
	load_str("sta_pass", sta_pw_buf, sizeof(sta_pw_buf), "");
	load_str("sta_ip", sta_ip_buf, sizeof(sta_ip_buf), "");
	load_str("sta_mask", sta_nm_buf, sizeof(sta_nm_buf), "");
	load_str("sta_gw", sta_gw_buf, sizeof(sta_gw_buf), "");

	char def_ble_name[40];
	snprintf(def_ble_name, sizeof(def_ble_name), "RNode HaLow %s",
	         mac_suffix);
	load_str("ble_name", ble_name_buf, sizeof(ble_name_buf), def_ble_name);

	wifi_ap_enabled = load_bool("ap_en", true);
	int8_t am = load_i8("ap_auth", THALOW_AUTH_OPEN);
	if (am < THALOW_AUTH_OPEN || am > THALOW_AUTH_WPA2_WPA3)
		am = THALOW_AUTH_OPEN;
	wifi_ap_authmode = (thalow_auth_t)am;
	wifi_sta_enabled = load_bool("sta_en", false);
	wifi_sta_dhcp = load_bool("sta_dhcp", true);
	ble_enabled = load_bool("ble_en", true);

	int8_t lm = load_i8("led_mode", LED_MODE_BLE);
	if (lm < LED_MODE_BLE || lm > LED_MODE_TRAFFIC)
		lm = LED_MODE_BLE;
	led_mode = (led_mode_t)lm;
	led_heartbeat = load_bool("led_hb", false);
	/* 0 = "not set": rns_proxy falls back to RNS_PROXY_PORT compile default. */
	rns_proxy_port = load_u16("rns_port", 0);
	rns_proxy_enabled = load_bool("rns_en", true);
	load_str("rns_wl", rns_proxy_wl_buf, sizeof(rns_proxy_wl_buf), "0.0.0.0/0");

	const char *am_str = wifi_ap_authmode == THALOW_AUTH_WPA2_WPA3 ? "WPA2_WPA3"
	                    : wifi_ap_authmode == THALOW_AUTH_WPA3 ? "WPA3"
	                    : wifi_ap_authmode == THALOW_AUTH_WPA2 ? "WPA2"
	                    : "OPEN";
	ESP_LOGI(TAG, "ssid=%s auth=%s ap_en=%d sta_en=%d dhcp=%d ble_en=%d led_mode=%d hb=%d",
	         ap_ssid_buf, am_str,
	         wifi_ap_enabled ? 1 : 0, wifi_sta_enabled ? 1 : 0,
	         wifi_sta_dhcp ? 1 : 0, ble_enabled ? 1 : 0,
	         led_mode, led_heartbeat ? 1 : 0);
}

static void erase_keys(const char *keys[], size_t n) {
	for (size_t i = 0; i < n; i++)
		nvs_erase_key(nvs, keys[i]);
}

esp_err_t thalow_config_reset(const char *section) {
	if (!nvs_ok || !section)
		return ESP_ERR_INVALID_STATE;

	char def_ap_ssid[40];
	snprintf(def_ap_ssid, sizeof(def_ap_ssid), "RNode-Halow-%s", mac_suffix);
	char def_ble_name[40];
	snprintf(def_ble_name, sizeof(def_ble_name), "RNode HaLow %s", mac_suffix);

	if (strcmp(section, "ap") == 0) {
		const char *keys[] = {"ap_en","ap_ssid","ap_pass","ap_auth","ap_ip","ap_mask"};
		erase_keys(keys, sizeof(keys)/sizeof(keys[0]));
		wifi_ap_enabled = true;
		wifi_ap_authmode = THALOW_AUTH_OPEN;
		strncpy(ap_ssid_buf, def_ap_ssid, sizeof(ap_ssid_buf)-1); ap_ssid_buf[sizeof(ap_ssid_buf)-1]='\0';
		ap_pw_buf[0]='\0';
		strncpy(ap_ip_buf, "10.10.0.2", sizeof(ap_ip_buf)-1); ap_ip_buf[sizeof(ap_ip_buf)-1]='\0';
		strncpy(ap_nm_buf, "255.255.255.0", sizeof(ap_nm_buf)-1); ap_nm_buf[sizeof(ap_nm_buf)-1]='\0';
	} else if (strcmp(section, "sta") == 0) {
		const char *keys[] = {"sta_en","sta_ssid","sta_pass","sta_dhcp","sta_ip","sta_mask","sta_gw"};
		erase_keys(keys, sizeof(keys)/sizeof(keys[0]));
		wifi_sta_enabled = false;
		wifi_sta_dhcp = true;
		sta_ssid_buf[0]='\0';
		sta_pw_buf[0]='\0';
		sta_ip_buf[0]='\0';
		sta_nm_buf[0]='\0';
		sta_gw_buf[0]='\0';
	} else if (strcmp(section, "ble") == 0) {
		const char *keys[] = {"ble_en","ble_name"};
		erase_keys(keys, sizeof(keys)/sizeof(keys[0]));
		ble_enabled = true;
		strncpy(ble_name_buf, def_ble_name, sizeof(ble_name_buf)-1); ble_name_buf[sizeof(ble_name_buf)-1]='\0';
	} else if (strcmp(section, "led") == 0) {
		const char *keys[] = {"led_mode","led_hb"};
		erase_keys(keys, sizeof(keys)/sizeof(keys[0]));
		led_mode = LED_MODE_BLE;
		led_heartbeat = false;
	} else if (strcmp(section, "tcp") == 0) {
		const char *keys[] = {"rns_en","rns_port","rns_wl"};
		erase_keys(keys, sizeof(keys)/sizeof(keys[0]));
		rns_proxy_enabled = true;
		rns_proxy_port = 0;
		strncpy(rns_proxy_wl_buf, "0.0.0.0/0", sizeof(rns_proxy_wl_buf) - 1);
		rns_proxy_wl_buf[sizeof(rns_proxy_wl_buf) - 1] = '\0';
	} else {
		return ESP_ERR_INVALID_ARG;
	}
	nvs_commit(nvs);
	ESP_LOGI(TAG, "section '%s' reset to defaults", section);
	return ESP_OK;
}

/* --- WiFi AP enabled --- */
bool thalow_config_get_wifi_ap_enabled(void) {
	return wifi_ap_enabled;
}

esp_err_t thalow_config_set_wifi_ap_enabled(bool v) {
	wifi_ap_enabled = v;
	return save_i8("ap_en", v ? 1 : 0);
}

/* --- AP IP --- */
const char *thalow_config_get_wifi_ap_ip(void) {
	return ap_ip_buf;
}

esp_err_t thalow_config_set_wifi_ap_ip(const char *ip) {
	if (!ip)
		return ESP_ERR_INVALID_ARG;
	ip4_addr_t tmp;
	if (!parse_ip4(ip, &tmp))
		return ESP_ERR_INVALID_ARG;
	if (strlen(ip) > sizeof(ap_ip_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(ap_ip_buf, ip, sizeof(ap_ip_buf) - 1);
	ap_ip_buf[sizeof(ap_ip_buf) - 1] = '\0';
	return save_str("ap_ip", ap_ip_buf);
}

/* --- AP netmask --- */
const char *thalow_config_get_wifi_ap_netmask(void) {
	return ap_nm_buf;
}

esp_err_t thalow_config_set_wifi_ap_netmask(const char *nm) {
	if (!nm)
		return ESP_ERR_INVALID_ARG;
	ip4_addr_t tmp;
	if (!parse_ip4(nm, &tmp))
		return ESP_ERR_INVALID_ARG;
	if (strlen(nm) > sizeof(ap_nm_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(ap_nm_buf, nm, sizeof(ap_nm_buf) - 1);
	ap_nm_buf[sizeof(ap_nm_buf) - 1] = '\0';
	return save_str("ap_mask", ap_nm_buf);
}

thalow_auth_t thalow_config_get_wifi_ap_authmode(void) {
	return wifi_ap_authmode;
}

esp_err_t thalow_config_set_wifi_ap_authmode(thalow_auth_t m) {
	if (m != THALOW_AUTH_OPEN && m != THALOW_AUTH_WPA2 && m != THALOW_AUTH_WPA3 && m != THALOW_AUTH_WPA2_WPA3)
		return ESP_ERR_INVALID_ARG;
	wifi_ap_authmode = m;
	return save_i8("ap_auth", (int8_t)m);
}

/* --- SSID / password (AP) --- */
const char *thalow_config_get_ssid(void) {
	return ap_ssid_buf;
}

esp_err_t thalow_config_set_ssid(const char *ssid) {
	if (!ssid)
		return ESP_ERR_INVALID_ARG;

	size_t l = strlen(ssid);
	if (l < 1 || l > 32)
		return ESP_ERR_INVALID_SIZE;
	if (ssid[0] == ' ' || ssid[l - 1] == ' ')
		return ESP_ERR_INVALID_ARG;

	strncpy(ap_ssid_buf, ssid, sizeof(ap_ssid_buf) - 1);
	ap_ssid_buf[sizeof(ap_ssid_buf) - 1] = '\0';
	return save_str("ap_ssid", ap_ssid_buf);
}

const char *thalow_config_get_password(void) {
	return ap_pw_buf;
}

esp_err_t thalow_config_set_password(const char *pw) {
	if (!pw)
		return ESP_ERR_INVALID_ARG;

	size_t l = strlen(pw);
	if (l > 63)
		return ESP_ERR_INVALID_SIZE;
	if (l > 0 && l < 8)
		return ESP_ERR_INVALID_ARG;

	strncpy(ap_pw_buf, pw, sizeof(ap_pw_buf) - 1);
	ap_pw_buf[sizeof(ap_pw_buf) - 1] = '\0';
	return save_str("ap_pass", ap_pw_buf);
}

/* --- WiFi STA enabled --- */
bool thalow_config_get_wifi_sta_enabled(void) {
	return wifi_sta_enabled;
}

esp_err_t thalow_config_set_wifi_sta_enabled(bool v) {
	wifi_sta_enabled = v;
	return save_i8("sta_en", v ? 1 : 0);
}

/* --- STA SSID --- */
const char *thalow_config_get_wifi_sta_ssid(void) {
	return sta_ssid_buf;
}

esp_err_t thalow_config_set_wifi_sta_ssid(const char *ssid) {
	if (!ssid)
		return ESP_ERR_INVALID_ARG;

	size_t l = strlen(ssid);
	if (l > 32)
		return ESP_ERR_INVALID_SIZE;
	if (l > 0 && (ssid[0] == ' ' || ssid[l - 1] == ' '))
		return ESP_ERR_INVALID_ARG;

	strncpy(sta_ssid_buf, ssid, sizeof(sta_ssid_buf) - 1);
	sta_ssid_buf[sizeof(sta_ssid_buf) - 1] = '\0';
	return save_str("sta_ssid", sta_ssid_buf);
}

/* --- STA password --- */
const char *thalow_config_get_wifi_sta_password(void) {
	return sta_pw_buf;
}

esp_err_t thalow_config_set_wifi_sta_password(const char *pw) {
	if (!pw)
		return ESP_ERR_INVALID_ARG;

	size_t l = strlen(pw);
	if (l > 63)
		return ESP_ERR_INVALID_SIZE;
	if (l > 0 && l < 8)
		return ESP_ERR_INVALID_ARG;

	strncpy(sta_pw_buf, pw, sizeof(sta_pw_buf) - 1);
	sta_pw_buf[sizeof(sta_pw_buf) - 1] = '\0';
	return save_str("sta_pass", sta_pw_buf);
}

/* --- STA DHCP --- */
bool thalow_config_get_wifi_sta_dhcp(void) {
	return wifi_sta_dhcp;
}

esp_err_t thalow_config_set_wifi_sta_dhcp(bool v) {
	wifi_sta_dhcp = v;
	return save_i8("sta_dhcp", v ? 1 : 0);
}

/* --- STA IP --- */
const char *thalow_config_get_wifi_sta_ip(void) {
	return sta_ip_buf;
}

esp_err_t thalow_config_set_wifi_sta_ip(const char *ip) {
	if (!ip)
		return ESP_ERR_INVALID_ARG;
	if (strlen(ip) > sizeof(sta_ip_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(sta_ip_buf, ip, sizeof(sta_ip_buf) - 1);
	sta_ip_buf[sizeof(sta_ip_buf) - 1] = '\0';
	return save_str("sta_ip", sta_ip_buf);
}

/* --- STA netmask --- */
const char *thalow_config_get_wifi_sta_netmask(void) {
	return sta_nm_buf;
}

esp_err_t thalow_config_set_wifi_sta_netmask(const char *nm) {
	if (!nm)
		return ESP_ERR_INVALID_ARG;
	if (strlen(nm) > sizeof(sta_nm_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(sta_nm_buf, nm, sizeof(sta_nm_buf) - 1);
	sta_nm_buf[sizeof(sta_nm_buf) - 1] = '\0';
	return save_str("sta_mask", sta_nm_buf);
}

/* --- STA gateway --- */
const char *thalow_config_get_wifi_sta_gw(void) {
	return sta_gw_buf;
}

esp_err_t thalow_config_set_wifi_sta_gw(const char *gw) {
	if (!gw)
		return ESP_ERR_INVALID_ARG;
	if (strlen(gw) > sizeof(sta_gw_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(sta_gw_buf, gw, sizeof(sta_gw_buf) - 1);
	sta_gw_buf[sizeof(sta_gw_buf) - 1] = '\0';
	return save_str("sta_gw", sta_gw_buf);
}

/* --- BLE enabled --- */
bool thalow_config_get_ble_enabled(void) {
	return ble_enabled;
}

esp_err_t thalow_config_set_ble_enabled(bool v) {
	ble_enabled = v;
	return save_i8("ble_en", v ? 1 : 0);
}

/* --- BLE name --- */
const char *thalow_config_get_ble_name(void) {
	return ble_name_buf;
}

esp_err_t thalow_config_set_ble_name(const char *name) {
	if (!name)
		return ESP_ERR_INVALID_ARG;
	if (strlen(name) > sizeof(ble_name_buf) - 1)
		return ESP_ERR_INVALID_SIZE;
	strncpy(ble_name_buf, name, sizeof(ble_name_buf) - 1);
	ble_name_buf[sizeof(ble_name_buf) - 1] = '\0';
	return save_str("ble_name", ble_name_buf);
}

/* --- Status LED mode --- */
led_mode_t thalow_config_get_led_mode(void) {
	return led_mode;
}

esp_err_t thalow_config_set_led_mode(led_mode_t m) {
	if (m < LED_MODE_BLE || m > LED_MODE_TRAFFIC)
		return ESP_ERR_INVALID_ARG;
	led_mode = m;
	return save_i8("led_mode", (int8_t)m);
}

/* --- Status LED heartbeat --- */
bool thalow_config_get_led_heartbeat(void) {
	return led_heartbeat;
}

esp_err_t thalow_config_set_led_heartbeat(bool en) {
	led_heartbeat = en;
	return save_i8("led_hb", en ? 1 : 0);
}

/* --- RNS TCP proxy --- */
bool thalow_config_get_rns_proxy_enabled(void) {
	return rns_proxy_enabled;
}

esp_err_t thalow_config_set_rns_proxy_enabled(bool v) {
	rns_proxy_enabled = v;
	return save_i8("rns_en", v ? 1 : 0);
}

uint16_t thalow_config_get_rns_proxy_port(void) {
	return rns_proxy_port;
}

esp_err_t thalow_config_set_rns_proxy_port(uint16_t port) {
	rns_proxy_port = port;
	return save_u16("rns_port", port);
}

const char *thalow_config_get_rns_proxy_whitelist(void) {
	return rns_proxy_wl_buf;
}

esp_err_t thalow_config_set_rns_proxy_whitelist(const char *wl) {
	if (!wl)
		return ESP_ERR_INVALID_ARG;
	if (strlen(wl) >= sizeof(rns_proxy_wl_buf))
		return ESP_ERR_INVALID_SIZE;
	strncpy(rns_proxy_wl_buf, wl, sizeof(rns_proxy_wl_buf) - 1);
	rns_proxy_wl_buf[sizeof(rns_proxy_wl_buf) - 1] = '\0';
	return save_str("rns_wl", rns_proxy_wl_buf);
}
