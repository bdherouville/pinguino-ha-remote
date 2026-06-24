#!/usr/bin/env python3
"""Capture touchscreen serial diagnostics.

Use this after flashing a build with CONFIG_PINGUINO_TOUCHSCREEN_SERIAL_DIAGNOSTIC=y.
It prints all boot/log output and marks diagnostic lines so freezes can be
correlated with touch, tileview and UI heartbeat state.
"""

from __future__ import annotations

import argparse
import sys
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Monitor touchscreen serial diagnostics.")
    parser.add_argument("-p", "--port", default="/dev/ttyUSB0", help="Serial port.")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument("-s", "--seconds", type=int, default=120, help="Capture duration.")
    parser.add_argument(
        "--diag-only",
        action="store_true",
        help="Only print lines containing touchscreen diagnostic markers.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError:
        print("pyserial is required: python3 -m pip install pyserial", file=sys.stderr)
        return 2

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:
        print(f"could not open {args.port}: {exc}", file=sys.stderr)
        return 2

    ser.setDTR(False)
    ser.setRTS(False)
    deadline = time.time() + args.seconds
    buffer = bytearray()
    print(f"monitoring {args.port} for {args.seconds}s; reproduce the freeze now")
    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buffer.extend(chunk)
            while b"\n" in buffer:
                raw, _, buffer = buffer.partition(b"\n")
                line = raw.decode("utf-8", "replace").rstrip("\r")
                is_diag = " diag " in line or "touch down" in line or "touch move" in line or "touch up" in line
                if args.diag_only and not is_diag:
                    continue
                prefix = ">>> " if is_diag else ""
                print(prefix + line, flush=True)
    finally:
        ser.close()

    if buffer and not args.diag_only:
        print(buffer.decode("utf-8", "replace"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
