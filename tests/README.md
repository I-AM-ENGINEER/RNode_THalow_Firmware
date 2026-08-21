# RNode-HaLow Firmware — Test Suite (3 levels)

| Level | Where | What | Runner |
|-------|-------|------|--------|
| 1 | host (gcc) | unit tests of pure-C modules | `make -C tests test` |
| 2 | QEMU esp32s3 | same modules + real FreeRTOS, xtensa codegen | `python tests/qemu/run_qemu.py` |
| 3 | real HW via USB | boot smoke, BLE, WiFi/API | `python -m pytest tests/hw -v` |

## Level 1 — host unit tests

Compiles production sources (`rns_framing.c`, `kiss.c`, `gzip_inflate.c`,
`switch.c`) against host stubs (`freertos_host_mock.h`, `stub/`).

```sh
cd tests && make test
```

Suites: `test_rns_framing` (10), `test_kiss` (6), `test_dns_overflow`
(proves the captive-DNS fix), `test_gzip_inflate` (7, embedded vectors in
`gzip_vectors.h` — no zlib needed), `test_switch_ring` (7, real switch.c
with mocked FreeRTOS).

## Level 2 — QEMU (ESP32-S3)

`tests/qemu/` is a standalone IDF project whose `app_main` runs Unity
tests compiled from the **same** production files with the **real**
xtensa toolchain and FreeRTOS scheduler.

One-time: `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`

```sh
python tests/qemu/run_qemu.py            # build + flash-image + run
python tests/qemu/run_qemu.py --no-build # skip idf.py build
```

Runner details: `idf.py qemu` is bypassed (its TCP-serial handshake is
unreliable on Windows); the runner regenerates `qemu_flash.bin` itself
and drives `qemu-system-xtensa -serial stdio` directly, waiting for the
machine-readable marker `QEMU_TESTS_RESULT PASS|FAIL`. Exit code 0 iff
PASS.

Windows note: the runner spawns idf.py through `idf_env.bat` (same env
hygiene as the repo `_build.bat`).

## Level 3 — real hardware (USB)

Requires: `pip install pyserial bleak pytest requests`. The board's
native USB-Serial/JTAG port (VID 0x303A) is auto-detected; tests reset
the board and capture the boot log.

```sh
python -m pytest tests/hw -v
```

* `test_serial_smoke.py` — boot banner, no panic/watchdog, switch/SLIP/AP
  ready, HaLow link activity.
* `test_ble_advertise.py` — device advertises `RNode HaLow <suffix>` with
  the NUS UUID, RSSI sanity.
* `test_wifi_api.py` — `/api/thalow_stats` HTTP + captive-DNS answer.
  Skips unless the host is already associated with the AP, or
  `THALOW_ALLOW_WIFI_SWITCH=1`. Note: the stock AP runs WPA3-SAE; a
  Windows open-profile cannot join it, so join manually (phone/laptop)
  or change `ap_auth` to WPA2 via the dashboard first.
