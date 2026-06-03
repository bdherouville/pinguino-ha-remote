#!/usr/bin/env python3
"""Detect, connect, pair, and fully enumerate the Ganymede remote over BLE.

Outputs a JSON document describing every service/characteristic/descriptor,
the values of all readable characteristics (with decoders for known ones),
the HID Report Map, and then subscribes to HID input-report notifications so
physical button presses can be captured.

Usage:
  probe_ganymede.py [scan_timeout_s] [capture_s]

Press a button on the remote to make it advertise; the script connects as soon
as it sees the device, then during the capture window press each button you
want to record.
"""
import os
import asyncio
import datetime
import json
import sys

from bleak import BleakClient, BleakScanner

TARGET_ADDR = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
TARGET_NAME = "Ganymede"

# Known characteristic decoders -> (label, fn(bytes)->str)
def dec_str(b):
    return b.decode("utf-8", "replace")

def dec_u8(b):
    return str(b[0]) if b else "<empty>"

def dec_appearance(b):
    if len(b) >= 2:
        v = int.from_bytes(b[:2], "little")
        return f"0x{v:04X} ({v})"
    return "<short>"

def dec_temp(b):
    if len(b) >= 2:
        v = int.from_bytes(b[:2], "little", signed=True)
        return f"{v/100.0} C"
    return "<short>"

def dec_humidity(b):
    if len(b) >= 2:
        return f"{int.from_bytes(b[:2],'little')/100.0} %"
    return "<short>"

def dec_pressure(b):
    if len(b) >= 4:
        return f"{int.from_bytes(b[:4],'little')/10.0} Pa"
    return "<short>"

DECODERS = {
    "00002a00": ("Device Name", dec_str),
    "00002a01": ("Appearance", dec_appearance),
    "00002a19": ("Battery Level", dec_u8),
    "00002a6e": ("Temperature", dec_temp),
    "00002a6f": ("Humidity", dec_humidity),
    "00002a6d": ("Pressure", dec_pressure),
    "00002a4e": ("Protocol Mode", lambda b: b.hex()),
    "00002a4b": ("HID Report Map", lambda b: b.hex()),
    "00002a4a": ("HID Information", lambda b: b.hex()),
    "00002a50": ("PnP ID", lambda b: b.hex()),
    "00002a24": ("Model Number", dec_str),
    "00002a25": ("Serial Number", dec_str),
    "00002a26": ("Firmware Rev", dec_str),
    "00002a27": ("Hardware Rev", dec_str),
    "00002a29": ("Manufacturer", dec_str),
}

def short(uuid):
    u = uuid.lower()
    if u.endswith("-0000-1000-8000-00805f9b34fb"):
        return u.split("-")[0]
    return u


async def find(scan_timeout):
    print(f"Scanning up to {scan_timeout:.0f}s; PRESS A BUTTON on the remote...", flush=True)
    dev = await BleakScanner.find_device_by_address(TARGET_ADDR, timeout=scan_timeout)
    if dev is None:
        dev = await BleakScanner.find_device_by_name(TARGET_NAME, timeout=2)
    return dev


