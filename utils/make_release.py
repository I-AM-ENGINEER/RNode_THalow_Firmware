#!/usr/bin/env python3
"""
Prepares a flashable release package for the RNode-HaLow firmware.

Produces a zip in RNode-Firmware convention:
  rnode_firmware_thalow.bin          - application image
  rnode_firmware_thalow.bootloader   - ESP32-S3 bootloader
  rnode_firmware_thalow.partitions   - partition table
  rnode_firmware_thalow.boot_app0   - OTA data (otadata initial)
  esptool.py                         - standalone esptool wrapper
  release.json                       - version + SHA256 (rnodeconf format)

Usage:
    python utils/make_release.py                     # package existing build/
    python utils/make_release.py -v v0.2.0           # specify version
    python utils/make_release.py --build -v v1.0.0   # rebuild then package
    python utils/make_release.py --no-zip            # skip zip
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

FW_PREFIX = "rnode_firmware_thalow"
ZIP_NAME = FW_PREFIX + ".zip"

# Build artifact -> (relative build path, flash offset)
ARTIFACTS = [
    (FW_PREFIX + ".bin",         "rnode-halow.bin",                     "0x10000"),
    (FW_PREFIX + ".bootloader",  "bootloader/bootloader.bin",            "0x0"),
    (FW_PREFIX + ".partitions",  "partition_table/partition-table.bin",  "0x8000"),
    (FW_PREFIX + ".boot_app0",   "ota_data_initial.bin",                 "0xe000"),
]

FLASH_SETTINGS = {
    "chip": "esp32s3",
    "mode": "dio",
    "freq": "80m",
    "size": "16MB",
}

# Standalone esptool wrapper – auto-installs esptool via pip if missing.
# Required by rnodeconf, which looks for esptool.py in the firmware dir.
ESPTOOL_PY = r'''#!/usr/bin/env python3
"""Standalone esptool wrapper.

Behaves like a single-file esptool.py: if the ``esptool`` package is not
installed, it is installed via pip, then esptool's CLI is invoked.
"""
import os
import sys

# Remove this script's own directory from sys.path so that ``import esptool``
# finds the installed package, not this wrapper file (self-shadowing).
_this_dir = os.path.dirname(os.path.abspath(__file__))
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != _this_dir]
# Clear any cached self-import.
for _mod in list(sys.modules):
    if _mod == "esptool" or _mod.startswith("esptool."):
        del sys.modules[_mod]

try:
    import esptool
except ImportError:
    import subprocess
    print("esptool not found, installing via pip...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "esptool"])
    import esptool

if __name__ == "__main__":
    esptool.main(sys.argv[1:])
'''

# Cross-platform flash script bundled in the zip.
FLASH_PY = r'''#!/usr/bin/env python3
"""
Flashes the RNode-HaLow firmware to an ESP32-S3 over USB/serial.

Auto-detects the serial port (or use --port), flashes bootloader, partition
table, boot_app0 and application with esptool.  esptool is installed via pip
on first run if not present.

Usage:
    python flash.py                       # auto-detect port
    python flash.py --port COM6           # specify port
    python flash.py --port COM6 --erase   # clean install (erase first)
