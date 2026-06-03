#!/usr/bin/env python3
"""Observe how the remote advertises while held in PAIRING MODE.

Logs every advert from the known address AND any device named 'Ganymede'
(in case pairing mode uses a different/random address), printing full payload
so we can see new flags, the HID service (0x1812), or a directed advert.
"""
import os
import asyncio
import sys
from bleak import BleakScanner

TARGET_ADDR = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
TARGET_NAME = "Ganymede"
seen = {}


async def main(timeout_s):
    def cb(d, adv):
        name = adv.local_name or d.name or ""
        is_target = d.address.upper() == TARGET_ADDR or TARGET_NAME.lower() in name.lower()
        if not is_target:
            return
        key = (d.address, tuple(sorted(adv.service_uuids)), adv.local_name,
               tuple(sorted(adv.manufacturer_data.items())))
        if key in seen:
            return
        seen[key] = True
        print(f"\n[{d.address}] rssi={adv.rssi} name={adv.local_name!r}")
        print(f"  flags/appearance : appearance={getattr(adv,'appearance',None)}")
        print(f"  service_uuids    : {adv.service_uuids}")
        print(f"  service_data     : { {k: v.hex() for k,v in adv.service_data.items()} }")
        print(f"  manufacturer     : { {k: v.hex() for k,v in adv.manufacturer_data.items()} }", flush=True)

    s = BleakScanner(detection_callback=cb)
    print(f"Scanning {timeout_s:.0f}s. HOLD the remote in PAIRING MODE...", flush=True)
    await s.start()
    await asyncio.sleep(timeout_s)
    await s.stop()
    print(f"\nDistinct advert variants seen: {len(seen)}")
    return 0 if seen else 2


if __name__ == "__main__":
    t = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    sys.exit(asyncio.run(main(t)))
