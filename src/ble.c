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

/* IDF 5.4.4 does not yet contain the upstream "remove before add" fix in
 * ble_hs_pvcy.c. Re-pairing a peer whose identity is already in the
 * controller resolving list fails with HCI error 0x12/0x212 on
 * LE Add Device To Resolving List, and that error propagates into SMP and
 * kills the pairing. We clear the stale entry ourselves in the
 * REPEAT_PAIRING handler. Internal NimBLE helper, stable signature. */
extern int ble_hs_pvcy_remove_entry( uint8_t addr_type, const uint8_t *addr );

#include "ble.h"
#include "thalow_config.h"
#include "config/project_config.h"

static const char *TAG = "ble";

/* ------------------------------------------------------------------ */
/* Design notes (full analysis: docs/ble-analysis.md)                  */
/*                                                                     */
/* Security model: Just Works + bonding + Secure Connections. The old  */
/* static-passkey MITM posture protected nothing (public constant      */
/* passkey == Just Works) while breaking headless clients: python RNS   */
/* has no pairing agent, and our terminate-during-pairing made Android */
/* delete its bond ("device disappears from saved devices"). Data is   */
/* still encrypted-only: RX writes return INSUFFICIENT_AUTHEN until    */
/* the link is encrypted, notifications are withheld until encrypted.  */
/*                                                                     */
/* Bonding: keys distributed are ENC|ID in BOTH directions. With SC the */
/* ENC bit is stripped by NimBLE anyway; what matters is ID (IRK +      */
/* identity address) so the bond is stored under the peer's IDENTITY   */
/* address and the controller resolving list can resolve the central's */
/* rotating RPA after a reboot. This is the root-cause fix for         */
/* "pairing does not survive reboot".                                  */
/* ------------------------------------------------------------------ */

#define RX_STREAM_SIZE   (2048)
/* TX buffer: must hold a full worst-case KISS frame. KISS escaping can
 * double every payload byte (FEND/FESC -> 2 bytes), so a 1024-byte RNS
 * payload becomes up to 2*1024 + cmd + 2*FEND ~= 2052 bytes. Write policy
 * is frame-atomic: a frame is either fully buffered or fully dropped. */
#define TX_BUFFER_SIZE   (2064)
/* Max payload of a single GATT Write Request, bounded by the negotiated
 * ATT MTU (mtu-3). Static scratch instead of a stack buffer so the NimBLE
 * host task (which runs this callback) does not burn 512 bytes of stack. */
#define BLE_GATT_WRITE_MAX (512)
/* Cap for a single notification chunk: mtu-3 clamped to 509. */
#define BLE_NOTIFY_MAX   (509)
#define FLUSH_PERIOD_MS  (10)
#define KISS_FEND        (0xC0)

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
/* CCCD subscribed (client wants notifications). Data only flows when the
 * link is also encrypted. */
static volatile bool tx_subscribed = false;
static volatile bool conn_encrypted = false;
static uint16_t rx_handle;
static uint16_t tx_handle;

static ble_state_t ble_state = BLE_STATE_OFF;
static bool allow_pairing = false;

/* Pairing gate: outside the pairing window, advertising is filtered by the
 * controller whitelist (rebuilt from the NVS bond store). Only bonded peers
 * may connect at all, so no new pairing can happen unless the BOOT button
 * opened the window. A side effect of the filter policy: non-bonded
 * scanners do not get the scan response either, so the device shows up
 * unnamed for strangers (columba's wizard lists named RNodes only). */
static bool s_phy_requested = false; /* one 2M PHY request per connection */

/* Two-phase advertising. After boot / disconnect / pairing-enabled, we do a
 * FAST burst (<=40 ms interval) so Android/iOS system Bluetooth scanners can
 * discover the device within ~1 second (BLE Core Spec Vol 3, Part C, 9.3).
 * After BLE_ADV_FAST_MS we drop to SLOW (~160-250 ms): still comfortably
 * discoverable inside bleak's 2 s scan window and fast for Android direct
 * connects, but ~4x lower RF duty cycle than the fast burst. */
static bool s_adv_fast = true;
/* Pause depth for WiFi scans (>0 = advertising stopped). Depth-counted so
 * nested/concurrent scans cannot accidentally resume early. */
static uint8_t s_ble_pause_depth = 0;

static StreamBufferHandle_t rx_stream;
static SemaphoreHandle_t tx_mutex;
static uint8_t tx_buf[TX_BUFFER_SIZE];
static size_t tx_buf_len = 0;

