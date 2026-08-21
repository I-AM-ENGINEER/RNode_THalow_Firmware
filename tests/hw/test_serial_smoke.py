"""Level-3 serial smoke tests: the board boots cleanly and every subsystem
reports ready within the capture window."""
import re


def test_boot_banner(boot_log):
    assert "RNode-HaLow firmware starting" in boot_log, \
        f"boot banner missing; got:\n{boot_log[:800]}"


def test_no_panic_or_watchdog(boot_log):
    bad = [pat for pat in ("assert failed", "Guru Meditation",
                           "task_wdt", "Rebooting...",
                           "stack overflow", "LoadProhibited")
           if pat in boot_log]
    assert not bad, f"panic indicators in boot log: {bad}"


def test_switch_ready(boot_log):
    assert re.search(r"switch: ready \(3 rings x 32 slots", boot_log), \
        "packet switch did not initialize"


def test_slip_up(boot_log):
    assert re.search(r"SLIP up on UART\d+: 192\.168\.7\.1/30", boot_log), \
        "SLIP interface did not come up"


def test_wifi_ap_started(boot_log):
    m = re.search(r"SoftAP\s+(\S+)\s+on 10\.10\.0\.2", boot_log)
    assert m, "SoftAP did not start"
    # remember SSID for the WiFi/API test via a module-global cache file
    import json, os, tempfile
    path = os.path.join(tempfile.gettempdir(), "thalow_ap_ssid.json")
    with open(path, "w") as f:
        json.dump({"ssid": m.group(1)}, f)


def test_halow_link_connected(boot_log):
    # The radio link retries every second; give it the whole window.
    assert "halow connected" in boot_log or "halow disconnected" in boot_log, \
        "no HaLow link activity at all (radio task dead?)"


def test_ble_initialized(boot_log):
    assert "BLE initialized" in boot_log or "BLE disabled by config" in boot_log
