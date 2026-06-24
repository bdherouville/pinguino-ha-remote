#!/usr/bin/env python3
"""Local assessment runner for touchscreen firmware changes.

The default run is intentionally fast and dependency-light. Optional ESP-IDF
build and boot-log checks can be enabled when the local toolchain or hardware is
available.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "esp32_bridge"
IDF_EXPORT = Path.home() / ".espressif" / "v6.0.1" / "esp-idf" / "export.sh"

FAST_GATES = [
    ["python3", "tools/test_touchscreen_contracts.py"],
    ["python3", "tools/check_touchscreen_firmware.py"],
]

REVIEW_CHECKLIST = [
    "Scope is limited to the requested behavior; unrelated refactors are separate.",
    "HTTP, MQTT, HA discovery, UART strings and NVS keys are preserved or migrated.",
    "Button commands still flow through the command queue; no direct UI/HTTP/MQTT UART writes.",
    "Changed behavior has source tests, HIL coverage, or a documented manual acceptance item.",
    "Chip target, flash size, partition offsets and GPIO ownership match the touched flavor.",
    "LVGL callbacks, touch reads, display flush and UI task loops do not block.",
    "Missing nRF52840, BME680, Wi-Fi or MQTT still boots and degrades visibly.",
    "No Wi-Fi/MQTT secrets are logged, exposed in status APIs, or committed.",
    "Firmware changes record build size and are flashed/boot-checked when hardware is available.",
]


def run(cmd: list[str], cwd: Path = ROOT) -> int:
    print(f"\n$ {' '.join(cmd)}")
    completed = subprocess.run(cmd, cwd=cwd)
    return completed.returncode


def run_idf_build(flavor: str) -> int:
    if not IDF_EXPORT.exists():
        print(f"\nERROR: ESP-IDF export script not found: {IDF_EXPORT}", file=sys.stderr)
        return 2

    builds = {
        "headless": (
            "build-headless-esp32s3",
            "sdkconfig.headless",
            "sdkconfig.defaults;sdkconfig.defaults.headless",
        ),
        "touchscreen": (
            "build-touchscreen-esp32",
            "sdkconfig.touchscreen",
            "sdkconfig.defaults;sdkconfig.defaults.touchscreen",
        ),
    }
    build_dir, sdkconfig, defaults = builds[flavor]
    command = (
        f"source {IDF_EXPORT} >/dev/null && "
        f"idf.py -B {build_dir} -DSDKCONFIG={sdkconfig} "
        f"-DSDKCONFIG_DEFAULTS='{defaults}' build"
    )
    print(f"\n$ {command}")
    completed = subprocess.run(["bash", "-lc", command], cwd=FW)
    return completed.returncode


def capture_boot_log(port: str, seconds: int) -> int:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError:
        print("\nERROR: pyserial is required for --boot-log", file=sys.stderr)
        return 2

    print(f"\nCapturing {seconds}s boot log from {port}")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except Exception as exc:  # pragma: no cover - hardware path
        print(f"ERROR: could not open {port}: {exc}", file=sys.stderr)
        return 2

    ser.setDTR(False)
    ser.setRTS(False)
    deadline = time.time() + seconds
    data = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            data.extend(chunk)
    ser.close()

    text = data.decode("utf-8", "replace")
    print(text[-9000:] if text else "NO_SERIAL_OUTPUT")

    failures = []
    if "task_wdt" in text:
        failures.append("task watchdog appeared in boot log")
    if "touchscreen UI task running" not in text:
        failures.append("UI task startup line not observed")
    if "Ganymede bridge up" not in text:
        failures.append("bridge startup line not observed")

    if failures:
        for failure in failures:
            print(f"BOOT LOG FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


def print_checklist() -> None:
    print("\nManual review checklist:")
    for item in REVIEW_CHECKLIST:
        print(f"- [ ] {item}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run local code assessment gates for touchscreen firmware changes."
    )
    parser.add_argument(
        "--idf-build",
        choices=("none", "touchscreen", "headless", "all"),
        default="none",
        help="Optionally run ESP-IDF builds after fast source gates.",
    )
    parser.add_argument("--port", help="Serial port for --boot-log, for example /dev/ttyUSB0.")
    parser.add_argument(
        "--boot-log",
        action="store_true",
        help="Capture boot log and fail on watchdog or missing startup lines.",
    )
    parser.add_argument(
        "--boot-seconds",
        type=int,
        default=10,
        help="Seconds to capture when --boot-log is used.",
    )
    parser.add_argument(
        "--no-checklist",
        action="store_true",
        help="Do not print the manual review checklist.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    os.environ.setdefault("PYTHONUNBUFFERED", "1")

    if not shutil.which("python3"):
        print("ERROR: python3 is required", file=sys.stderr)
        return 2

    failures = 0
    for gate in FAST_GATES:
        failures += 1 if run(gate) != 0 else 0

    if args.idf_build in ("touchscreen", "all"):
        failures += 1 if run_idf_build("touchscreen") != 0 else 0
    if args.idf_build in ("headless", "all"):
        failures += 1 if run_idf_build("headless") != 0 else 0

    if args.boot_log:
        if not args.port:
            print("ERROR: --boot-log requires --port", file=sys.stderr)
            failures += 1
        else:
            failures += 1 if capture_boot_log(args.port, args.boot_seconds) != 0 else 0

    if not args.no_checklist:
        print_checklist()

    if failures:
        print(f"\nAssessment failed: {failures} gate(s) failed.", file=sys.stderr)
        return 1

    print("\nAssessment automated gates passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
