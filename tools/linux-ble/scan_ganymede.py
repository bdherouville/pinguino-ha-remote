#!/usr/bin/env python3
"""Continuously scan for the Ganymede remote and dump full advertisement data.

The remote only advertises for ~3s after a physical button press, so keep this
running and press a button on the remote. Exits 0 as soon as it is seen.
"""
import os
import asyncio
import sys
from bleak import BleakScanner

TARGET_ADDR = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
TARGET_NAME = "Ganymede"


def matches(device, adv):
    if device.address.upper() == TARGET_ADDR:
        return True
    name = (adv.local_name or device.name or "")
    return TARGET_NAME.lower() in name.lower()


async def main(timeout_s: float):
    found = asyncio.Event()
    result = {}

    def cb(device, adv):
        if not matches(device, adv):
            return
        result["device"] = device
        result["adv"] = adv
        if not found.is_set():
            print(f"\n=== FOUND {device.address}  rssi={adv.rssi} ===")
            print(f"  local_name      : {adv.local_name!r}")
            print(f"  tx_power        : {adv.tx_power}")
            print(f"  appearance      : {getattr(adv, 'appearance', None)}")
            print(f"  service_uuids   : {adv.service_uuids}")
            print(f"  service_data    : { {k: v.hex() for k, v in adv.service_data.items()} }")
            print(f"  manufacturer    : { {k: v.hex() for k, v in adv.manufacturer_data.items()} }")
            print(f"  rssi            : {adv.rssi}")
            found.set()

    scanner = BleakScanner(detection_callback=cb)
    print(f"Scanning up to {timeout_s:.0f}s for {TARGET_NAME} ({TARGET_ADDR})...")
    print(">>> PRESS A BUTTON on the remote now <<<", flush=True)
    await scanner.start()
    try:
        await asyncio.wait_for(found.wait(), timeout=timeout_s)
    except asyncio.TimeoutError:
        print("Timed out, device not seen.")
    finally:
        await scanner.stop()
    return 0 if found.is_set() else 2


if __name__ == "__main__":
    t = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    sys.exit(asyncio.run(main(t)))
