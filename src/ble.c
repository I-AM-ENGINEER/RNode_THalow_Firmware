#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init( void );

#include "ble.h"
#include "config/project_config.h"

static const char *TAG = "ble";

#define RX_STREAM_SIZE   (2048)
#define TX_BUFFER_SIZE   (512)
#define BLE_NOTIFY_MAX   (250)
#define STATUS_TASK_MS   (10)

/* Nordic UART Service UUIDs (little-endian byte order) */
/* 6e400001-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* 6e400002-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* 6e400003-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint8_t own_addr_type;
static char s_device_name[32];
static uint16_t active_conn = BLE_HS_CONN_HANDLE_NONE;
static bool tx_subscribed = false;
static uint16_t rx_handle;
static uint16_t tx_handle;

static ble_state_t ble_state = BLE_STATE_OFF;
static bool allow_pairing = false;

static StreamBufferHandle_t rx_stream;
static SemaphoreHandle_t tx_mutex;
static uint8_t tx_buf[TX_BUFFER_SIZE];
static size_t tx_buf_len = 0;

/* ------------------------------------------------------------------ */
/* GATT                                                               */
/* ------------------------------------------------------------------ */

static int gatt_access_cb( uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg ) {
	(void)attr;
	(void)arg;

	if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
		struct ble_gap_conn_desc desc;
		if (ble_gap_conn_find(conn, &desc) != 0 ||
		    !desc.sec_state.encrypted ||
		    !desc.sec_state.authenticated) {
			return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
		}

		uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
		if (len > TX_BUFFER_SIZE)
			len = TX_BUFFER_SIZE;

		uint8_t tmp[TX_BUFFER_SIZE];
		ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len);
		ESP_LOGD(TAG, "rx write %u bytes", len);
		xStreamBufferSend(rx_stream, tmp, len, 0);
	}

	return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &nus_svc_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]) {
			{
				.uuid = &nus_rx_uuid.u,
				.access_cb = gatt_access_cb,
				.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
				.val_handle = &rx_handle,
			},
			{
				.uuid = &nus_tx_uuid.u,
				.access_cb = gatt_access_cb,
				.flags = BLE_GATT_CHR_F_NOTIFY,
				.val_handle = &tx_handle,
			},
			{ 0 },
		},
	},
	{ 0 },
};

static int gatt_svr_init( void ) {
	ble_svc_gap_init();
	ble_svc_gatt_init();

	int rc = ble_gatts_count_cfg(gatt_svcs);
	if (rc != 0)
		return rc;

	return ble_gatts_add_svcs(gatt_svcs);
}

/* ------------------------------------------------------------------ */
/* Advertising                                                        */
/* ------------------------------------------------------------------ */

static int gap_event_cb( struct ble_gap_event *event, void *arg );

static void ble_advertise( void ) {
	struct ble_hs_adv_fields fields;
	struct ble_hs_adv_fields rsp;
	struct ble_gap_adv_params adv_params;
	int rc;

	/* Advertising data: flags + 128-bit NUS service UUID */
	memset(&fields, 0, sizeof(fields));
	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.uuids128 = (ble_uuid128_t[]) {
		BLE_UUID128_INIT(
		    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
		    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e)
	};
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;

	rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv set_fields failed: %d", rc);
		return;
	}

	/* Scan response: always include the device name. */
	memset(&rsp, 0, sizeof(rsp));
	{
		const char *name = ble_svc_gap_device_name();
		rsp.name = (uint8_t *)name;
		rsp.name_len = strlen(name);
		rsp.name_is_complete = 1;
	}

	rc = ble_gap_adv_rsp_set_fields(&rsp);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv rsp set_fields failed: %d", rc);
		return;
	}

	memset(&adv_params, 0, sizeof(adv_params));
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

	rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
	                       &adv_params, gap_event_cb, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv_start failed: %d", rc);
		return;
	}

	ESP_LOGI(TAG, "advertising started");
}

/* Restart advertising so a scan-response change (e.g. name visibility)
 * takes effect immediately. Safe to call from any task. */
static void ble_advertise_restart( void ) {
	ble_gap_adv_stop();
	ble_advertise();
}

/* ------------------------------------------------------------------ */
/* GAP events                                                         */
/* ------------------------------------------------------------------ */