/* Scratch for flattening a GATT write mbuf; only touched from the NimBLE
 * host task (access callback), so no extra locking needed. */
static uint8_t wr_scratch[BLE_GATT_WRITE_MAX];

/* RX overflow recovery: once the stream is full we drop bytes until the
 * next KISS FEND so the parser resynchronizes on a frame boundary instead
 * of consuming a corrupted mid-frame tail. */
static bool s_rx_resync = false;

/* Light-weight stats, logged occasionally so silent data loss is visible. */
static uint32_t s_tx_frames_dropped = 0;
static uint32_t s_tx_drop_nosub = 0;
static uint32_t s_rx_drop_bytes = 0;
static uint32_t s_notify_fail = 0;
static uint32_t s_auth_reject = 0;

static void ble_advertise( void );

/* Rebuild the controller whitelist from the persisted bond identities.
 * Called right before every advertising (re)start while the pairing gate
 * is active. MAX_BONDS is 3; 8 slots is generous headroom. */
static void ble_whitelist_refresh( void ) {
	ble_addr_t ids[8];
	int count = 0;

	int rc = ble_store_util_bonded_peers(ids, &count,
	                                     (int)(sizeof(ids) / sizeof(ids[0])));
	if (rc != 0) {
		ESP_LOGW(TAG, "bonded_peers read failed: %d", rc);
		return;
	}

	rc = ble_gap_wl_set(ids, (uint8_t)count);
	if (rc != 0) {
		ESP_LOGW(TAG, "whitelist set failed: rc=%d (count=%d)", rc, count);
		return;
	}

	ESP_LOGI(TAG, "pairing gate: whitelist = %d bonded peer(s)", count);
}

/* ------------------------------------------------------------------ */
/* GATT                                                               */
/* ------------------------------------------------------------------ */

/* Push transport bytes toward the KISS parser, dropping whole frame tails
 * (never a silent mid-frame splice) when the stream is full. */
static void rx_stream_send( const uint8_t *data, size_t len ) {
	if (s_rx_resync) {
		const uint8_t *fend = memchr(data, KISS_FEND, len);
		if (fend == NULL) {
			s_rx_drop_bytes += len;
			return;
		}
		s_rx_drop_bytes += (size_t)(fend - data) + 1;
		len   -= (size_t)(fend - data) + 1;
		data  += (size_t)(fend - data) + 1;
		s_rx_resync = false;
		if (len == 0)
			return;
	}

	size_t space = xStreamBufferSpacesAvailable(rx_stream);
	if (len > space) {
		s_rx_resync = true;
		s_rx_drop_bytes += len - space;
		len = space;
		ESP_LOGW(TAG, "rx stream full, dropping frame tail (+resync), total dropped %lu bytes",
		         (unsigned long)s_rx_drop_bytes);
	}
	if (len > 0)
		xStreamBufferSend(rx_stream, data, len, 0);
}

static int gatt_access_cb( uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg ) {
	(void)attr;
	(void)arg;

	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
		return 0;

	/* Data plane is encrypted-only. Returning INSUFFICIENT_AUTHEN makes a
	 * bonded central start encryption (or an unbonded one start pairing)
	 * automatically -- the standard ATT security-elevation path. */
	struct ble_gap_conn_desc desc;
	if (ble_gap_conn_find(conn, &desc) != 0 || !desc.sec_state.encrypted) {
		s_auth_reject++;
		ESP_LOGW(TAG, "write rejected on unencrypted link (count %lu)",
		         (unsigned long)s_auth_reject);
		return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
	}

	uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
	if (len > BLE_GATT_WRITE_MAX)
		len = BLE_GATT_WRITE_MAX;

	ble_hs_mbuf_to_flat(ctxt->om, wr_scratch, sizeof(wr_scratch), &len);
	ESP_LOGD(TAG, "rx write %u bytes", len);
	rx_stream_send(wr_scratch, len);
	return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &nus_svc_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]) {
			{
				/* Both write modes: columba writes WITH response,
				 * python RNS/bleak writes WITHOUT response. */
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

	/* Advertising data: flags + 128-bit NUS service UUID. The UUID MUST be
	 * here (not just in the GATT table): columba's ScanFilter and bleak's
	 * discovery both match on advertised service UUIDs. */
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

	/* Scan response: device name. Both clients require the advertised name
	 * to start with "RNode " (the configured default name does). */
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

	if (allow_pairing) {
		/* Pairing window open: anyone may discover, scan and connect. */
		adv_params.filter_policy = BLE_HCI_ADV_FILT_NONE;
	} else {
		/* Gate closed: only bonded peers may connect (and only they get
		 * the scan response with the name). The whitelist is refreshed
		 * from the bond store right before the advertising start. */
		ble_whitelist_refresh();
		adv_params.filter_policy = BLE_HCI_ADV_FILT_SCAN;
	}

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
		/* Most common cause: advertising already active. Not fatal. */
		ESP_LOGW(TAG, "adv_start rc=%d (already active?)", rc);
		return;
	}

	ESP_LOGI(TAG, "advertising started (%s, %lu ms)",
	         s_adv_fast ? "fast" : "slow", (unsigned long)duration_ms);
}