"""

import argparse
import glob
import os
import subprocess
import sys


def auto_detect_port():
    # Prefer pyserial's port enumeration (cleaner than winreg on Windows).
    try:
        from serial.tools import list_ports
        ports = sorted(p.device for p in list_ports.comports())
        if ports:
            return ports[0]
    except Exception:
        pass

    # Fallback: glob + Windows registry.
    candidates = []
    candidates += glob.glob("/dev/ttyUSB*")
    candidates += glob.glob("/dev/ttyACM*")
    candidates += glob.glob("/dev/cu.usbserial*")
    candidates += glob.glob("/dev/cu.usbmodem*")
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        _name, value, _ = winreg.EnumValue(key, i)
                        # Registry values may include a trailing colon.
                        candidates.append(value.rstrip(":"))
                        i += 1
                    except OSError:
                        break
        except Exception:
            pass
    candidates = sorted(set(candidates))
    return candidates[0] if candidates else None


def main():
    p = argparse.ArgumentParser(description="Flash RNode-HaLow firmware to ESP32-S3")
    p.add_argument("--port", help="Serial port (e.g. COM6, /dev/ttyACM0)")
    p.add_argument("--baud", type=int, default=921600, help="Flash baud rate (default: 921600)")
    p.add_argument("--erase", action="store_true", help="Erase entire flash first (clean install)")
    args = p.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    # Use the bundled esptool.py wrapper (auto-installs esptool if missing).
    esptool_py = os.path.join(here, "esptool.py")
    base = [
        sys.executable, esptool_py,
        "--chip", "esp32s3",
        "--port", args.port or "",
        "--baud", str(args.baud),
        "--before", "default-reset",
        "--after", "hard-reset",
    ]

    port = args.port or auto_detect_port()
    if not port:
        print("No serial port found. Specify with --port <COMx>.")
        sys.exit(1)
    if not args.port:
        print(f"Using detected port: {port}")
    base[base.index("--port") + 1] = port

    if args.erase:
        print("Erasing flash...")
        subprocess.check_call(base + ["erase-flash"])

    print("Flashing firmware...")
    files = [
        ("0x0",     os.path.join(here, "rnode_firmware_thalow.bootloader")),
        ("0x8000",  os.path.join(here, "rnode_firmware_thalow.partitions")),
        ("0xe000",  os.path.join(here, "rnode_firmware_thalow.boot_app0")),
        ("0x10000", os.path.join(here, "rnode_firmware_thalow.bin")),
    ]
    flash_cmd = base + [
        "write-flash", "-z",
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "16MB",
    ]
    for offset, path in files:
        if not os.path.isfile(path):
            print(f"ERROR: Missing {path}")
            sys.exit(1)
        flash_cmd += [offset, path]

    subprocess.check_call(flash_cmd)

    print()
    print("Done! Firmware flashed successfully.")
    print("Reboot the device, then pair over BLE.")


if __name__ == "__main__":
    main()
'''

RELEASE_README = """# RNode-HaLow Firmware Release {version}

## Contents
- `rnode_firmware_thalow.bin`         - Application firmware (flash @ 0x10000)
- `rnode_firmware_thalow.bootloader`   - ESP32-S3 bootloader (flash @ 0x0)
- `rnode_firmware_thalow.partitions`   - Partition table (flash @ 0x8000)
- `rnode_firmware_thalow.boot_app0`    - OTA data (flash @ 0xe000)
- `esptool.py`                         - Standalone esptool wrapper
- `flash.py`                           - Cross-platform flash script
- `release.json`                       - Version + SHA256 manifest

## Quick flash

### Windows
```
python flash.py
python flash.py --port COM6 --erase
```

### Linux / macOS
```
python3 flash.py
python3 flash.py --port /dev/ttyACM0 --erase
```

The script auto-installs `esptool` via pip if not present.

## Flash with esptool manually

```
python -m esptool --chip esp32s3 --port COM6 --baud 921600 \\
    --before default_reset --after hard_reset \\
    write_flash -z \\
    --flash_mode dio --flash_freq 80m --flash_size 16MB \\
    0x0     rnode_firmware_thalow.bootloader \\
    0x8000  rnode_firmware_thalow.partitions \\
    0xe000  rnode_firmware_thalow.boot_app0 \\
    0x10000 rnode_firmware_thalow.bin
```

