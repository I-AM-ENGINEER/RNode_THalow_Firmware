#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* --- Board identity --- */
#define BOARD_NAME        "ESP32-S3-WROOM-1 N16R8"
#define FLASH_SIZE_MB     (16)
#define PSRAM_SIZE_MB     (8)

/* --- UART1: radio interface --- */
#define RADIO_UART_PORT      (UART_NUM_1)
#define RADIO_UART_TX_PIN    (5)
#define RADIO_UART_RX_PIN    (4)
#define RADIO_UART_BAUDRATE  (2000000)

/* --- WiFi --- */
#define WIFI_AP_SSID        "RNode-HaLow"
#define WIFI_AP_MAX_CONN    (4)

/* --- SLIP netif --- */
#define SLIP_LOCAL_IP   PP_HTONL(LWIP_MAKEU32(192, 168, 7, 1))
#define SLIP_PEER_IP    PP_HTONL(LWIP_MAKEU32(192, 168, 7, 2))
#define SLIP_NETMASK    PP_HTONL(LWIP_MAKEU32(255, 255, 255, 252))

/* --- BLE (Nordic UART Service, NimBLE) --- */
#define BLE_DEVICE_NAME      "RNode HaLow"
#define BLE_PASSKEY          (123456)
#define BLE_PAIRING_TIMEOUT  (35000)

/* --- Status LED + BOOT button --- */
#define LED_PIN              (38)
#define LED_ACTIVE_LOW       (1)
#define BUTTON_PIN           (0)
#define BUTTON_PAIRING_HOLD  (3000)

/* --- Logging --- */
#define LOG_TAG_MAIN        "rnode"

#ifdef __cplusplus
}
#endif