/* ------------------------------------------------------------------ */
/* GAP events                                                         */
/* ------------------------------------------------------------------ */

static const char *hci_reason_str( int reason ) {
	switch (reason) {
	case 0x08: return "conn timeout (supervision)";
	case 0x0d: return "peer closed cleanly";
	case 0x13: return "peer low resources";
	case 0x14: return "peer power off";
	case 0x16: return "local host terminated";
	case 0x3e: return "conn fail to establish";
	default:   return "other";
	}
}

static int gap_event_cb( struct ble_gap_event *event, void *arg ) {
	(void)arg;
	struct ble_gap_conn_desc desc;

	switch (event->type) {

	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status != 0) {
			ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
			s_adv_fast = true;
			ble_advertise();
			return 0;
		}
		active_conn = event->connect.conn_handle;
		conn_encrypted = false;
		tx_subscribed = false;
		s_phy_requested = false;
		ble_state = BLE_STATE_ON;
		ESP_LOGI(TAG, "connected (handle %u)", active_conn);
		/* No LL procedures here. Encryption/param/PHY sequencing happens
		 * on ENC_CHANGE / CONN_UPDATE -- starting procedures before SMP
		 * finishes is the historical cause of post-pairing disconnects
		 * on Android (procedure collision). */
		return 0;

	case BLE_GAP_EVENT_DISCONNECT: {
		int reason = event->disconnect.reason;
		ESP_LOGW(TAG, "disconnected (handle %u, reason=%d/%s)",
		         event->disconnect.conn.conn_handle, reason,
		         hci_reason_str(reason));
		active_conn = BLE_HS_CONN_HANDLE_NONE;
		conn_encrypted = false;
		tx_subscribed = false;
		s_phy_requested = false;
		/* If the pairing window is still open, keep advertising in
		 * pairing mode so the user can retry within the window. */
		ble_state = allow_pairing ? BLE_STATE_PAIRING : BLE_STATE_ON;
		s_adv_fast = true; /* make us discoverable again right after a drop */
		if (s_ble_pause_depth == 0)
			ble_advertise();
		else
			ESP_LOGI(TAG, "disconnect during pause, advertising deferred to resume");
		return 0;
	}

	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle == tx_handle) {
			tx_subscribed = event->subscribe.cur_notify;
			ESP_LOGI(TAG, "tx CCCD: notify %d->%d (conn %u)",
			         event->subscribe.prev_notify,
			         event->subscribe.cur_notify,
			         event->subscribe.conn_handle);
		}
		return 0;

	case BLE_GAP_EVENT_ENC_CHANGE:
		if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
			ESP_LOGW(TAG, "enc_change: conn_find failed");
			return 0;
		}
		ESP_LOGI(TAG, "enc_change: enc=%d auth=%d bond=%d (status=%d)",
		         desc.sec_state.encrypted, desc.sec_state.authenticated,
		         desc.sec_state.bonded, event->enc_change.status);
		if (desc.sec_state.encrypted) {
			conn_encrypted = true;
			ble_state = BLE_STATE_CONNECTED;
			allow_pairing = false; /* pairing accomplished */

			/* First LL procedure after encryption: tighten the connection
			 * interval (40..80 ms, legal per supervision formula and Apple
			 * QA1931). The 2M PHY request is deferred until CONN_UPDATE
			 * completes so the two procedures never overlap. */
			struct ble_gap_upd_params up = {
				.itvl_min = 32,            /* 40 ms */
				.itvl_max = 64,            /* 80 ms */
				.latency = 0,
				.supervision_timeout = 300,/* 3 s  (> 2 * 80 ms: legal) */
			};
			int rc = ble_gap_update_params(event->enc_change.conn_handle, &up);
			ESP_LOGI(TAG, "param update req: rc=%d", rc);
		} else {
			/* Encryption restore failed. With correct ID-key distribution
			 * this should not happen; if the peer lost its bond it will
			 * re-pair (Just Works, accepted) and recover automatically. */
			ESP_LOGW(TAG, "enc_change: encryption NOT established");
		}
		return 0;

	case BLE_GAP_EVENT_CONN_UPDATE:
		if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) != 0)
			return 0;
		ESP_LOGI(TAG, "conn update: itvl=%u us latency=%u superv=%u ms (status=%d)",
		         desc.conn_itvl * 1250, desc.conn_latency,
		         desc.supervision_timeout * 10, event->conn_update.status);
		/* Param update finished -> now (and only now) request the 2M PHY.
		 * One LL procedure at a time, and only once per connection (some
		 * centrals issue several conn updates in a row). Failures are
		 * harmless (1M PHY works fine) and are only logged. */
		if (!s_phy_requested && conn_encrypted &&
		    active_conn != BLE_HS_CONN_HANDLE_NONE) {
			s_phy_requested = true;
			int rc = ble_gap_set_prefered_le_phy(active_conn,
				BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
				BLE_GAP_LE_PHY_CODED_ANY);
			ESP_LOGI(TAG, "phy 2M req: rc=%d", rc);
		}
		return 0;

	case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
		ESP_LOGI(TAG, "phy update: tx=%d rx=%d (status=%d)",
		         event->phy_updated.tx_phy, event->phy_updated.rx_phy,
		         event->phy_updated.status);
		return 0;

	case BLE_GAP_EVENT_PASSKEY_ACTION:
		/* Should never fire: IO cap is NoInputNoOutput (Just Works). Kept
		 * as a defensive log; do NOT terminate the link here -- a
		 * terminate during SMP makes Android delete its bond. */
		ESP_LOGW(TAG, "unexpected passkey action %d (just-works device)",
		         event->passkey.params.action);
		return 0;

	case BLE_GAP_EVENT_REPEAT_PAIRING: {
		/* Peer is bonded on our side but initiated pairing again (e.g. it
		 * lost or rotated its keys). Standard NimBLE recovery: drop our
		 * stale bond and let the new pairing proceed. The resolving-list
		 * entry must be cleared first -- see the note on
		 * ble_hs_pvcy_remove_entry above (IDF 5.4.4 lacks the upstream
		 * remove-before-add fix; re-adding an existing identity fails
		 * with HCI 0x212 and kills the pairing). */
		int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
		if (rc == 0) {
			ble_hs_pvcy_remove_entry(desc.peer_id_addr.type,
			                         desc.peer_id_addr.val);
			ble_store_util_delete_peer(&desc.peer_id_addr);
			ESP_LOGI(TAG, "repeat pairing: stale bond cleared, retrying");
		}
		return BLE_GAP_REPEAT_PAIRING_RETRY;
	}

	case BLE_GAP_EVENT_ADV_COMPLETE:
		/* Don't auto-restart while paused: ble_pause() explicitly stopped
		 * advertising and the ADV_COMPLETE it generated must not undo
		 * that; ble_resume() restarts when the scan finishes. */
		if (s_ble_pause_depth > 0)
			return 0;
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

	int bonds = 0;
	ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
	ESP_LOGI(TAG, "bonded peers restored from NVS: %d", bonds);

	if (ble_state == BLE_STATE_OFF)
		ble_state = BLE_STATE_ON;
	ble_advertise();
}

