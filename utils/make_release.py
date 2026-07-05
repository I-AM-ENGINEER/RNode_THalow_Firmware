#!/usr/bin/env python3
"""
Prepares a flashable release package for the RNode-HaLow firmware.

Collects build artifacts (bootloader, partition table, application) into an
``out/`` folder, generates a SHA256 manifest (release.json), a ready-to-run
cross-platform flash script, a README, and an optional zip archive.

Usage:
    python utils/make_release.py                 # package existing build/
    python utils/make_release.py --build         # rebuild first, then package
    python utils/make_release.py --zip           # also create zip archive
    python utils/make_release.py --build -v 1.1.0 --zip
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
OUT_DIR = os.path.join(REPO_ROOT, "out")

# Build artifact -> (relative build path, flash offset)
ARTIFACTS = {
    "bootloader.bin":       ("bootloader/bootloader.bin",           "0x0"),
    "partition-table.bin":  ("partition_table/partition-table.bin", "0x8000"),
    "rnode-halow.bin":      ("rnode-halow.bin",                     "0x10000"),
}

FLASH_SETTINGS = {
    "chip": "esp32s3",
    "mode": "dio",
    "freq": "80m",
    "size": "16MB",
}

FLASH_SCRIPT = r'''#!/usr/bin/env python3
"""
Flashes the RNode-HaLow firmware to an ESP32-S3 over USB/serial.

Auto-detects the serial port (or use --port), flashes bootloader, partition
table, and application with esptool. esptool is installed via pip on first run
if not present.

Usage:
    python flash.py                  # auto-detect port
    python flash.py --port COM5      # specify port
    python flash.py --port COM5 --erase   # clean install
"""

import argparse
import subprocess
import sys


def install_esptool():
    print("Installing esptool via pip...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "--upgrade", "esptool"])


def esptool_available():
    try:
        subprocess.run([sys.executable, "-m", "esptool", "version"],
                       capture_output=True, check=True)
        return True
    except Exception:
        return False


def auto_detect_port():
    import glob
    candidates = []

    # Linux: /dev/ttyUSB*, /dev/ttyACM*
    candidates += glob.glob("/dev/ttyUSB*")
    candidates += glob.glob("/dev/ttyACM*")

    # macOS: /dev/cu.usbserial-*, /dev/cu.usbmodem*
    candidates += glob.glob("/dev/cu.usbserial*")
    candidates += glob.glob("/dev/cu.usbmodem*")

    # Windows: COM ports via registry
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        _name, value, _ = winreg.EnumValue(key, i)
                        candidates.append(value)
                        i += 1
                    except OSError:
                        break
        except Exception:
            pass

    candidates = sorted(set(candidates))
    return candidates[0] if candidates else None


def main():
    p = argparse.ArgumentParser(description="Flash RNode-HaLow firmware to ESP32-S3")
    p.add_argument("--port", help="Serial port (e.g. COM5, /dev/ttyACM0)")
    p.add_argument("--baud", type=int, default=921600, help="Flash baud rate (default: 921600)")
    p.add_argument("--erase", action="store_true", help="Erase entire flash first (clean install)")
    args = p.parse_args()

    if not esptool_available():
        install_esptool()

    port = args.port or auto_detect_port()
    if not port:
        print("No serial port found. Specify with --port <COMx>.")
        sys.exit(1)
    if not args.port:
        print(f"Using detected port: {port}")

    here = os.path.dirname(os.path.abspath(__file__))
    base = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", port,
        "--baud", str(args.baud),
        "--before", "default_reset",
        "--after", "hard_reset",
    ]

    if args.erase:
        print("Erasing flash...")
        subprocess.check_call(base + ["erase_flash"])

    print("Flashing firmware...")
    files = [
        ("0x0",     os.path.join(here, "bootloader.bin")),
        ("0x8000",  os.path.join(here, "partition-table.bin")),
        ("0x10000", os.path.join(here, "rnode-halow.bin")),
    ]
    flash_cmd = base + [
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "16MB",
    ]
    for offset, path in files:
        flash_cmd += [offset, path]

    subprocess.check_call(flash_cmd)

    print()
    print("Done! Firmware flashed successfully.")
    print("Reboot the device, then pair over BLE.")


if __name__ == "__main__":
    main()
'''

RELEASE_README = """# RNode-HaLow Firmware Release v{version}

