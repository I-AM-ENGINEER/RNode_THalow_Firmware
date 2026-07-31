#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
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
#include "thalow_config.h"
#include "config/project_config.h"

static const char *TAG = "ble";

#define RX_STREAM_SIZE   (2048)
/* TX buffer: must hold a full worst-case KISS frame. KISS escaping can
 * double every payload byte (FEND/FESC -> 2 bytes), so a 1024-byte RNS
 * payload becomes up to 2*1024 + cmd + 2*FEND ~= 2052 bytes. */
#define TX_BUFFER_SIZE   (2064)
/* Max payload of a single GATT Write Request. Limited by the negotiated
 * MTU (att_mtu - 3 header); 512 is the NimBLE max. Decoupled from
 * TX_BUFFER_SIZE so the GATT access callback doesn't burn 2 KB of
 * NimBLE host-task stack. */
#define BLE_GATT_WRITE_MAX (512)
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
static char s_device_name[40];
static uint16_t active_conn = BLE_HS_CONN_HANDLE_NONE;
static bool tx_subscribed = false;
static uint16_t rx_handle;
static uint16_t tx_handle;

static ble_state_t ble_state = BLE_STATE_OFF;
static bool allow_pairing = false;

/* Two-phase advertising. After boot / disconnect / pairing-enabled, we do a
 * FAST burst (<=40 ms interval) so Android/iOS system Bluetooth scanners can
 * discover the device within ~1 second. After BLE_ADV_FAST_MS we drop to SLOW
 * (~1.3-1.6 s interval) to save power. See BLE Core Spec Vol 3, Part C, 9.3
 * "Connection Establishment" -- fast interval is explicitly recommended for
 * the discovery phase. */
static bool s_adv_fast = true;
/* True while BLE is paused for a WiFi scan (advertising stopped). */
static bool s_ble_paused = false;

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
		if (len > BLE_GATT_WRITE_MAX)
			len = BLE_GATT_WRITE_MAX;

		uint8_t tmp[BLE_GATT_WRITE_MAX];
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

	/* Two-phase advertising. Fast burst right after boot/disconnect/pairing
	 * so phones in Bluetooth settings see the device quickly; drops to a
	 * slow interval for power saving afterwards. */
	int32_t duration_ms;
	if (s_adv_fast) {
		adv_params.itvl_min = BLE_ADV_FAST_MIN;
		adv_params.itvl_max = BLE_ADV_FAST_MAX;
		duration_ms = BLE_ADV_FAST_MS;
	} else {
		adv_params.itvl_min = BLE_ADV_SLOW_MIN;
		adv_params.itvl_max = BLE_ADV_SLOW_MAX;
		duration_ms = BLE_HS_FOREVER;
	}

	rc = ble_gap_adv_start(own_addr_type, NULL, duration_ms,
	                       &adv_params, gap_event_cb, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv_start failed: %d", rc);
		return;
	}

	ESP_LOGI(TAG, "advertising started (%s, %lu ms)",
	         s_adv_fast ? "fast" : "slow", (unsigned long)duration_ms);
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
			ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
			s_adv_fast = true; /* give the next scan a fresh fast window */
			ble_advertise();
			return 0;
		}
		active_conn = event->connect.conn_handle;
		ble_state = BLE_STATE_ON;
		ESP_LOGI(TAG, "connected (handle %u, status=%d)",
		         active_conn, event->connect.status);

		/* PHY update + param update are deferred to BLE_GAP_EVENT_ENC_CHANGE.
		 * Scheduling them here, before SMP pairing completes, races the
		 * controller's LL procedures against the pairing procedure and on
		 * some centrals (notably Android) causes a proc-collision -> link
		 * drop shortly after "encrypted + authenticated". Doing them after
		 * encryption is the recommended NimBLE pattern. */
		return 0;

	case BLE_GAP_EVENT_DISCONNECT:
		/* event->disconnect.reason is the HCI error code from the controller.
		 * Common ones:
		 *   8 (CONN_TIMEOUT) -- supervision timeout (link faded out)
		 *  13 (REMOTE_USER_TERM) -- peer closed cleanly
		 *  19 (REMOTE_DEV_LOW_RESOURCES) / 20 (POWER_OFF) -- peer died
		 *  22 (LOCAL_HOST_TERM) -- we terminated
		 *  62 (CONN_FAIL_ESTABLISH) -- connection never really came up
		 */
		ESP_LOGW(TAG, "disconnected (handle %u, reason=%d) conn_handle=%u",
		         event->disconnect.conn.conn_handle,
		         event->disconnect.reason,
		         event->disconnect.conn.conn_handle);
		active_conn = BLE_HS_CONN_HANDLE_NONE;
		tx_subscribed = false;
		ble_state = BLE_STATE_ON;
		s_adv_fast = true; /* make us discoverable again right after a drop */
		if (!s_ble_paused)
			ble_advertise();
		else
			ESP_LOGI(TAG, "disconnect during pause, advertising deferred to resume");
		return 0;

	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle == tx_handle) {
			tx_subscribed = event->subscribe.cur_notify;
			ESP_LOGI(TAG, "tx CCCD: notify %d->%d, indicate %d->%d (handle %u)",
			         event->subscribe.prev_notify, event->subscribe.cur_notify,
			         event->subscribe.prev_indicate, event->subscribe.cur_indicate,
			         event->subscribe.conn_handle);
		}
		return 0;

	case BLE_GAP_EVENT_ENC_CHANGE:
		if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
			ESP_LOGW(TAG, "enc_change: conn_find failed");
			return 0;
		}
		ESP_LOGI(TAG, "enc_change: enc=%d auth=%d bond=%d key_sz=%u (status=%d)",
		         desc.sec_state.encrypted, desc.sec_state.authenticated,
		         desc.sec_state.bonded, desc.sec_state.key_size,
		         event->enc_change.status);
		if (desc.sec_state.encrypted && desc.sec_state.authenticated) {
			ble_state = BLE_STATE_CONNECTED;
			allow_pairing = false;

			/* Now that encryption is established, it is safe to nudge the
			 * controller: prefer 2M PHY for throughput and tighten the
			 * connection interval. Doing this here (rather than at CONNECT)
			 * avoids racing SMP -- the historical cause of post-pairing
			 * disconnects on Android. */
			int rc_phy = ble_gap_set_prefered_le_phy(event->enc_change.conn_handle,
				BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
				BLE_GAP_LE_PHY_CODED_ANY);
			ESP_LOGI(TAG, "phy update req: rc=%d", rc_phy);

			struct ble_gap_upd_params up = {
				/* 40..80 ms interval: responsive enough for a Reticulum/BLE
				 * bridge, much lower duty cycle than the 7.5..15 ms default.
				 * supervision 4 s (320*10ms) -- tolerate brief RF gaps. */
				.itvl_min = 32,
				.itvl_max = 64,
				.latency = 0,
				.supervision_timeout = 320,
			};
			int rc_upd = ble_gap_update_params(event->enc_change.conn_handle, &up);
			ESP_LOGI(TAG, "param update req: rc=%d", rc_upd);
		} else {
			ESP_LOGW(TAG, "enc_change: encryption incomplete (enc=%d auth=%d)",
			         desc.sec_state.encrypted, desc.sec_state.authenticated);
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

	case BLE_GAP_EVENT_ADV_COMPLETE:
		/* Don't auto-restart while paused: ble_pause() explicitly stopped
		 * advertising and the ADV_COMPLETE it generated must not undo that.
		 * ble_resume() restarts advertising when the scan finishes. */
		if (s_ble_paused)
			return 0;
		/* Fast burst window expired (duration ran out). Drop to slow
		 * advertising for power saving and restart. If we were already
		 * in slow mode this means advertising was stopped externally --
		 * just stay slow and restart. */
		if (s_adv_fast) {
			s_adv_fast = false;
			ESP_LOGI(TAG, "fast window expired, switching to slow");
		}
		ble_advertise();
		return 0;

	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "mtu: %u (conn %u)", event->mtu.value,
		         event->mtu.conn_handle);
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

	/* Runtime BLE name from thalow_config (already includes MAC suffix). */
	const char *name = thalow_config_get_ble_name();
	strncpy(s_device_name, name, sizeof(s_device_name) - 1);
	s_device_name[sizeof(s_device_name) - 1] = '\0';
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
	if (!thalow_config_get_ble_enabled()) {
		ESP_LOGW(TAG, "BLE disabled by config, skipping init");
		return;
	}

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
	/* Distribute only the LTK (encryption key). We do NOT distribute the ID
	 * key (IRK) because this device uses a static address (no RPA privacy),
	 * so the controller has no resolving list to populate. Sending IRK
	 * anyway caused "hci_err=0x212 LE Add Device To Resolving List" spam
	 * on every bond. LTK is enough for reconnection without re-pairing. */
	ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

	rc = gatt_svr_init();
	if (rc != 0) {
		ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
		return;
	}
	ESP_LOGI(TAG, "gatt_svr_init ok");

	ble_svc_gap_device_name_set(thalow_config_get_ble_name());
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

	/* Re-arm the fast discovery window so the user (who just pressed the
	 * pairing button) can actually find the device in Bluetooth settings
	 * within ~1 s, regardless of how long ago the last fast burst expired.
	 * Restarting advertising while it is already running is allowed; the
	 * in-flight instance is replaced. */
	s_adv_fast = true;
	if (ble_hs_synced()) {
		ble_gap_adv_stop();
		ble_advertise();
	}
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

