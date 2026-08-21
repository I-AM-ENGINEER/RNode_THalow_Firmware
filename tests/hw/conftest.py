"""Shared fixtures for Level-3 hardware tests (real ESP32-S3 over USB)."""
import time
import pytest
import serial
import serial.tools.list_ports

BAUD = 115200
ESPRESSIF_VID = 0x303A      # native USB-Serial/JTAG
CH343_VID = 0x1A86          # external UART bridge


def find_console_port():
    """Prefer the Espressif native USB-JTAG/serial port for the console."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID:
            return p.device
    for p in ports:
        if p.vid == CH343_VID:
            return p.device
    return None


@pytest.fixture(scope="session")
def console_port():
    port = find_console_port()
    if not port:
        pytest.skip("no ESP32 serial port found")
    return port


@pytest.fixture(scope="session")
def boot_log(console_port):
    """Reset the board and capture the first ~20 s of console output."""
    with serial.Serial(console_port, BAUD, timeout=1) as s:
        # enter run mode & reset via classic RTS/DTR dance
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setRTS(False)
        time.sleep(0.05)
        s.reset_input_buffer()

        chunks = []
        t0 = time.time()
        while time.time() - t0 < 20:
            data = s.read(1024)
            if data:
                chunks.append(data)
        text = b"".join(chunks).decode("utf-8", "replace")
    return text


def extract_ap_ssid(log):
    """Pull the SoftAP SSID from a 'SoftAP <SSID> on ...' log line."""
    import re
    m = re.search(r"SoftAP\s+(\S+)\s+on", log)
    return m.group(1) if m else None
