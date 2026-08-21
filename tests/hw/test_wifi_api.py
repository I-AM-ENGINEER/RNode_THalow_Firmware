"""Level-3 WiFi + HTTP API test (best-effort).

Associating the host with the device SoftAP from a test is intrusive: it
changes the machine's WiFi state. The test therefore only runs when either
(a) the host is ALREADY associated with an RNode-Halow AP, or (b) the env
var THALOW_ALLOW_WIFI_SWITCH=1 is set. Otherwise it skips cleanly.
"""
import json
import os
import socket
import subprocess
import tempfile

import pytest

AP_IP = "10.10.0.2"
STATS_PATH = "/api/thalow_stats"


def _current_ssid():
    try:
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    for line in out.splitlines():
        if "SSID" in line and "BSSID" not in line:
            return line.split(":", 1)[1].strip()
    return None


def _host_has_ap_ip():
    """True if any local interface has an 10.10.0.x address."""
    try:
        host = socket.gethostbyname(socket.gethostname())
    except Exception:
        return False
    # gethostbyname returns one address only; probe routes instead
    out = subprocess.run(["route", "print", "-4", "10.10.0.2"],
                         capture_output=True, text=True, timeout=10).stdout
    return "10.10.0." in out


@pytest.fixture(scope="module")
def ap_reachable():
    ssid = _current_ssid()
    if ssid and ssid.startswith("RNode-Halow"):
        return True
    if _host_has_ap_ip():
        return True
    if os.environ.get("THALOW_ALLOW_WIFI_SWITCH") == "1":
        path = os.path.join(tempfile.gettempdir(), "thalow_ap_ssid.json")
        if os.path.exists(path):
            want = json.load(open(path))["ssid"]
            subprocess.run(["netsh", "wlan", f"name={want}", "connect",
                            f"ssid={want}"], capture_output=True, timeout=15)
            import time
            time.sleep(8)
            return _host_has_ap_ip()
    pytest.skip("host not associated with RNode-Halow AP "
                "(set THALOW_ALLOW_WIFI_SWITCH=1 to allow auto-join)")


def test_stats_endpoint(ap_reachable):
    import requests
    r = requests.get(f"http://{AP_IP}{STATS_PATH}", timeout=8)
    assert r.status_code == 200, f"stats HTTP {r.status_code}"
    data = r.json()
    # dashboard stats payload must contain at least uptime/heap keys
    joined = json.dumps(data)
    assert any(k in joined for k in ("heap", "uptime", "free")), \
        f"unexpected stats payload: {joined[:300]}"


def test_captive_dns(ap_reachable):
    """The captive DNS on the AP must resolve arbitrary names to 10.10.0.2."""
    q = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00" \
        b"\x07example\x03com\x00\x00\x01\x00\x01"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(q, (AP_IP, 53))
    resp, _ = s.recvfrom(512)
    s.close()
    assert len(resp) > 12 and resp[-4:] == bytes([10, 10, 0, 2]), \
        f"DNS answer wrong: {resp.hex()}"