static void on_reset( int reason ) {
	ESP_LOGW(TAG, "nimble host reset: %d -> resynchronising", reason);
	/* The host will re-sync (on_sync) and re-advertise; make sure no stale
	 * connection state survives the reset. */
	active_conn = BLE_HS_CONN_HANDLE_NONE;
	conn_encrypted = false;
	tx_subscribed = false;
	s_phy_requested = false;
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
		vTaskDelay(pdMS_TO_TICKS(FLUSH_PERIOD_MS));
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
	ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

	/* --- Security: Just Works + Bonding + Secure Connections ----------
	 * NoInputNoOutput with sm_mitm=0 => Just Works association model:
	 * pairs silently on every platform (columba auto-confirms consent
	 * pairing; `bluetoothctl pair` needs no agent for python RNS). The
	 * link is still encrypted and the data plane rejects unencrypted
	 * access (see gatt_access_cb / ble_flush). A static public passkey
	 * provided no real MITM protection, so nothing is lost -- see
	 * docs/ble-analysis.md section 4. */
	ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
	ble_hs_cfg.sm_bonding        = 1;
	ble_hs_cfg.sm_mitm           = 0;
	ble_hs_cfg.sm_sc             = 1;
	/* ID keys (IRK + identity address) MUST be exchanged both ways, or the
	 * bond is keyed on the peer's RPA and dies at the first RPA rotation /
	 * reboot -- the root cause of "pairing does not survive reboot" (SC
	 * strips the ENC bit, so an ENC-only mask distributes nothing at all). */
	ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

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
	ESP_LOGI(TAG, "pairing window open (just-works, advertising boosted)");

	/* Re-arm the fast discovery window so the user (who just pressed the
	 * pairing button) can find the device within ~1 s regardless of how
	 * long ago the last fast burst expired. Restarting advertising while
	 * it is already running is allowed; the in-flight instance is
	 * replaced. */
	s_adv_fast = true;
	if (ble_hs_synced()) {
		ble_gap_adv_stop();
		ble_advertise();
	}
}