void ble_pause(void) {
	if (!thalow_config_get_ble_enabled())
		return;
	if (s_ble_paused)
		return;
	s_ble_paused = true;
	/* Stop advertising but keep the BT controller + NimBLE host task alive.
	 * Calling esp_bt_controller_disable() while the host task is running races
	 * HCI traffic into a torn-down VHCI transport -> xQueueGenericSend(NULL)
	 * assert -> reboot (observed during WiFi scan). Stopping advertising frees
	 * the airtime the scan needs; the IDF coexistence arbiter handles the rest.
	 * An existing GATT connection stays up -- coex juggles it. */
	if (ble_hs_synced()) {
		ble_gap_adv_stop();
	}
	ESP_LOGI(TAG, "BLE paused for WiFi scan (advertising stopped)");
}

void ble_resume(void) {
	if (!s_ble_paused)
		return;
	s_ble_paused = false;
	/* Restart advertising only if not currently connected. While connected the
	 * peripheral can't advertise; the DISCONNECT handler restarts it when the
	 * link drops. Re-arm fast mode so we're rediscoverable quickly post-scan. */
	if (active_conn == BLE_HS_CONN_HANDLE_NONE && ble_hs_synced()) {
		s_adv_fast = true;
		ble_advertise();
	}
	ESP_LOGI(TAG, "BLE resumed");
}

void ble_flush( void ) {
	/* When BLE is disabled in config, ble_init() never runs and tx_mutex is
	 * never created (it stays NULL). xSemaphoreTake(NULL, ...) asserts and
	 * panics. The on_rns_frame -> kiss_send_data -> on_kiss_tx -> ble_flush
	 * path is still wired even with BLE off (so an incoming RF packet still
	 * reaches this code), so we must bail out before touching the mutex.
	 * The !tx_subscribed early-return below would also fire, but only AFTER
	 * the take -- which is exactly where the crash happens. */
	if (!thalow_config_get_ble_enabled())
		return;

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
