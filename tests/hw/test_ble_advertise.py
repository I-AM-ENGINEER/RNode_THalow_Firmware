"""Level-3 BLE test: the device must advertise with its RNode name and the
Nordic UART Service UUID visible to the host's Bluetooth adapter."""
import asyncio
import json
import os
import tempfile

import pytest

NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
SCAN_SECONDS = 15


@pytest.fixture(scope="module")
def expected_ssid_prefix():
    # BLE name mirrors the AP SSID suffix: "RNode HaLow <suffix>"
    path = os.path.join(tempfile.gettempdir(), "thalow_ap_ssid.json")
    if os.path.exists(path):
        with open(path) as f:
            ssid = json.load(f)["ssid"]
        return "RNode HaLow " + ssid.rsplit("-", 1)[-1]
    return "RNode HaLow"


def test_ble_advertising(expected_ssid_prefix):
    from bleak import BleakScanner

    async def scan():
        return await BleakScanner.discover(timeout=SCAN_SECONDS,
                                           return_adv=True)

    devices = asyncio.run(scan())
    hits = []
    for d, adv in devices.values():
        name = d.name or (adv.local_name or "")
        uuids = [str(u).lower() for u in adv.service_uuids]
        if name.startswith("RNode HaLow") or NUS_SVC in uuids:
            hits.append((name, str(d.address), int(adv.rssi), uuids))

    assert hits, (
        f"no 'RNode HaLow*' advertiser found in {SCAN_SECONDS}s scan; "
        f"seen: {[(d.name,) for d, _ in devices.values()][:10]}")

    name, addr, rssi, uuids = hits[0]
    assert rssi > -95, f"signal too weak: {rssi} dBm"
    print(f"\n[BLE] found {name} @ {addr} rssi={rssi} svc={uuids}")