static int gap_event_cb( struct ble_gap_event *event, void *arg ) {
	(void)arg;
	struct ble_gap_conn_desc desc;

	switch (event->type) {

	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status != 0) {
			ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
			ble_advertise();
			return 0;
		}
		active_conn = event->connect.conn_handle;
		ble_state = BLE_STATE_ON;
		ESP_LOGI(TAG, "connected (handle %u)", active_conn);

		ble_gap_set_prefered_le_phy(active_conn,
			BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
			BLE_GAP_LE_PHY_CODED_ANY);

		struct ble_gap_upd_params up = {
			.itvl_min = 6,
			.itvl_max = 12,
			.latency = 0,
			.supervision_timeout = 500,
		};
		ble_gap_update_params(active_conn, &up);
		return 0;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
		active_conn = BLE_HS_CONN_HANDLE_NONE;
		tx_subscribed = false;
		ble_state = BLE_STATE_ON;
		ble_advertise();
		return 0;

	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle == tx_handle) {
			tx_subscribed = event->subscribe.cur_notify;
			ESP_LOGI(TAG, "tx notify %s",
			         tx_subscribed ? "subscribed" : "unsubscribed");
		}
		return 0;

	case BLE_GAP_EVENT_ENC_CHANGE:
		if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0)
			return 0;
		if (desc.sec_state.encrypted && desc.sec_state.authenticated) {
			ble_state = BLE_STATE_CONNECTED;
			allow_pairing = false;
			ESP_LOGI(TAG, "encrypted + authenticated");
		}
		return 0;

	case BLE_GAP_EVENT_PASSKEY_ACTION:
		if (!allow_pairing) {
			ESP_LOGW(TAG, "pairing rejected (not in pairing mode)");
			ble_gap_terminate(event->passkey.conn_handle,
			                  BLE_ERR_NO_PAIRING);
			return 0;
		}
		if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
			struct ble_sm_io pk = { 0 };
			pk.action = BLE_SM_IOACT_DISP;
			pk.passkey = BLE_PASSKEY;
			ESP_LOGI(TAG, "passkey: %06lu", (unsigned long)BLE_PASSKEY);
			ble_sm_inject_io(event->passkey.conn_handle, &pk);
		}
		return 0;

	case BLE_GAP_EVENT_REPEAT_PAIRING: {
		int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
		if (rc == 0)
			ble_store_util_delete_peer(&desc.peer_id_addr);
		return BLE_GAP_REPEAT_PAIRING_RETRY;
	}

	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "mtu: %u", event->mtu.value);
		return 0;

	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* NimBLE host callbacks                                              */
/* ------------------------------------------------------------------ */