## Requirements
- Python 3.8+
- USB connection to the ESP32-S3 (T-Halow board's USB-CDC port)

## After flashing
1. Power on the device.
2. Scan for BLE devices and pair.
3. Connect from a KISS-compatible client (Columba, Sideband).
"""


def run_build():
    print("== Building firmware ==")
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path and os.path.exists(os.path.join(idf_path, "export.ps1")):
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
    parser.add_argument("--no-zip", action="store_true", help="Skip creating the zip archive")
    args = parser.parse_args()

    if args.build:
        run_build()

    # Verify artifacts
    for _name, rel, _off in ARTIFACTS:
        path = os.path.join(BUILD_DIR, rel)
        if not os.path.exists(path):
            print(f"ERROR: Missing build artifact: {path}", file=sys.stderr)
            print("Run with --build or build first.", file=sys.stderr)
            sys.exit(1)

    # Clean output dir (tolerate locked empty subdirs from open terminals).
    if os.path.exists(OUT_DIR):
        def _onerror(func, path, exc_info):
            # If a dir is locked (cwd of a terminal), try clearing its
            # contents; ignore the final rmdir failure.
            if os.path.isdir(path):
                for entry in os.listdir(path):
                    p = os.path.join(path, entry)
                    if os.path.isdir(p):
                        shutil.rmtree(p, ignore_errors=True)
                    else:
                        try:
                            os.unlink(p)
                        except OSError:
                            pass
        shutil.rmtree(OUT_DIR, onerror=_onerror)
    os.makedirs(OUT_DIR, exist_ok=True)

    # Stage files in a temp subfolder so the zip never packs itself.
    pkg_dir = os.path.join(OUT_DIR, "_pkg")

    print("== Collecting artifacts ==")

    manifest = {}
    for name, rel, offset in ARTIFACTS:
        src = os.path.join(BUILD_DIR, rel)
        dst = os.path.join(pkg_dir, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        size = os.path.getsize(dst)
        digest = sha256(dst)
        manifest[name] = {"offset": offset, "sha256": digest, "size": size}
        print(f"  {name:<38} {size:>8} bytes  {digest}")

    # esptool.py (standalone wrapper)
    esptool_path = os.path.join(pkg_dir, "esptool.py")
    with open(esptool_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(ESPTOOL_PY)
    if os.name != "nt":
        os.chmod(esptool_path, 0o755)
    print("  esptool.py")

    # flash.py
    flash_path = os.path.join(pkg_dir, "flash.py")
    with open(flash_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(FLASH_PY)
    if os.name != "nt":
        os.chmod(flash_path, 0o755)
    print("  flash.py")

    # README.md
    readme_path = os.path.join(pkg_dir, "README.md")
    with open(readme_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(RELEASE_README.format(version=args.version))
    print("  README.md")

    # release.json (inside the zip: version, flash settings, per-file hashes).
    # This is stable – it does NOT include the zip hash, so creating the zip
    # does not change its own hash.
    json_path = os.path.join(pkg_dir, "release.json")
    release_internal = {
        "version": args.version,
        "flash": {k: v for k, v in FLASH_SETTINGS.items()},
        "files": manifest,
    }
    with open(json_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(release_internal, f, indent=2)
        f.write("\n")
    print("  release.json")

    # Create zip.  Zip lives at out/ root, contents from _pkg/.
    if not args.no_zip:
        print("== Creating zip archive ==")
        zip_path = os.path.join(OUT_DIR, ZIP_NAME)
        if os.path.exists(zip_path):
            os.remove(zip_path)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for root, _dirs, files in os.walk(pkg_dir):
                for fname in sorted(files):
                    fpath = os.path.join(root, fname)
                    arcname = os.path.relpath(fpath, pkg_dir)
                    zf.write(fpath, arcname)
        zip_size = os.path.getsize(zip_path)
        zip_hash = sha256(zip_path)
        print(f"  {ZIP_NAME}  ({zip_size // 1024} KB)  {zip_hash}")

        # Standalone release.json in rnodeconf format: { "zip_name": { "version", "hash" } }
        # rnodeconf downloads this file separately and uses the hash to verify the zip.
        rnodeconf_release = {
            ZIP_NAME: {
                "version": args.version,
                "hash": zip_hash,
            }
        }
        with open(os.path.join(OUT_DIR, "release.json"), "w",
                  encoding="utf-8", newline="\n") as f:
            json.dump(rnodeconf_release, f, indent=2)
            f.write("\n")

    # Remove the staging folder so out/ only contains the zip + release.json.
    shutil.rmtree(pkg_dir, ignore_errors=True)

    print()
    print(f"Release ready in: {OUT_DIR}")


if __name__ == "__main__":
    main()
