#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* --- Board identity --- */
#define BOARD_NAME        "ESP32-S3-WROOM-1 N16R8"
#define FLASH_SIZE_MB     (16)
#define PSRAM_SIZE_MB     (8)

/* --- Firmware version ---
 * FW_VERSION is no longer defined here. It is injected automatically at
 * build time from git (see src/CMakeLists.txt): a release tag when HEAD is
 * exactly on a tag, otherwise "v0.0.0".
 */

/* --- UART1: radio interface --- */
#define RADIO_UART_PORT      (UART_NUM_1)
#define RADIO_UART_TX_PIN    (5)
#define RADIO_UART_RX_PIN    (4)
#define RADIO_UART_BAUDRATE  (2000000)

/* --- WiFi --- */
#define WIFI_AP_SSID        "RNode-HaLow"
#define WIFI_AP_MAX_CONN    (4)
#define WIFI_AP_PASSWORD    ""
#define WIFI_AP_IP          PP_HTONL(LWIP_MAKEU32(10, 10, 0, 2))
#define WIFI_AP_GW          PP_HTONL(LWIP_MAKEU32(10, 10, 0, 2))
#define WIFI_AP_NETMASK     PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0))
#define WEB_PROXY_PORT      (80)
#define HALOW_WEB_HOST      "192.168.7.2"
#define HALOW_WEB_PORT      (80)

/* --- SLIP netif --- */
#define SLIP_LOCAL_IP   PP_HTONL(LWIP_MAKEU32(192, 168, 7, 1))
#define SLIP_PEER_IP    PP_HTONL(LWIP_MAKEU32(192, 168, 7, 2))
#define SLIP_NETMASK    PP_HTONL(LWIP_MAKEU32(255, 255, 255, 252))

/* --- RNS (Reticulum) TCP proxy ---
 * Listens on this TCP port (bound to all interfaces) and forwards each
 * accepted connection as a raw byte-stream pipe to the radio's RNS TCP
 * interface at SLIP_PEER_IP:RNS_PROXY_UPSTREAM_PORT. Lets WiFi/STA clients
 * use the RNode as a Reticulum TCP interface without speaking BLE. */
#define RNS_PROXY_PORT            (4242)
#define RNS_PROXY_UPSTREAM_PORT   (4242)
#define RNS_PROXY_MAX_CLIENTS     (4)
#define RNS_PROXY_IDLE_TIMEOUT_MS (120000)
#define RNS_PROXY_BUF_SZ          (1024)

/* --- BLE (Nordic UART Service, NimBLE) --- */
#define BLE_DEVICE_NAME      "RNode HaLow"
#define BLE_PASSKEY          (123456)
#define BLE_PAIRING_TIMEOUT  (35000)

/* --- BLE advertising (two-phase) ---
 * After boot / disconnect / pairing-enabled, the device does a burst of FAST
 * advertising so Android/iOS scanners in the system Bluetooth settings can
 * discover and pair within ~1 second. After the burst, it drops to SLOW
 * advertising to save power (roughly 5-10x lower RF duty cycle).
 *
 * Fast interval:   32..64   units of 0.625 ms = 20..40 ms   (~25-50 Hz)
 * Slow interval:   2048..2560 units of 0.625 ms = 1280..1600 ms (~0.7 Hz)
 *
 * The fast burst runs for BLE_ADV_FAST_MS. Android's system scanner
 * implements match filtering on the controller; at >100 ms intervals it
 * routinely drops devices, especially when Wi-Fi/BLE coexistence steals
 * RF slots. Keeping the interval <= ~40 ms during discovery makes the
 * device reliably visible in Bluetooth settings. */
#define BLE_ADV_FAST_MS     (30000)
#define BLE_ADV_FAST_MIN    (32)
#define BLE_ADV_FAST_MAX    (64)
#define BLE_ADV_SLOW_MIN    (2048)
#define BLE_ADV_SLOW_MAX    (2560)

/* --- Status LED + BOOT button --- */
#define LED_PIN              (38)
#define LED_ACTIVE_LOW       (1)
#define BUTTON_PIN           (0)
#define BUTTON_PAIRING_HOLD  (3000)
#define BUTTON_FACTORY_RESET_HOLD (30000)

/* --- Battery (GPIO3, divider 2:1) ---
 * GPIO3 maps to ADC1 channel 2 on ESP32-S3 (see soc/adc_channel.h:
 * ADC1_GPIO3_CHANNEL = 2). Vadc = Vbat/2, 100k/100k divider.
 */
#define BATTERY_ADC_GPIO     (3)
#define BATTERY_ADC_CH       (ADC_CHANNEL_2)
#define BATTERY_DIVIDER      (2.0f)
#define BATTERY_SAMPLE_MS    (3000)
#define BATTERY_OVERSAMPLE   (32)

/* --- Logging --- */
#define LOG_TAG_MAIN        "rnode"

#ifdef __cplusplus
}
#endif