static void on_sync( void ) {
	ESP_LOGI(TAG, "host synced");

	int rc = ble_hs_util_ensure_addr(0);
	if (rc != 0) {
		ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
		return;
	}

	rc = ble_hs_id_infer_auto(0, &own_addr_type);
	if (rc != 0) {
		ESP_LOGE(TAG, "id_infer_auto failed: %d", rc);
		return;
	}

	uint8_t addr[6] = {0};
	ble_hs_id_copy_addr(own_addr_type, addr, NULL);
	ESP_LOGI(TAG, "addr type=%d: %02x:%02x:%02x:%02x:%02x:%02x",
	         own_addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

	/* Append the device-specific MAC suffix (last 3 bytes) to the name,
	 * e.g. "RNode HaLow DDEEFF", so multiple units are distinguishable. */
	snprintf(s_device_name, sizeof(s_device_name), "%s %02X%02X%02X",
	         BLE_DEVICE_NAME, addr[2], addr[1], addr[0]);
	ble_svc_gap_device_name_set(s_device_name);
	ESP_LOGI(TAG, "device name: %s", s_device_name);

	if (ble_state == BLE_STATE_OFF)
		ble_state = BLE_STATE_ON;
	ble_advertise();
}

static void on_reset( int reason ) {
	ESP_LOGW(TAG, "nimble reset: %d", reason);
}

static void host_task( void *param ) {
	(void)param;
	nimble_port_run();
	nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* TX flush task                                                      */
/* ------------------------------------------------------------------ */

static void flush_task( void *arg ) {
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(3000));
	for (;;) {
		ble_flush();
		vTaskDelay(pdMS_TO_TICKS(STATUS_TASK_MS));
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void ble_init( void ) {
	rx_stream = xStreamBufferCreate(RX_STREAM_SIZE, 1);
	tx_mutex = xSemaphoreCreateMutex();

	int rc = nimble_port_init();
	if (rc != ESP_OK) {
		ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
		return;
	}
	ESP_LOGI(TAG, "nimble_port_init ok");

	ble_hs_cfg.reset_cb          = on_reset;
	ble_hs_cfg.sync_cb           = on_sync;
	ble_hs_cfg.gatts_register_cb = NULL;
	ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

	ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_DISP_ONLY;
	ble_hs_cfg.sm_bonding        = 1;
	ble_hs_cfg.sm_mitm           = 1;
	ble_hs_cfg.sm_sc             = 1;
	ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC |
	                               BLE_SM_PAIR_KEY_DIST_ID;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
	                               BLE_SM_PAIR_KEY_DIST_ID;

	rc = gatt_svr_init();
	if (rc != 0) {
		ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
		return;
	}
	ESP_LOGI(TAG, "gatt_svr_init ok");

	ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
	ble_store_config_init();

	nimble_port_freertos_init(host_task);

	xTaskCreate(flush_task, "ble_flush", 4096, NULL, 5, NULL);

	ESP_LOGI(TAG, "BLE initialized (%s)", BLE_DEVICE_NAME);
}

void ble_enable_pairing( void ) {
	allow_pairing = true;
	if (ble_state == BLE_STATE_OFF || ble_state == BLE_STATE_ON)
		ble_state = BLE_STATE_PAIRING;
	ESP_LOGI(TAG, "pairing enabled (passkey: %06lu)",
	         (unsigned long)BLE_PASSKEY);
}

void ble_disable_pairing( void ) {
	allow_pairing = false;
	if (ble_state == BLE_STATE_PAIRING)
		ble_state = BLE_STATE_ON;
	ESP_LOGI(TAG, "pairing disabled");
}

ble_state_t ble_get_state( void ) {
	return ble_state;
}

uint32_t ble_get_passkey( void ) {
	return BLE_PASSKEY;
}

bool ble_connected( void ) {
	return ble_state == BLE_STATE_CONNECTED;
}

int ble_available( void ) {
	return (int)xStreamBufferBytesAvailable(rx_stream);
}

int ble_read( void ) {
	uint8_t b;
	if (xStreamBufferReceive(rx_stream, &b, 1, portMAX_DELAY) == 0)
		return -1;
	return b;
}

size_t ble_read_bytes( uint8_t *buf, size_t len ) {
	return xStreamBufferReceive(rx_stream, buf, len, 0);
}

size_t ble_write( const uint8_t *buf, size_t len ) {
	if (!tx_subscribed || active_conn == BLE_HS_CONN_HANDLE_NONE)
		return 0;

	xSemaphoreTake(tx_mutex, portMAX_DELAY);
	for (size_t i = 0; i < len; i++) {
		tx_buf[tx_buf_len++] = buf[i];
		if (tx_buf_len >= TX_BUFFER_SIZE)
			break;
	}
	xSemaphoreGive(tx_mutex);
	return len;
}

void ble_flush( void ) {
	xSemaphoreTake(tx_mutex, portMAX_DELAY);

	if (!tx_subscribed || active_conn == BLE_HS_CONN_HANDLE_NONE) {
		tx_buf_len = 0;
		xSemaphoreGive(tx_mutex);
		return;
	}

	uint16_t mtu = ble_att_mtu(active_conn);
	if (mtu < 23) mtu = 23;
	size_t max_chunk = mtu - 3;
	if (max_chunk > BLE_NOTIFY_MAX) max_chunk = BLE_NOTIFY_MAX;

	while (tx_buf_len > 0) {
		size_t chunk = (tx_buf_len > max_chunk) ? max_chunk : tx_buf_len;
		struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, chunk);
		if (om == NULL)
			break;
		int rc = ble_gatts_notify_custom(active_conn, tx_handle, om);
		if (rc != 0)
			break;
		tx_buf_len -= chunk;
		if (tx_buf_len > 0)
			memmove(tx_buf, tx_buf + chunk, tx_buf_len);
	}

	xSemaphoreGive(tx_mutex);
}
