#!/usr/bin/env python3
"""run_qemu.py — Level 2 runner: build & execute the Unity test firmware
in QEMU (ESP32-S3) headlessly, asserting the final result marker.

Usage:
    python run_qemu.py                 # build + run
    python run_qemu.py --no-build      # reuse existing build

Exit code 0 iff 'QEMU_TESTS_RESULT PASS' is observed on the serial output.

ESP-IDF environment: on Windows the runner wraps every idf.py call in the
repo's _build.bat (which exports the IDF env); elsewhere plain `idf.py`
must already be on PATH. QEMU binaries are fetched transparently by IDF's
esp-idf-qemu support on first run.
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

RESULT_RE = re.compile(r"QEMU_TESTS_RESULT (PASS|FAIL)")
DEFAULT_TIMEOUT = 240

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ENV_BAT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "idf_env.bat")


def idf_popen(args, cwd):
    """Spawn `idf.py <args>` with the proper environment wrapper."""
    if os.name == "nt":
        line = ENV_BAT + " >nul 2>nul & idf.py " + " ".join(args)
        return subprocess.Popen(["cmd", "/c", line], cwd=cwd,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8",
                                errors="replace")
    return subprocess.Popen(["idf.py"] + args, cwd=cwd,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")


def refresh_flash_image(proj):
    """Regenerate build/qemu_flash.bin when the app is newer than it.

    Normally 'idf.py qemu' does this; we do it ourselves so the runner can
    drive qemu-system-xtensa directly (its TCP-serial handshake is flaky on
    Windows)."""
    b = os.path.join(proj, "build")
    app = os.path.join(b, "qemu_tests.bin")
    bl = os.path.join(b, "bootloader", "bootloader.bin")
    pt = os.path.join(b, "partition_table", "partition-table.bin")
    out = os.path.join(b, "qemu_flash.bin")
    for f in (app, bl, pt):
        if not os.path.exists(f):
            print(f"[run_qemu] missing {f}; build first", file=sys.stderr)
            return False
    newest_src = max(os.path.getmtime(x) for x in (app, bl, pt))
    if os.path.exists(out) and os.path.getmtime(out) >= newest_src:
        return True
    print("[run_qemu] regenerating qemu_flash.bin...")
    line = ("python -m esptool --chip esp32s3 merge_bin "
            f"--output {out} --fill-flash-size 2MB "
            "--flash_mode dio --flash_freq 80m --flash_size 2MB "
            f"0x0 {bl} 0x8000 {pt} 0x10000 {app}")
    rc = subprocess.call(["cmd", "/c", ENV_BAT + " >nul 2>nul & " + line])
    return rc == 0


def find_qemu():
    """Locate qemu-system-xtensa installed by idf_tools (or on PATH)."""
    import glob
    pats = [os.path.expanduser("~/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa.exe"),
            os.path.expanduser("~/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa")]
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    from shutil import which
    return which("qemu-system-xtensa")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    args = ap.parse_args()

    proj = os.path.dirname(os.path.abspath(__file__))

    if not args.no_build:
        print("[run_qemu] building test firmware...")
        p = idf_popen(["-B", "build", "build"], proj)
        out = p.communicate()[0]
        if p.returncode != 0:
            print(out[-4000:], file=sys.stderr)
            print("[run_qemu] BUILD FAILED", file=sys.stderr)
            return 2
        print("[run_qemu] build OK")

    if not refresh_flash_image(proj):
        return 2
    print("[run_qemu] launching QEMU (direct, stdio serial)...")
    flash = os.path.join(proj, "build", "qemu_flash.bin")
    efuse = os.path.join(proj, "build", "qemu_efuse.bin")
    if not (os.path.exists(flash) and os.path.exists(efuse)):
        print("[run_qemu] run 'idf.py -B build qemu' once to generate "
              "flash/efuse images", file=sys.stderr)
        return 2

    qexe = find_qemu()
    if not qexe:
        print("[run_qemu] qemu-system-xtensa not found; install via: "
              "python $IDF_PATH/tools/idf_tools.py install qemu-xtensa",
              file=sys.stderr)
        return 2
    print(f"[run_qemu] qemu: {qexe}")

    qargs = [qexe, "-M", "esp32s3", "-m", "32M",
             "-drive", f"file={flash},if=mtd,format=raw",
             "-drive", f"file={efuse},if=none,format=raw,id=efuse",
             "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
             "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
             "-display", "none", "-serial", "stdio"]
    proc = subprocess.Popen(qargs, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")

    result = None
    deadline = time.time() + args.timeout
    lines = []

    def pump():
        nonlocal result
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip()
            lines.append(line)
            m = RESULT_RE.search(line)
            if m and result is None:
                result = m.group(1)
            # live tail so failures are visible in CI logs
            print(f"[qemu] {line}", flush=True)

    th = threading.Thread(target=pump, daemon=True)
    th.start()

    while time.time() < deadline:
        if result is not None:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    if proc.poll() is None:
        time.sleep(1.0)  # let trailing lines arrive
        print("[run_qemu] terminating qemu...")
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()

    if result == "PASS":
        print("\n[run_qemu] LEVEL-2 SUITE PASSED")
        return 0
    if result == "FAIL":
        print("\n[run_qemu] LEVEL-2 SUITE FAILED", file=sys.stderr)
        return 1
    print(f"\n[run_qemu] marker not seen within {args.timeout}s "
          f"(rc={proc.returncode})", file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main())
