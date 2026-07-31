/*
 * thalow_inject.js
 *
 * Injects the "thalow" tab into the RNode HaLow web configurator so it looks
 * and behaves like the native radio configurator tabs. The markup uses only
 * real radio CSS classes (.panel, .toggle-label, .status-message, .note,
 * .stats-table, .tab-content, .panel-actions) and the same JS patterns as
 * main.js (setStatus, postJson, dirty-tracking, native checkbox toggles).
 *
 * API contract (implemented in web_proxy.c):
 *   GET  /api/thalow_cfg  -> full settings JSON (no passwords, only *_set)
 *   POST /api/thalow_cfg  -> JSON subset, passwords are presence-based
 *   GET  /api/thalow_scan -> 200 JSON array (sorted by RSSI desc) or 503
 */
(function () {
	'use strict';

	var TAB = 'thalow';
	var CFG_API = '/api/thalow_cfg';
	var SCAN_API = '/api/thalow_scan';
	var CONNECT_API = '/api/thalow_connect';
	var STATS_API = '/api/thalow_stats';

	var SECTION_FIELDS = {
		ap: ['thalow_ap_enable', 'thalow_ap_ssid', 'thalow_ap_authmode', 'thalow_ap_pw', 'thalow_ap_ip', 'thalow_ap_netmask'],
		sta: ['thalow_sta_enable', 'thalow_sta_ssid', 'thalow_sta_pw', 'thalow_sta_dhcp',
			'thalow_sta_ip', 'thalow_sta_netmask', 'thalow_sta_gw'],
		ble: ['thalow_ble_enable', 'thalow_ble_name'],
		led: ['thalow_led_mode', 'thalow_led_heartbeat'],
		tcp: ['thalow_tcp_enable', 'thalow_tcp_port', 'thalow_tcp_whitelist']
	};

	var SECTION_VALIDATE_FIELDS = {
		ap: ['thalow_ap_ssid', 'thalow_ap_authmode', 'thalow_ap_pw', 'thalow_ap_ip', 'thalow_ap_netmask'],
		sta: ['thalow_sta_ssid', 'thalow_sta_pw', 'thalow_sta_ip', 'thalow_sta_netmask', 'thalow_sta_gw'],
		ble: ['thalow_ble_name'],
		led: [],
		tcp: []
	};

	var SECTION_PW = { ap: 'thalow_ap_pw', sta: 'thalow_sta_pw' };

	var baseSnapshots = { ap: '', sta: '', ble: '', led: '', tcp: '' };
	var apPwSet = false;

	var esp32PollTimer = null;
	var esp32Polling = false;
	var esp32Loaded = false;

	function sectionOf(id) {
		for (var sec in SECTION_FIELDS) {
			if (SECTION_FIELDS[sec].indexOf(id) !== -1) return sec;
		}
		return null;
	}

	function $(id) {
		return document.getElementById(id);
	}

	function setStatus(id, text, isError) {
		var el = $(id);
		if (!el) return;
		el.textContent = text || '';
		el.classList.toggle('ok', !isError && !!text);
		el.classList.toggle('error', !!isError && !!text);
	}

	function postJson(url, payload) {
		return fetch(url, {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify(payload || {})
		});
	}

	function setInput(id, val) {
		var el = $(id);
		if (el) el.value = (val === undefined || val === null) ? '' : val;
	}

	function setCheckbox(id, checked) {
		var el = $(id);
		if (el) el.checked = !!checked;
	}

	function escapeHtml(s) {
		return String(s).replace(/[&<>"']/g, function (c) {
			return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
		});
	}

	function isValidIpv4(s) {
		if (!s || typeof s !== 'string') return false;
		var p = s.split('.');
		if (p.length !== 4) return false;
		for (var i = 0; i < 4; i++) {
			if (!/^\d{1,3}$/.test(p[i])) return false;
			var n = parseInt(p[i], 10);
			if (n < 0 || n > 255) return false;
		}
		return true;
	}

	function buildTabButton() {
		var btn = document.createElement('button');
		btn.type = 'button';
		btn.setAttribute('data-tab', TAB);
		btn.textContent = 'esp32';
		return btn;
	}

	function buildSection() {
		var sec = document.createElement('section');
		sec.id = TAB;
		sec.className = 'tab-content';
		sec.innerHTML = [
			'<div class="panel">',
			'  <h3>Wi-Fi Access Point</h3>',
			'  <label class="toggle-label"><span>Enable access point</span><input type="checkbox" id="thalow_ap_enable"></label>',
			'  <label><span>SSID</span><input type="text" id="thalow_ap_ssid" maxlength="32" spellcheck="false"></label>',
		'  <label><span>Security</span><select id="thalow_ap_authmode"><option value="OPEN">Open</option><option value="WPA2">WPA2-PSK</option><option value="WPA3">WPA3-PSK</option><option value="WPA2_WPA3">WPA2-PSK/WPA3-PSK</option></select></label>',
		'  <label id="thalow_ap_pw_label"><span>Password</span><input type="password" id="thalow_ap_pw" maxlength="63"></label>',
		'  <label><span>IP address</span><input type="text" id="thalow_ap_ip" maxlength="15" placeholder="10.10.0.2" spellcheck="false"></label>',
		'  <label><span>Subnet mask</span><input type="text" id="thalow_ap_netmask" maxlength="15" placeholder="255.255.255.0" spellcheck="false"></label>',
		'  <div class="panel-actions panel-actions-wrap">',
		'    <button type="button" id="thalow_save_ap" disabled>Save</button>',
		'  </div>',
		'  <div id="thalow_status_ap" class="status-message" aria-live="polite"></div>',
			'</div>',

			'<div class="panel">',
			'  <h3>Wi-Fi Station</h3>',
			'  <label class="toggle-label"><span>Enable station</span><input type="checkbox" id="thalow_sta_enable"></label>',
			'  <label><span>SSID</span><input type="text" id="thalow_sta_ssid" maxlength="32" spellcheck="false"></label>',
		'  <label><span>Password</span><input type="password" id="thalow_sta_pw" maxlength="63"></label>',
		'  <label class="toggle-label"><span>Use DHCP</span><input type="checkbox" id="thalow_sta_dhcp"></label>',
		'  <label><span>Static IP</span><input type="text" id="thalow_sta_ip" maxlength="15" spellcheck="false"></label>',
		'  <label><span>Netmask</span><input type="text" id="thalow_sta_netmask" maxlength="15" spellcheck="false"></label>',
		'  <label><span>Gateway</span><input type="text" id="thalow_sta_gw" maxlength="15" spellcheck="false"></label>',
		'  <div class="panel-actions panel-actions-wrap">',
		'    <button type="button" id="thalow_scan_btn" class="secondary">Scan networks (up to 30s)</button>',
		'    <button type="button" id="thalow_save_sta" disabled>Save</button>',
		'  </div>',
		'  <div id="thalow_status_sta" class="status-message" aria-live="polite"></div>',
		'  <div id="thalow_scan_wrap" hidden>',
		'    <table class="stats-table">',
		'      <thead><tr><th data-key="ssid">SSID</th><th data-key="rssi">RSSI (dBm)</th><th data-key="channel">Ch</th><th data-key="auth">Security</th><th></th></tr></thead>',
		'      <tbody></tbody>',
		'    </table>',
		'  </div>',
		'  <div id="thalow_scan_status" class="status-message" aria-live="polite"></div>',
		'</div>',

			'<div class="panel">',
			'  <h3>Bluetooth (BLE)</h3>',
			'  <label class="toggle-label"><span>Enable Bluetooth</span><input type="checkbox" id="thalow_ble_enable"></label>',
			'  <label><span>Bluetooth name</span><input type="text" id="thalow_ble_name" maxlength="29" spellcheck="false"></label>',
			'  <p class="note">Default name is derived from the MAC address. Changes apply after a reboot. Note: some Reticulum clients match a name starting with "RNode " and may not detect the node if it is renamed differently.</p>',
			'  <div class="panel-actions panel-actions-wrap">',
			'    <button type="button" id="thalow_save_ble" disabled>Save</button>',
			'  </div>',
			'  <div id="thalow_status_ble" class="status-message" aria-live="polite"></div>',
			'</div>',

			'<div class="panel">',
			'  <h3>LED</h3>',
			'  <label><span>Mode</span><select id="thalow_led_mode">' +
			'<option value="BLE">BLE</option>' +
			'<option value="TCP">TCP</option>' +
			'<option value="WIFI_STA">WiFi station</option>' +
			'<option value="WIFI_AP">WiFi access point</option>' +
			'<option value="TRAFFIC">Traffic</option>' +
			'</select></label>',
			'  <p class="note" id="thalow_led_mode_note"></p>',
			'  <label class="toggle-label"><span>Heartbeat (flash every 10s)</span><input type="checkbox" id="thalow_led_heartbeat"></label>',
			'  <p class="note">Brief flash every 10 seconds — confirms the device is powered on and running. It overlays the selected mode, so the LED always blinks even when the mode state is off.</p>',
			'  <div class="panel-actions panel-actions-wrap">',
			'    <button type="button" id="thalow_save_led" disabled>Save</button>',
			'  </div>',
			'  <div id="thalow_status_led" class="status-message" aria-live="polite"></div>',
			'</div>',

			/* TCP Radio Bridge panel. Mirrors the radio firmware's own panel so
			 * the layout is identical, but this controls the ESP32-side RNS
			 * TCP proxy (esp -> radio relay), not the radio's own bridge. */
			'<div class="panel">',
			'  <h3>TCP Radio Bridge</h3>',
			'  <label class="toggle-label"><span>Enable bridge</span><input type="checkbox" id="thalow_tcp_enable"></label>',
			'  <fieldset id="thalow_tcp_fields" disabled>',
			'    <label><span>Port</span><input type="number" id="thalow_tcp_port" min="1024" max="65535" step="1"></label>',
			'    <label><span>Whitelist</span><input type="text" id="thalow_tcp_whitelist" spellcheck="false"></label>',
			'  </fieldset>',
			'  <label><span>Client</span><span id="thalow_tcp_client">--</span></label>',
			'  <p class="note">This TCP socket forwards outgoing data to the radio interface and delivers incoming radio data back to the socket. The whitelist accepts comma-separated CIDRs (e.g. <code>10.10.0.0/24</code>); default <code>0.0.0.0/0</code> allows everyone.</p>',
			'  <div class="panel-actions panel-actions-wrap">',
			'    <button type="button" id="thalow_save_tcp" disabled>Save</button>',
			'  </div>',
			'  <div id="thalow_status_tcp" class="status-message" aria-live="polite"></div>',
			'</div>'
		].join('\n');
		return sec;
	}

	function updateApPwVisibility() {
		var am = $('thalow_ap_authmode');
		if (!am) return;
		var show = (am.value !== 'OPEN');
		var lbl = $('thalow_ap_pw_label');
		if (lbl) lbl.style.display = show ? '' : 'none';
	}

	function updateStaDhcp() {
		var dhcp = $('thalow_sta_dhcp');
		if (!dhcp) return;
		var on = dhcp.checked;
		['thalow_sta_ip', 'thalow_sta_netmask', 'thalow_sta_gw'].forEach(function (id) {
			var el = $(id);
			if (el) el.disabled = on;
		});
	}

	function updateScanBtn() {
		var enable = $('thalow_sta_enable');
		var btn = $('thalow_scan_btn');
		if (btn) btn.disabled = !(enable && enable.checked);
	}

	function updateTcpDisabled() {
		var enable = $('thalow_tcp_enable');
		var fields = $('thalow_tcp_fields');
		if (fields) fields.disabled = !(enable && enable.checked);
	}

	var LED_MODE_DESC = {
		BLE: 'Bluetooth connection state. Solid = connected; fast blink = pairing; slow blink = advertising (waiting for connection); off = BLE disabled.',
		TCP: 'External Reticulum TCP client count. Solid = at least one client is connected to the TCP proxy port; off = no clients connected.',
		WIFI_STA: 'Uplink status. Solid = ESP32 connected to a WiFi network; slow blink = not connected (waiting / trying).',
		WIFI_AP: 'Number of devices connected to this access point, shown as that many short blinks every 3 seconds (1 blink = 1 client, 2 = 2, etc.). Off = no clients.',
		TRAFFIC: 'Blinks on data flowing through the Reticulum proxy (both send and receive).'
	};

	function updateLedModeNote() {
		var sel = $('thalow_led_mode');
		var note = $('thalow_led_mode_note');
		if (sel && note) note.textContent = LED_MODE_DESC[sel.value] || '';
	}

	function injectStyles() {
		if (document.getElementById('thalow_pw_style')) return;
		var st = document.createElement('style');
		st.id = 'thalow_pw_style';
		st.textContent =
			'.pw-field{position:relative;flex:1 1 0;display:flex;align-items:center;min-width:0}' +
			'.pw-field input{width:100%;box-sizing:border-box;padding-right:30px;min-width:0}' +
			'.pw-eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);' +
			'background:transparent;border:none;cursor:pointer;padding:2px;' +
			'display:flex;align-items:center;line-height:0;color:#6b7280}' +
			'nav.tabs{flex-wrap:wrap}' +
			'nav.tabs button{padding:0.45rem 0.6rem;font-size:0.9rem;white-space:nowrap}' +
			'#thalow_scan_wrap{overflow-x:auto;-webkit-overflow-scrolling:touch;max-width:100%}' +
			'#thalow_scan_wrap table{min-width:440px}' +
			'#esp32stat_table tr.thalow-stat-section th{' +
			'font-size:0.78rem;text-transform:uppercase;letter-spacing:0.04em;' +
			'color:#6b7280;padding-top:0.7rem;border-bottom:1px solid #e5e7eb}';
		(document.head || document.documentElement).appendChild(st);
	}

	function makePasswordField(input) {
		if (!input || !input.parentNode) return;
		if (input.parentNode.classList &&
			input.parentNode.classList.contains('pw-field')) return;
		var span = document.createElement('span');
		span.className = 'pw-field';
		input.parentNode.insertBefore(span, input);
		span.appendChild(input);
		var eye = document.createElement('button');
		eye.type = 'button';
		eye.className = 'pw-eye';
		eye.tabIndex = -1;
		eye.setAttribute('aria-label', 'Toggle password visibility');
		var EYE_OPEN = '<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#6b7280" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle></svg>';
		var EYE_OFF = '<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#6b7280" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"></path><line x1="1" y1="1" x2="23" y2="23"></line></svg>';
		eye.innerHTML = EYE_OFF;
		eye.addEventListener('click', function () {
			if (input.type === 'password') {
				input.type = 'text';
				eye.innerHTML = EYE_OPEN;
			} else {
				input.type = 'password';
				eye.innerHTML = EYE_OFF;
			}
		});
		span.appendChild(eye);
	}

	function formSnapshotFor(fields) {
		var s = {};
		fields.forEach(function (id) {
			var el = $(id);
			if (!el) return;
			s[id] = (el.type === 'checkbox') ? el.checked : el.value;
		});
		return s;
	}

	function formBaseFor(sec) {
		try {
			return JSON.parse(baseSnapshots[sec] || '{}');
		} catch (e) {
			return {};
		}
	}

	function isSectionDirty(sec) {
		var base = formBaseFor(sec);
		var cur = formSnapshotFor(SECTION_FIELDS[sec]);
		var pwId = SECTION_PW[sec];
		if (pwId && cur[pwId] === '') cur[pwId] = base[pwId];
		return JSON.stringify(cur) !== JSON.stringify(base);
	}

	function markErr(id) {
		var el = $(id);
		if (el) el.classList.add('error');
	}

	function clearErrors(fields) {
		fields.forEach(function (id) {
			var el = $(id);
			if (el) el.classList.remove('error');
		});
	}

	function validateSection(sec) {
		clearErrors(SECTION_VALIDATE_FIELDS[sec]);

		var ok = true;
		var msg = true;
		if (sec === 'ap') {
			var apSsid = $('thalow_ap_ssid').value.trim();
			var apPw = $('thalow_ap_pw').value;
			var apAm = $('thalow_ap_authmode').value;
			var apIp = $('thalow_ap_ip').value.trim();
			var apNm = $('thalow_ap_netmask').value.trim();
			if (apSsid.length < 1 || apSsid.length > 32) { markErr('thalow_ap_ssid'); ok = false; }
			if (!isValidIpv4(apIp)) { markErr('thalow_ap_ip'); ok = false; }
			if (!isValidIpv4(apNm)) { markErr('thalow_ap_netmask'); ok = false; }
			if (apAm !== 'OPEN') {
				if (apPw.length === 0 && !apPwSet) {
					markErr('thalow_ap_pw'); ok = false;
					msg = 'Password required for WPA2/WPA3';
				} else if (apPw.length > 0 && (apPw.length < 8 || apPw.length > 63)) {
					markErr('thalow_ap_pw'); ok = false;
					msg = 'Password must be 8..63 characters';
				}
			}
		} else if (sec === 'sta') {
			var staEnable = $('thalow_sta_enable').checked;
			var staSsid = $('thalow_sta_ssid').value.trim();
			var staPw = $('thalow_sta_pw').value;
			var staDhcp = $('thalow_sta_dhcp').checked;
			if (staEnable && (staSsid.length < 1 || staSsid.length > 32)) { markErr('thalow_sta_ssid'); ok = false; }
			if (staPw.length > 0 && (staPw.length < 8 || staPw.length > 63)) { markErr('thalow_sta_pw'); ok = false; }
			if (!staDhcp) {
				var ip = $('thalow_sta_ip').value.trim();
				var nm = $('thalow_sta_netmask').value.trim();
				var gw = $('thalow_sta_gw').value.trim();
				if (!ip || !nm || !gw) {
					markErr('thalow_sta_ip'); markErr('thalow_sta_netmask'); markErr('thalow_sta_gw');
					ok = false;
				}
			}
		} else if (sec === 'ble') {
			var bleName = $('thalow_ble_name').value.trim();
			if (bleName.length < 1 || bleName.length > 29) { markErr('thalow_ble_name'); ok = false; }
		}
		return ok ? true : (msg === true ? 'Please fix the highlighted fields.' : msg);
	}

	function updateSectionDirty(sec) {
		var btn = $('thalow_save_' + sec);
		if (!btn) return;
		var valid = (validateSection(sec) === true);
		var changed = isSectionDirty(sec);
		btn.disabled = !(changed && valid);
	}

	function recompute(sec) {
		updateSectionDirty(sec);
	}

	function loadSettings() {
		if (typeof fetch !== 'function') return;
		fetch(CFG_API)
			.then(function (r) {
				return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status));
			})
			.then(function (d) {
		setCheckbox('thalow_ap_enable', d.wifi_ap_enabled);
		setInput('thalow_ap_ssid', d.wifi_ap_ssid);
		setInput('thalow_ap_authmode', d.wifi_ap_authmode || 'OPEN');
		setInput('thalow_ap_ip', d.wifi_ap_ip);
		setInput('thalow_ap_netmask', d.wifi_ap_netmask);
			setCheckbox('thalow_sta_enable', d.wifi_sta_enabled);
				setInput('thalow_sta_ssid', d.wifi_sta_ssid);
				setCheckbox('thalow_sta_dhcp', d.wifi_sta_dhcp);
				setInput('thalow_sta_ip', d.wifi_sta_ip);
				setInput('thalow_sta_netmask', d.wifi_sta_netmask);
				setInput('thalow_sta_gw', d.wifi_sta_gw);
				setCheckbox('thalow_ble_enable', d.ble_enabled);
				setInput('thalow_ble_name', d.ble_name);
			setInput('thalow_led_mode', d.led_mode || 'BLE');
			setCheckbox('thalow_led_heartbeat', d.led_heartbeat);
			updateLedModeNote();
			setCheckbox('thalow_tcp_enable', d.rns_proxy_enabled);
			(function () {
				var portEl = $('thalow_tcp_port');
				if (portEl) {
					var p = d.rns_proxy_port;
					portEl.value = (p === undefined || p === null || p === 0) ? 4242 : String(p);
				}
			})();
			setInput('thalow_tcp_whitelist', d.rns_proxy_whitelist || '0.0.0.0/0');
			updateTcpDisabled();

		var apPw = $('thalow_ap_pw');
		if (apPw) apPw.placeholder = d.wifi_ap_password_set ? '(set — type new to change)' : '(no password)';
		apPwSet = !!(d.wifi_ap_password_set);
		var staPw = $('thalow_sta_pw');
		if (staPw) staPw.placeholder = d.wifi_sta_password_set ? '(set — type new to change)' : '(no password)';

		updateStaDhcp();
		updateApPwVisibility();
		updateScanBtn();

				['ap', 'sta', 'ble', 'led', 'tcp'].forEach(function (sec) {
					baseSnapshots[sec] = JSON.stringify(formSnapshotFor(SECTION_FIELDS[sec]));
					var btn = $('thalow_save_' + sec);
					if (btn) btn.disabled = true;
					setStatus('thalow_status_' + sec, '', false);
				});
			})
			.catch(function (err) {
				['ap', 'sta', 'ble', 'led', 'tcp'].forEach(function (sec) {
					setStatus('thalow_status_' + sec, 'Failed to load settings: ' + err.message, true);
				});
			});
	}

	function saveSection(sec) {
		var v = validateSection(sec);
		if (v !== true) {
			setStatus('thalow_status_' + sec, v, true);
			updateSectionDirty(sec);
			return;
		}

		var apEnable = $('thalow_ap_enable').checked;
		var apSsid = $('thalow_ap_ssid').value.trim();
		var apPw = $('thalow_ap_pw').value;
		var apIp = $('thalow_ap_ip').value.trim();
		var apNm = $('thalow_ap_netmask').value.trim();
		var staEnable = $('thalow_sta_enable').checked;
		var staSsid = $('thalow_sta_ssid').value.trim();
		var staPw = $('thalow_sta_pw').value;
		var staDhcp = $('thalow_sta_dhcp').checked;
		var staIp = $('thalow_sta_ip').value.trim();
		var staNm = $('thalow_sta_netmask').value.trim();
		var staGw = $('thalow_sta_gw').value.trim();
		var bleEnable = $('thalow_ble_enable').checked;
		var bleName = $('thalow_ble_name').value.trim();

		var payload = {};
		var okMsg = '';
		var bothOff = false;

		if (sec === 'ap') {
			payload.wifi_ap_enabled = apEnable;
			payload.wifi_ap_ssid = apSsid;
			payload.wifi_ap_authmode = $('thalow_ap_authmode').value;
			if (apPw.length > 0) payload.wifi_ap_password = apPw;
			payload.wifi_ap_ip = apIp;
			payload.wifi_ap_netmask = apNm;
			okMsg = 'Saved. WiFi applies now (reconnect)';
			bothOff = (!apEnable && !staEnable);
		} else if (sec === 'sta') {
			payload.wifi_sta_enabled = staEnable;
			payload.wifi_sta_ssid = staSsid;
			if (staPw.length > 0) payload.wifi_sta_password = staPw;
			if (staDhcp) {
				payload.wifi_sta_dhcp = true;
			} else {
				payload.wifi_sta_dhcp = false;
				payload.wifi_sta_ip = staIp;
				payload.wifi_sta_netmask = staNm;
				payload.wifi_sta_gw = staGw;
			}
			okMsg = 'Saved. WiFi applies now (reconnect)';
			bothOff = (!apEnable && !staEnable);
		} else if (sec === 'ble') {
			payload.ble_enabled = bleEnable;
			payload.ble_name = bleName;
			okMsg = 'Saved. Applies after reboot';
		} else if (sec === 'led') {
			payload.led_mode = $('thalow_led_mode').value;
			payload.led_heartbeat = $('thalow_led_heartbeat').checked;
			okMsg = 'Saved.';
		} else if (sec === 'tcp') {
			payload.rns_proxy_enabled = $('thalow_tcp_enable').checked;
			var tp = $('thalow_tcp_port');
			if (tp) payload.rns_proxy_port = parseInt(tp.value, 10);
			payload.rns_proxy_whitelist = $('thalow_tcp_whitelist').value || '';
			okMsg = 'Saved.';
		}

		if (bothOff) {
			var go = window.confirm(
				'Both WiFi interfaces will be disabled. Web access will be lost ' +
				'until the device reboots. Continue?');
			if (!go) return;
		}

		if (sec === 'ap') {
			var apBase = formBaseFor('ap');
			var ipMaskChanged = (apBase['thalow_ap_ip'] !== apIp) ||
			                     (apBase['thalow_ap_netmask'] !== apNm);
			if (ipMaskChanged) {
				var go2 = window.confirm(
					'Changing the AP IP address or subnet mask will disconnect ' +
					'all connected clients. Continue?');
				if (!go2) return;
			}
		}

		var btn = $('thalow_save_' + sec);
		var statusId = 'thalow_status_' + sec;
		if (btn) btn.disabled = true;
		setStatus(statusId, 'Saving...', false);

		postJson(CFG_API, payload)
			.then(function (r) {
				return r.text().then(function (txt) {
					var data = txt ? JSON.parse(txt) : {};
					if (!r.ok) throw data;
					return data;
				});
			})
			.then(function () {
				setStatus(statusId, okMsg, false);
				loadSettings();
			})
			.catch(function (err) {
				setStatus(statusId,
					'Save failed: ' + (err && err.error ? err.error : (err.message || err)), true);
				if (btn) btn.disabled = false;
			});
	}

	function buildScanTable(list) {
		var tbody = document.querySelector('#thalow_scan_wrap tbody');
		if (!tbody) return;
		tbody.innerHTML = '';
		list.forEach(function (net) {
			var ssid = net.ssid != null ? net.ssid : '';
			var auth = net.auth != null ? net.auth : '';
			var hidden = (ssid === '');
			var tr = document.createElement('tr');
			tr.setAttribute('data-ssid', ssid);
			tr.setAttribute('data-auth', auth);
			var ssidCell = hidden
				? '<td><span style="color:#999">Hidden network</span></td>'
				: '<td>' + escapeHtml(ssid) + '</td>';
			tr.innerHTML =
				ssidCell +
				'<td>' + (net.rssi != null ? net.rssi : '') + '</td>' +
				'<td>' + (net.channel != null ? net.channel : '') + '</td>' +
				'<td>' + escapeHtml(auth) + '</td>' +
				'<td class="scan-connect-cell"><button type="button" class="secondary scan-connect-btn" style="background:#4b5563;color:#fff;border:none;border-radius:4px;padding:2px 8px;font-size:12px;cursor:pointer;">Connect</button></td>';
			tbody.appendChild(tr);
		});
	}

	function doScan() {
		var status = $('thalow_scan_status');
		var wrap = $('thalow_scan_wrap');
		setStatus('thalow_scan_status', 'Scanning… (up to 30s)', false);
		if (wrap) wrap.hidden = true;

		fetch(SCAN_API)
			.then(function (r) {
				return r.json().then(function (data) {
					if (!r.ok) throw data;
					return data;
				});
			})
			.then(function (list) {
				if (!Array.isArray(list)) list = [];
				buildScanTable(list);
				if (list.length === 0) {
					setStatus('thalow_scan_status', 'No networks found.', false);
					if (wrap) wrap.hidden = true;
				} else {
					setStatus('thalow_scan_status', list.length + ' networks found', false);
					if (wrap) wrap.hidden = false;
				}
			})
			.catch(function (err) {
				setStatus('thalow_scan_status',
					err && err.error ? err.error : 'Scan failed', true);
				if (wrap) wrap.hidden = true;
			});
	}

	function openConnectModal(ssid, auth) {
		var overlay = document.createElement('div');
		overlay.style.position = 'fixed';
		overlay.style.left = '0';
		overlay.style.top = '0';
		overlay.style.right = '0';
		overlay.style.bottom = '0';
		overlay.style.background = 'rgba(0,0,0,0.5)';
		overlay.style.display = 'flex';
		overlay.style.alignItems = 'center';
		overlay.style.justifyContent = 'center';
		overlay.style.zIndex = '9999';

		var panel = document.createElement('div');
		panel.className = 'panel';
		panel.style.width = '340px';
		panel.style.maxWidth = '92vw';

		var hidden = !ssid;
		var needsPassword = (auth !== 'OPEN');

		var html = '<h3>Connect to Wi-Fi</h3>';
		if (hidden) {
			html += '<label><span>SSID</span><input type="text" id="thalow_conn_ssid" placeholder="Enter hidden SSID" spellcheck="false"></label>';
		} else {
			html += '<label><span>SSID</span><input type="text" id="thalow_conn_ssid" value="' + escapeHtml(ssid) + '" readonly spellcheck="false"></label>';
		}
		if (needsPassword) {
			html += '<label><span>Password</span><input type="password" id="thalow_conn_pw" maxlength="63" spellcheck="false"></label>';
		} else {
			html += '<p class="note">Open network — no password.</p>';
		}
		html += '<div class="panel-actions panel-actions-wrap">' +
			'<button type="button" id="thalow_conn_connect">Connect</button>' +
			'<button type="button" id="thalow_conn_cancel" class="secondary">Cancel</button>' +
			'</div>';
		html += '<div class="status-message" id="thalow_conn_status" aria-live="polite"></div>';
		panel.innerHTML = html;
		overlay.appendChild(panel);
		document.body.appendChild(overlay);

		if (needsPassword) makePasswordField($('thalow_conn_pw'));

		var closed = false;
		var done = false;
		function closeModal() {
			if (closed) return;
			closed = true;
			document.removeEventListener('keydown', onKey);
			if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
		}
		function onKey(e) {
			if (e.key === 'Escape' || e.keyCode === 27) closeModal();
		}

		var connBtn = $('thalow_conn_connect');
		var cancelBtn = $('thalow_conn_cancel');

		overlay.addEventListener('click', function (e) {
			if (e.target === overlay) closeModal();
		});
		document.addEventListener('keydown', onKey);
		cancelBtn.addEventListener('click', closeModal);

		connBtn.addEventListener('click', function () {
			if (done) { closeModal(); return; }
			doConnect();
		});

		function doConnect() {
			var ssidInput = $('thalow_conn_ssid');
			var pwInput = $('thalow_conn_pw');
			var curSsid = ssidInput.value.trim();
			if (!curSsid) {
				setStatus('thalow_conn_status', 'Enter SSID', true);
				return;
			}
			var payload = { ssid: curSsid };
			if (needsPassword) {
				payload.password = pwInput ? pwInput.value : '';
			}

			var st = $('thalow_conn_status');
			st.textContent = 'Connecting…';
			st.classList.remove('ok', 'error');
			connBtn.disabled = true;

			postJson(CONNECT_API, payload)
				.then(function (r) {
					return r.json().then(function (d) {
						if (!r.ok) throw { __api: true, data: d };
						return d;
					});
				})
				.then(function (d) {
					if (d && d.success) {
						setStatus('thalow_conn_status', 'Connected. IP: ' + (d.ip || ''), false);
						connBtn.textContent = 'Close';
						connBtn.disabled = false;
						done = true;
						loadSettings();
					} else {
						setStatus('thalow_conn_status',
							(d && d.reason) ? d.reason : 'Connection failed', true);
						connBtn.disabled = false;
					}
				})
				.catch(function (err) {
					if (err && err.__api) {
						var d = err.data;
						setStatus('thalow_conn_status',
							(d && d.error) ? d.error
								: (d && d.reason) ? d.reason : 'Connection failed', true);
					} else {
						setStatus('thalow_conn_status', 'Connection to device lost', true);
					}
					connBtn.disabled = false;
				});
		}

		var firstInput = hidden ? $('thalow_conn_ssid')
			: (needsPassword ? $('thalow_conn_pw') : $('thalow_conn_connect'));
		if (firstInput) firstInput.focus();
	}

	function onScanRowClick(e) {
		var tr = e.target.closest ? e.target.closest('#thalow_scan_wrap tr') : null;
		if (!tr) return;
		var ssid = tr.getAttribute('data-ssid');
		if (ssid === null) return;
		setInput('thalow_sta_ssid', ssid);
		var pw = $('thalow_sta_pw');
		if (pw) pw.focus();
		recompute('sta');
	}

	function activateThalow() {
		var btns = document.querySelectorAll('nav.tabs button');
		if (btns.forEach) {
			btns.forEach(function (b) {
				b.classList.toggle('active', b.getAttribute('data-tab') === TAB);
			});
		}
		var secs = document.querySelectorAll('.tab-content');
		if (secs.forEach) {
			secs.forEach(function (s) { s.classList.remove('active'); });
		}
		var sec = $(TAB);
		if (sec) sec.classList.add('active');
		if (window.history && window.history.replaceState) {
			window.history.replaceState(null, '', '#' + TAB);
		}
		loadSettings();
	}

	function fmtMB(v) {
		if (v === null || v === undefined) return '--';
		return Math.round(v / 1048576) + ' MB';
	}
	function fmtKB(v) {
		if (v === null || v === undefined) return '--';
		return (v / 1024).toFixed(1) + ' KB';
	}
	function fmtMHz(v) {
		if (v === null || v === undefined) return '--';
		return (v / 1000000).toFixed(0) + ' MHz';
	}
	function fmtUptime(s) {
		if (s === null || s === undefined) return '--';
		s = Math.floor(s);
		var d = Math.floor(s / 86400); s %= 86400;
		var h = Math.floor(s / 3600); s %= 3600;
		var m = Math.floor(s / 60);
		var sec = s % 60;
		var parts = [];
		if (d) parts.push(d + 'd');
		if (h || d) parts.push(h + 'h');
		if (m || h || d) parts.push(m + 'm');
		parts.push(sec + 's');
		return parts.join(' ');
	}
	function fmtVal(v, suffix) {
		if (v === null || v === undefined) return '--';
		return v + (suffix || '');
	}

	function renderEsp32Stats(d) {
		var tbody = document.querySelector('#esp32stat_table tbody');
		if (!tbody) return;

		var heapTxt = '--';
		if (d.free_heap !== null && d.free_heap !== undefined &&
			d.total_heap !== null && d.total_heap !== undefined && d.total_heap > 0) {
			var usedHeap = d.total_heap - d.free_heap;
			var usedHeapKiB = Math.round(usedHeap / 1024);
			var totalHeapKiB = Math.round(d.total_heap / 1024);
			var heapPct = Math.round(usedHeap / d.total_heap * 100);
			heapTxt = usedHeapKiB + '/' + totalHeapKiB + ' KiB (' + heapPct + '%)';
		}

		var psramTxt = null;
		if (d.psram_total !== null && d.psram_total !== undefined &&
			d.psram_total > 0) {
			var usedPsram = d.psram_total - (d.psram_free || 0);
			var usedPsramKiB = Math.round(usedPsram / 1024);
			var ptotalKiB = Math.round(d.psram_total / 1024);
			var ppct = Math.round(usedPsram / d.psram_total * 100);
			psramTxt = usedPsramKiB + '/' + ptotalKiB + ' KiB (' + ppct + '%)';
		}

		var tempTxt = '--';
		if (d.temperature_c !== null && d.temperature_c !== undefined)
			tempTxt = Math.round(d.temperature_c) + ' °C';

		var vmv = (d.battery_voltage_mv !== null && d.battery_voltage_mv !== undefined)
			? d.battery_voltage_mv : 0;
		var bat;
		if (vmv > 0 && vmv < 1000) {
			bat = 'Disconnected (< 1 V)';
		} else if (vmv >= 1000) {
			bat = Math.round(d.battery_percent) + ' %  ' + (vmv / 1000).toFixed(2) + ' V';
		} else {
			bat = 'Disconnected (< 1 V)';
		}

		/* Group status/identity rows and network rows into sections. Each
		 * section is a {title, rows:[[label,val],...]}; null title means a flat
		 * group with no header. */
		var staStatusTxt = fmtVal(d.sta_status);
		if (d.sta_enabled && d.sta_rssi !== null && d.sta_rssi !== undefined)
			staStatusTxt += ' (' + Math.round(d.sta_rssi) + ' dBm)';

		var sections = [
			{
				title: 'Network',
				rows: [
					['WiFi mode', fmtVal(d.wifi_mode)],
					['Access point',
					 (d.ap_enabled ? fmtVal(d.ap_ssid) : 'disabled') +
					 (d.ap_enabled && d.ap_authmode ? ' [' + fmtVal(d.ap_authmode) + ']' : '')],
					['AP clients', d.ap_clients !== undefined && d.ap_clients !== null
					             ? String(d.ap_clients) : '--'],
					['AP IP', fmtVal(d.ip_ap)],
					['Station',
					 (d.sta_enabled ? (fmtVal(d.sta_ssid) || '(empty)') : 'disabled')],
					['Station status', staStatusTxt],
					['Station IP', fmtVal(d.ip_sta)],
					['Reticulum clients', (function () {
						var c = (d.tcp_clients !== undefined && d.tcp_clients !== null) ? String(d.tcp_clients) : '--';
						var cl = d.tcp_client || '';
						return cl ? (c + ' (' + cl + ')') : c;
					})()],
					['Hostname', fmtVal(d.hostname)],
					['MAC WiFi AP', fmtVal(d.mac_wifi_ap)],
					['MAC WiFi STA', fmtVal(d.mac_wifi_sta)],
					['MAC Bluetooth', fmtVal(d.mac_bt)]
				]
			},
			{
				title: 'Device',
				rows: [
					['Bluetooth', fmtVal(d.ble_state)],
					['Battery', bat],
					['Temperature', tempTxt],
					['Uptime', fmtUptime(d.uptime_sec)],
					['CPU usage', fmtVal(d.cpu_usage, ' %')]
				]
			},
			{
				title: 'System',
				rows: [
					['Firmware', fmtVal(d.fw_version)],
					['IDF version', fmtVal(d.idf_version)],
					['Chip', fmtVal(d.chip_model)],
					['Revision', fmtVal(d.chip_revision)],
					['Flash', fmtMB(d.flash_size)],
					['PSRAM', fmtMB(d.psram_size)],
					['Heap', heapTxt]
				]
			}
			];

		if (psramTxt !== null) {
			sections[2].rows.push(['PSRAM (heap)', psramTxt]);
		}

		var html = '';
		sections.forEach(function (sec) {
			if (sec.title)
				html += '<tr class="thalow-stat-section"><th colspan="2">' +
				        escapeHtml(sec.title) + '</th></tr>';
			sec.rows.forEach(function (r) {
				html += '<tr><th>' + escapeHtml(r[0]) + '</th><td>' + escapeHtml(r[1]) + '</td></tr>';
			});
		});
		tbody.innerHTML = html;

		/* Mirror the radio-style "Client" field in the TCP Radio Bridge panel.
		 * Shows the last accepted proxy client as IP:port, or '--'. */
		var tcpClientEl = $('thalow_tcp_client');
		if (tcpClientEl) {
			var cl = d.tcp_client || '';
			tcpClientEl.textContent = cl ? cl : '--';
		}
	}

	function loadEsp32Stats() {
		if (typeof fetch !== 'function') return;
		fetch(STATS_API)
			.then(function (r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
			.then(function (d) {
				renderEsp32Stats(d || {});
				esp32Loaded = true;
			})
			.catch(function () { /* keep last values on error */ });
	}

	function isDashboardActive() {
		var dash = $('dashboard');
		if (dash && dash.classList.contains('active')) return true;
		var btn = document.querySelector('nav.tabs button[data-tab="dashboard"]');
		if (btn && btn.classList.contains('active')) return true;
		return false;
	}

	function startEsp32Polling() {
		if (esp32Polling) return;
		esp32Polling = true;
		loadEsp32Stats();
		esp32PollTimer = setInterval(loadEsp32Stats, 3000);
	}

	function stopEsp32Polling() {
		esp32Polling = false;
		if (esp32PollTimer) { clearInterval(esp32PollTimer); esp32PollTimer = null; }
	}

	function setupEsp32Stats() {
		var dash = $('dashboard');
		if (!dash || $('esp32stat_table')) return;

		var panel = document.createElement('div');
		panel.className = 'panel';
		panel.innerHTML =
			'<h3>ESP32 Statistics</h3>' +
			'<table class="stats-table" id="esp32stat_table"><tbody>' +
			'<tr><td colspan="2">Loading…</td></tr>' +
			'</tbody></table>';
		dash.appendChild(panel);

		function sync() {
			if (isDashboardActive()) startEsp32Polling();
			else stopEsp32Polling();
		}

		sync();

		if (typeof MutationObserver !== 'undefined') {
			var obs = new MutationObserver(sync);
			if (dash) obs.observe(dash, { attributes: true, attributeFilter: ['class'] });
			var dashBtn = document.querySelector('nav.tabs button[data-tab="dashboard"]');
			if (dashBtn) obs.observe(dashBtn, { attributes: true, attributeFilter: ['class'] });
		}

		document.addEventListener('click', function (e) {
			var t = e.target.closest ? e.target.closest('nav.tabs button') : null;
			if (t) setTimeout(sync, 0);
		});
	}

	function inject() {
		if ($(TAB)) return;

		injectStyles();
		setupEsp32Stats();

		var nav = document.querySelector('nav.tabs');
		if (nav) nav.appendChild(buildTabButton());

		var main = document.querySelector('main');
		if (main) main.appendChild(buildSection());
		else document.body.appendChild(buildSection());

		document.addEventListener('click', function (e) {
			var t = e.target.closest ? e.target.closest('nav.tabs button') : null;
			if (!t) return;
			if (t.getAttribute('data-tab') === TAB) {
				e.preventDefault();
				activateThalow();
			}
		});

		$('thalow_save_ap').addEventListener('click', function () { saveSection('ap'); });
		$('thalow_save_sta').addEventListener('click', function () { saveSection('sta'); });
		$('thalow_save_ble').addEventListener('click', function () { saveSection('ble'); });
		$('thalow_save_led').addEventListener('click', function () { saveSection('led'); });
		$('thalow_save_tcp').addEventListener('click', function () { saveSection('tcp'); });
		$('thalow_sta_dhcp').addEventListener('change', function () { updateStaDhcp(); recompute('sta'); });
		$('thalow_sta_enable').addEventListener('change', function () { updateScanBtn(); recompute('sta'); });
		makePasswordField($('thalow_ap_pw'));
		makePasswordField($('thalow_sta_pw'));
		$('thalow_ap_authmode').addEventListener('change', function () {
			updateApPwVisibility();
			recompute('ap');
		});
		$('thalow_led_mode').addEventListener('change', function () { updateLedModeNote(); recompute('led'); });
		$('thalow_tcp_enable').addEventListener('change', function () { updateTcpDisabled(); recompute('tcp'); });
		$('thalow_scan_btn').addEventListener('click', doScan);
		var wrap = $('thalow_scan_wrap');
		if (wrap) wrap.addEventListener('click', function (e) {
			var btn = e.target.closest ? e.target.closest('.scan-connect-btn') : null;
			if (btn) {
				var tr = btn.closest('tr');
				if (tr) {
					openConnectModal(tr.getAttribute('data-ssid') || '', tr.getAttribute('data-auth') || '');
				}
				return;
			}
			onScanRowClick(e);
		});

		Object.keys(SECTION_FIELDS).forEach(function (sec) {
			SECTION_FIELDS[sec].forEach(function (id) {
				var el = $(id);
				if (!el) return;
				el.addEventListener('input', function (e) {
					var s = e.target && e.target.id ? sectionOf(e.target.id) : sec;
					if (s) recompute(s);
				});
				el.addEventListener('change', function (e) {
					var s = e.target && e.target.id ? sectionOf(e.target.id) : sec;
					if (s) recompute(s);
				});
			});
		});

		if (window.location && window.location.hash === '#' + TAB) {
			activateThalow();
		}

		lockRadioPanel('slip_enable',
			'These settings are managed by the ESP32.');
		lockRadioPanel('tcp_enable',
			'These settings are managed by the ESP32.');
	}

	/* Lock the radio panel containing the element with the given ID (slip_enable
	 * / tcp_enable). The panel stays visible but is made non-interactive by a
	 * CSS rule .thalow-locked { pointer-events:none !important } covering every
	 * input/select/textarea/button inside. That rule is unconditional -- the
	 * radio's own main.js re-enables fields via .disabled=false on every
	 * populateFromState(), but pointer-events:none ignores .disabled entirely,
	 * so the lock survives. We also flip .disabled=true ONCE at inject time for
	 * visual grey-out; we deliberately do NOT keep re-applying it. A
	 * MutationObserver that re-sets disabled/readonly in response to those same
	 * attributes mutating is a self-sustaining infinite loop (the original cause
	 * of the multi-GB tab crash). */
	function lockRadioPanel(anchorId, noteText) {
		try {
			var anchor = document.getElementById(anchorId);
			if (!anchor) return;
			var panel = anchor.closest('.panel');
			if (!panel) return;
			if (panel.getAttribute('data-thalow-locked') === '1') return;
			panel.setAttribute('data-thalow-locked', '1');

			/* Unique class so the CSS rule targets only locked panels. */
			panel.classList.add('thalow-locked');

			/* Inject the CSS rule once. pointer-events:none is the real lock --
			 * it cannot be undone by the radio JS setting .disabled=false. */
			if (!document.getElementById('thalow_lock_style')) {
				var st = document.createElement('style');
				st.id = 'thalow_lock_style';
				st.textContent =
					'.thalow-locked{opacity:0.6}' +
					'.thalow-locked input,.thalow-locked select,.thalow-locked textarea,.thalow-locked button{' +
					'pointer-events:none!important;' +
					'}';
				(document.head || document.documentElement).appendChild(st);
			}

			/* One-shot disabled pass for visual grey-out at inject time only.
			 * We do NOT re-apply this: the radio's own main.js toggles .disabled
			 * on dependent fields inside these panels (slip/tcp/telemetry enable
			 * toggles), and a MutationObserver that re-sets disabled/readonly in
			 * response to those very attributes mutating is an infinite loop
			 * (observer -> harden -> mutation -> observer ...) that blows up the
			 * microtask queue and crashes the tab with multi-GB RAM use. The real
			 * lock is the pointer-events:none rule above, which the radio JS
			 * cannot undo regardless of .disabled. */
			var els0 = panel.querySelectorAll('input, select, textarea, button');
			for (var i = 0; i < els0.length; i++) els0[i].disabled = true;

			var note = document.createElement('p');
			note.className = 'note';
			note.style.color = '#b45309';
			note.textContent = noteText;
			panel.insertBefore(note, panel.firstChild.nextSibling);
		} catch (e) { /* best effort */ }
	}

	if (document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', inject);
	} else {
		inject();
	}
})();