async def main(scan_timeout, capture_s, connect_timeout=35.0):
    report = {
        "target": TARGET_ADDR,
        "ts": datetime.datetime.now().isoformat(),
        "services": [],
        "reads": [],
        "notifications": [],
    }

    notif_records = []

    def on_notify(char, data: bytearray):
        rec = {
            "ts": datetime.datetime.now().isoformat(),
            "handle": getattr(char, "handle", None),
            "uuid": str(getattr(char, "uuid", char)),
            "hex": bytes(data).hex(),
            "len": len(data),
        }
        notif_records.append(rec)
        print(f"  NOTIFY h={rec['handle']} {rec['uuid'][:8]} -> {rec['hex']} ({rec['len']}B)", flush=True)

    # Phase A: scan ONCE to discover the device and populate BlueZ's cache, then
    # STOP scanning completely. Active scanning during the connection starves the
    # slow (sub-Hz) ATT traffic and makes BlueZ abort service discovery.
    print(">>> Phase A: press the button to make it advertise...", flush=True)
    dev = await BleakScanner.find_device_by_address(TARGET_ADDR, timeout=scan_timeout)
    if dev is None:
        print("Device not found.")
        return 2
    print(f"Discovered {dev.address}. Scanner stopped.", flush=True)
    await asyncio.sleep(1.0)  # let BlueZ fully tear down the discovery session

    # Phase B+C: connect with the slow per-device params already loaded into the
    # kernel. BlueZ will grab the next advert window; KEEP PRESSING to (a) make it
    # advertise so the connect completes and (b) keep the remote awake long enough
    # for the ~0.3-0.6s-interval GATT service discovery to finish.
    print(">>> Phase B: KEEP PRESSING the button continuously until 'Connected=True' <<<", flush=True)
    client = BleakClient(dev, timeout=connect_timeout, services=None)
    last_err = None
    for attempt in range(1, 4):
        try:
            await client.connect()
            break
        except Exception as e:
            last_err = e
            print(f"  connect attempt {attempt} failed: {e!r}; keep pressing...", flush=True)
            try:
                await client.disconnect()
            except Exception:
                pass
    if not client.is_connected:
        print(f"Could not connect: {last_err}")
        return 3
    try:
        print(f"Connected={client.is_connected}", flush=True)
        try:
            paired = await client.pair()
            print(f"pair() -> {paired}", flush=True)
            report["paired"] = bool(paired)
        except Exception as e:
            print(f"pair() failed: {e}", flush=True)
            report["paired"] = f"error: {e}"

        # Enumerate GATT
        for service in client.services:
            svc = {"uuid": short(service.uuid), "handle": service.handle,
                   "description": service.description, "chars": []}
            for ch in service.characteristics:
                c = {"uuid": short(ch.uuid), "handle": ch.handle,
                     "properties": ch.properties, "description": ch.description,
                     "descriptors": [{"uuid": short(d.uuid), "handle": d.handle,
                                      "description": d.description} for d in ch.descriptors]}
                svc["chars"].append(c)
            report["services"].append(svc)

        print("\n=== GATT TABLE ===", flush=True)
        for svc in report["services"]:
            print(f"Service {svc['uuid']} ({svc['description']}) h={svc['handle']}")
            for c in svc["chars"]:
                print(f"  Char {c['uuid']} h={c['handle']} props={c['properties']} ({c['description']})")
                for d in c["descriptors"]:
                    print(f"    Desc {d['uuid']} h={d['handle']} ({d['description']})")

        # Read all readable characteristics
        print("\n=== READS ===", flush=True)
        for service in client.services:
            for ch in service.characteristics:
                if "read" not in ch.properties:
                    continue
                try:
                    val = await client.read_gatt_char(ch)
                    label, fn = DECODERS.get(short(ch.uuid), (ch.description or "", None))
                    decoded = fn(bytes(val)) if fn else None
                    rec = {"uuid": short(ch.uuid), "handle": ch.handle,
                           "label": label, "hex": bytes(val).hex(),
                           "len": len(val), "decoded": decoded}
                    report["reads"].append(rec)
                    extra = f"  [{decoded}]" if decoded is not None else ""
                    print(f"  {short(ch.uuid)} h={ch.handle} ({label}) = {bytes(val).hex()} ({len(val)}B){extra}", flush=True)
                except Exception as e:
                    print(f"  {short(ch.uuid)} h={ch.handle} READ ERROR: {e}", flush=True)
                    report["reads"].append({"uuid": short(ch.uuid), "handle": ch.handle, "error": str(e)})

        # Subscribe to all notify/indicate characteristics (HID input reports, env, battery)
        print("\n=== SUBSCRIBING to notify/indicate chars ===", flush=True)
        subscribed = []
        for service in client.services:
            for ch in service.characteristics:
                if "notify" in ch.properties or "indicate" in ch.properties:
                    try:
                        await client.start_notify(ch, on_notify)
                        subscribed.append((short(ch.uuid), ch.handle))
                        print(f"  subscribed {short(ch.uuid)} h={ch.handle} {ch.properties}", flush=True)
                    except Exception as e:
                        print(f"  subscribe FAIL {short(ch.uuid)} h={ch.handle}: {e}", flush=True)

        print(f"\n=== CAPTURING {capture_s:.0f}s — PRESS EACH BUTTON on the remote now ===", flush=True)
        await asyncio.sleep(capture_s)

        for uuid, handle in subscribed:
            try:
                await client.stop_notify(handle)
            except Exception:
                pass

        report["notifications"] = notif_records
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    out = "tools/linux-ble/ganymede_probe.json"
    with open(out, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nWrote {out}  ({len(notif_records)} notifications captured)")
    return 0


if __name__ == "__main__":
    st = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    cap = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    sys.exit(asyncio.run(main(st, cap)))