## Contents
- `bootloader.bin`        - ESP32-S3 bootloader (flash @ 0x0)
- `partition-table.bin`   - Partition layout (flash @ 0x8000)
- `rnode-halow.bin`       - Application firmware (flash @ 0x10000)
- `flash.py`              - Cross-platform flash script (auto-detects port)
- `release.json`         - SHA256 hashes, offsets, flash settings

## Quick flash

### Windows
```
python flash.py
python flash.py --port COM5 --erase
```

### Linux / macOS
```
python3 flash.py
python3 flash.py --port /dev/ttyACM0 --erase
```

The script auto-installs `esptool` via pip if not present.

## Flash with esptool manually

```
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write_flash \\
    --flash_mode dio --flash_freq 80m --flash_size 16MB \\
    0x0     bootloader.bin \\
    0x8000  partition-table.bin \\
    0x10000 rnode-halow.bin
```

## Requirements
- Python 3.8+
- USB connection to the ESP32-S3 (T-Halow board's USB-CDC port)

## After flashing
1. Power on the device.
2. Scan for BLE devices and pair.
3. Connect from a KISS-compatible client (Columba, Sideband, Reticulum).
"""


def run_build():
    print("== Building firmware ==")
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path and os.path.exists(os.path.join(idf_path, "export.ps1")):
        # Windows: source export.ps1 in PowerShell
        cmd = ["powershell", "-Command",
               f". '{idf_path}\\export.ps1' *> $null; idf.py build"]
    else:
        cmd = ["idf.py", "build"]
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Prepare RNode-HaLow firmware release")
    parser.add_argument("--build", action="store_true", help="Run idf.py build before packaging")
    parser.add_argument("-v", "--version", default="v0.1.0b", help="Firmware version string (default: v0.1.0b)")
    parser.add_argument("--zip", action="store_true", help="Also create a zip archive")
    args = parser.parse_args()

    if args.build:
        run_build()

    # Verify artifacts
    for name, (rel, _off) in ARTIFACTS.items():
        path = os.path.join(BUILD_DIR, rel)
        if not os.path.exists(path):
            print(f"ERROR: Missing build artifact: {path}", file=sys.stderr)
            print("Run with --build or build first.", file=sys.stderr)
            sys.exit(1)

    # Clean output dir
    if os.path.exists(OUT_DIR):
        shutil.rmtree(OUT_DIR)
    os.makedirs(OUT_DIR)

    print("== Collecting artifacts ==")

    manifest = {}
    for name, (rel, offset) in ARTIFACTS.items():
        src = os.path.join(BUILD_DIR, rel)
        dst = os.path.join(OUT_DIR, name)
        shutil.copy2(src, dst)
        size = os.path.getsize(dst)
        digest = sha256(dst)
        manifest[name] = {"offset": offset, "sha256": digest, "size": size}
        print(f"  {name:<22} {size:>8} bytes  {digest}")

    # flash.py
    flash_path = os.path.join(OUT_DIR, "flash.py")
    with open(flash_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(FLASH_SCRIPT)
    if os.name != "nt":
        os.chmod(flash_path, 0o755)
    print("  flash.py")

    # README.md
    readme_path = os.path.join(OUT_DIR, "README.md")
    with open(readme_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(RELEASE_README.format(version=args.version))
    print("  README.md")

    # release.json
    release = {
        "version": args.version,
        "chip": FLASH_SETTINGS["chip"],
        "flash": {k: v for k, v in FLASH_SETTINGS.items() if k != "chip"},
        "files": manifest,
    }
    json_path = os.path.join(OUT_DIR, "release.json")
    with open(json_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(release, f, indent=2)
        f.write("\n")
    print("  release.json")

    # Optional zip
    if args.zip:
        print("== Creating zip archive ==")
        zip_name = f"rnode_firmware_thalow_{args.version}.zip"
        zip_path = os.path.join(os.path.dirname(OUT_DIR), zip_name)
        if os.path.exists(zip_path):
            os.remove(zip_path)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for root, _dirs, files in os.walk(OUT_DIR):
                for fname in sorted(files):
                    fpath = os.path.join(root, fname)
                    arcname = os.path.relpath(fpath, OUT_DIR)
                    zf.write(fpath, arcname)
        zip_size = os.path.getsize(zip_path)
        print(f"  {zip_name}  ({zip_size // 1024} KB)  {sha256(zip_path)}")

    print()
    print(f"Release ready in: {OUT_DIR}")


if __name__ == "__main__":
    main()
