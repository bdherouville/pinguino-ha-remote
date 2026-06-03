#!/usr/bin/env python3
"""Synchronized signal-behavior test: does the ESP32 scan suppress the remote?

Linux scans continuously and logs every sighting of the real remote (time, RSSI).
Meanwhile we toggle the ESP32's scan ON/OFF at known times over the serial CLI
(NO connecting). If the remote's Linux sightings collapse while the ESP32 is
scanning and recover when it stops, the ESP32 scan is interfering over the air.
Watch Android/nRF in parallel and note when it goes blind.

Timeline (35s): [0-10s ESP idle] -> ESP scan ON -> [10-25s ESP scanning] -> ESP off -> [25-35s idle]
"""
import os
import asyncio
import time
import serial
from bleak import BleakScanner

TARGET = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
PORT = "/dev/ttyACM2"
sightings = []
t0 = None


def cb(d, a):
    if d.address.upper() == TARGET:
        sightings.append((time.time() - t0, a.rssi))


def open_esp():
    s = serial.Serial()
    s.port = PORT; s.baudrate = 115200; s.dtr = False; s.rts = False; s.timeout = 0.3
    s.open(); time.sleep(0.3)
    return s


async def main():
    global t0
    esp = open_esp()
    def cmd(c):
        esp.write((c + "\n").encode()); esp.flush()
    cmd("stop"); time.sleep(0.6)        # ensure ESP not scanning
    print(">>> PRESS the remote CONTINUOUSLY for the full 35s; watch Android <<<", flush=True)
    t0 = time.time()
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(10)             # phase 1: ESP idle
    print(f"[{time.time()-t0:4.1f}s] ESP32 SCAN -> ON", flush=True)
    cmd("scan")
    await asyncio.sleep(15)             # phase 2: ESP scanning
    print(f"[{time.time()-t0:4.1f}s] ESP32 SCAN -> OFF", flush=True)
    cmd("stop")
    await asyncio.sleep(10)             # phase 3: ESP idle
    await scanner.stop()
    esp.close()

    def rng(lo, hi):
        return [r for t, r in sightings if lo <= t < hi]
    p1, p2, p3 = rng(0, 10), rng(10, 25), rng(25, 35)
    print("\n=== RESULT: Linux sightings of the remote ===")
    print(f"  Phase 1  ESP idle    (0-10s) : {len(p1):3d} sightings  rssi={p1}")
    print(f"  Phase 2  ESP SCANNING(10-25s): {len(p2):3d} sightings  rssi={p2}")
    print(f"  Phase 3  ESP idle   (25-35s) : {len(p3):3d} sightings  rssi={p3}")
    r1 = len(p1)/10.0; r2 = len(p2)/15.0; r3 = len(p3)/10.0
    print(f"  rates/s: idle={r1:.2f}  scanning={r2:.2f}  idle={r3:.2f}")
    if (r1 + r3) > 0 and r2 < 0.3 * ((r1 + r3) / 2 + 1e-9):
        print("  => SUPPRESSION CONFIRMED: remote nearly vanishes while ESP32 scans.")
    else:
        print("  => no clear suppression (remote seen similarly across phases).")


if __name__ == "__main__":
    asyncio.run(main())