void ble_disable_pairing( void ) {
	bool was_open = allow_pairing;
	allow_pairing = false;
	if (ble_state == BLE_STATE_PAIRING)
		ble_state = BLE_STATE_ON;
	ESP_LOGI(TAG, "pairing window closed");
	/* Switch advertising back to the bonded-only gate. Restarting while
	 * connected just fails harmlessly (can't advertise while connected);
	 * the DISCONNECT handler will restart in gated mode. */
	if (was_open && ble_hs_synced() && active_conn == BLE_HS_CONN_HANDLE_NONE) {
		ble_gap_adv_stop();
		ble_advertise();
	}
}

ble_state_t ble_get_state( void ) {
	return ble_state;
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
	/* Frame-atomic: either the whole frame fits or the whole frame is
	 * dropped. Callers hand us complete KISS frames; a silent mid-frame
	 * splice would corrupt the client's KISS parser (KISS has no CRC). */
	if (tx_mutex == NULL)
		return 0;

	if (!tx_subscribed || !conn_encrypted ||
	    active_conn == BLE_HS_CONN_HANDLE_NONE) {
		s_tx_drop_nosub += len;
		return 0;
	}

	xSemaphoreTake(tx_mutex, portMAX_DELAY);
	if (len > TX_BUFFER_SIZE - tx_buf_len) {
		s_tx_frames_dropped++;
		ESP_LOGW(TAG, "tx buffer full: frame dropped (%u/%u used, total dropped %lu)",
		         (unsigned)tx_buf_len, TX_BUFFER_SIZE,
		         (unsigned long)s_tx_frames_dropped);
		xSemaphoreGive(tx_mutex);
		return 0;
	}
	memcpy(tx_buf + tx_buf_len, buf, len);
	tx_buf_len += len;
	xSemaphoreGive(tx_mutex);
	return len;
}

void ble_pause(void) {
	if (!thalow_config_get_ble_enabled())
		return;
	s_ble_pause_depth++;
	if (s_ble_pause_depth > 1)
		return;
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
	if (s_ble_pause_depth == 0)
		return;
	s_ble_pause_depth--;
	if (s_ble_pause_depth > 0)
		return;
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
	 * path is still wired even with BLE off, so we must bail out before
	 * touching the mutex. */
	if (!thalow_config_get_ble_enabled())
		return;
	if (tx_mutex == NULL)
		return;

	xSemaphoreTake(tx_mutex, portMAX_DELAY);

	if (!tx_subscribed || !conn_encrypted ||
	    active_conn == BLE_HS_CONN_HANDLE_NONE) {
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
			break; /* mbuf pool exhausted; retry next flush tick */
		int rc = ble_gatts_notify_custom(active_conn, tx_handle, om);
		if (rc != 0) {
			if (++s_notify_fail % 100 == 1)
				ESP_LOGW(TAG, "notify rc=%d (queue busy?), failures %lu",
				         rc, (unsigned long)s_notify_fail);
			break;
		}
		tx_buf_len -= chunk;
		if (tx_buf_len > 0)
			memmove(tx_buf, tx_buf + chunk, tx_buf_len);
	}

	xSemaphoreGive(tx_mutex);
}
