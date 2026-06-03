#!/usr/bin/env python3
"""Patiently connect+bond to the real Ganymede remote and dump its live GATT.

The remote is a flaky peripheral: most connection attempts form the link but the
peer never services it (drops with 0x3e). Occasionally one holds long enough to
bond + discover services (this is how Android eventually reads it). This tool is
event-driven and PERSISTENT: it arms a connection, re-arms on every drop, bonds
when connected, and the moment services resolve it reads the HID Report Map and
subscribes to button-report notifications.

Run with system python3 (dbus + gi). Hold/repeatedly press the remote button the
whole time. Default run 240s.

  python3 detect_patient.py [seconds]
"""
import os
import json
import sys
import time
import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
GATT_SVC_IFACE = "org.bluez.GattService1"
GATT_CHR_IFACE = "org.bluez.GattCharacteristic1"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"
PROP_IFACE = "org.freedesktop.DBus.Properties"
AGENT_PATH = "/delonghi/agent2"
TARGET = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
ADAPTER = "hci0"
DEV_PATH = f"/org/bluez/{ADAPTER}/dev_" + TARGET.replace(":", "_")

bus = None
mainloop = None
state = {
    "armed": False, "pairing": False, "dumped": False,
    "conn_attempts": 0, "ever_connected": False, "ever_paired": False,
    "report": {"target": TARGET, "services": [], "reads": [], "notifications": []},
}


class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid): return
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device): return "0000"
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, device): return dbus.UInt32(0)
    @dbus.service.method("org.bluez.Agent1", in_signature="ouq", out_signature="")
    def DisplayPasskey(self, device, passkey, entered): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def DisplayPinCode(self, device, pincode): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        print(f"[agent] auto-confirm passkey {passkey:06d}")
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device): print("[agent] auto-authorize")
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self): pass


def om_objects():
    om = dbus.Interface(bus.get_object(BLUEZ, "/"), OM_IFACE)
    return om.GetManagedObjects()


def dev_iface():
    return dbus.Interface(bus.get_object(BLUEZ, DEV_PATH), DEVICE_IFACE)


def dev_props():
    return dbus.Interface(bus.get_object(BLUEZ, DEV_PATH), PROP_IFACE)


def get_prop(name, default=None):
    try:
        return dev_props().Get(DEVICE_IFACE, name)
    except Exception:
        return default


def short(uuid):
    u = str(uuid).lower()
    return u.split("-")[0] if u.endswith("-0000-1000-8000-00805f9b34fb") else u


def adapter_iface():
    return dbus.Interface(bus.get_object(BLUEZ, f"/org/bluez/{ADAPTER}"), ADAPTER_IFACE)


def arm_connect():
    """Persistently call Pair() (atomic connect+bond) until one link holds.

    Critical: active discovery must be STOPPED before connecting — concurrent
    active scanning starves the connection's first LL events and makes every
    establishment fail with 0x3e (Android held ~40% by NOT scanning while
    connecting; with the scan running we held 0%)."""
    if state["dumped"] or state["armed"]:
        return
    objs = om_objects()
    if DEV_PATH not in objs:
        # Need the device object; (re)start a brief discovery only when absent.
        if not state.get("discovering"):
            try:
                adapter_iface().StartDiscovery()
                state["discovering"] = True
            except Exception:
                pass
        return
    # Device known — make sure active discovery is OFF before we connect.
    if state.get("discovering"):
        try:
            adapter_iface().StopDiscovery()
        except Exception:
            pass
        state["discovering"] = False
        print("  [discovery stopped — connecting without active scan]", flush=True)
    if get_prop("Connected", False):
        return
    bonded = bool(get_prop("Paired", False))
    state["armed"] = True
    state["conn_attempts"] += 1
    n = state["conn_attempts"]
    verb = "connect" if bonded else "pair"
    print(f"[{time.strftime('%H:%M:%S')}] {verb} attempt #{n} (keep pressing)...", flush=True)
    try:
        dev_props().Set(DEVICE_IFACE, "Trusted", True)
    except Exception:
        pass

    def ok():
        state["armed"] = False
        state["ever_paired"] = True
        print(f"  #{n} {verb} OK", flush=True)

    def err(e):
        state["armed"] = False
        print(f"  #{n} {verb} failed: {str(e).split(':')[-1].strip()}", flush=True)

    try:
        # Once bonded, just Connect() (encryption auto-resumes from the LTK);
        # before that, Pair() to establish the bond.
        if bonded:
            dev_iface().Connect(reply_handler=ok, error_handler=err, timeout=40)
        else:
            dev_iface().Pair(reply_handler=ok, error_handler=err, timeout=40)
    except Exception as e:
        state["armed"] = False
        print(f"  #{n} {verb} raise: {e}", flush=True)


def on_notify_props(iface, changed, invalidated, path=None):
    if path != DEV_PATH or iface != DEVICE_IFACE:
        return
    if "Connected" in changed:
        c = bool(changed["Connected"])
        print(f"  >> Connected={c}", flush=True)
        if c:
            state["ever_connected"] = True
        else:
            GLib.timeout_add(400, lambda: (arm_connect(), False)[1])
    if "Paired" in changed and changed["Paired"]:
        state["ever_paired"] = True
        print("  >> Paired=True", flush=True)
    if "ServicesResolved" in changed:
        print(f"  >> ServicesResolved={bool(changed['ServicesResolved'])}", flush=True)
        if changed["ServicesResolved"] and not state["dumped"]:
            GLib.timeout_add(200, lambda: (dump_and_capture(), False)[1])


def read_char(path, uuid):
    try:
        c = dbus.Interface(bus.get_object(BLUEZ, path), GATT_CHR_IFACE)
        val = bytes(c.ReadValue({}))
        return val
    except Exception as e:
        print(f"    read {short(uuid)} err: {e}", flush=True)
        return None


def on_report_notify(iface, changed, invalidated, path=None):
    if iface != GATT_CHR_IFACE or "Value" not in changed:
        return
    val = bytes(changed["Value"])
    rec = {"ts": time.strftime("%H:%M:%S"), "path": path, "hex": val.hex(), "len": len(val)}
    state["report"]["notifications"].append(rec)
    print(f"    BUTTON NOTIFY {path.split('/')[-1]} -> {val.hex()} ({len(val)}B)", flush=True)


def dump_and_capture():
    if state["dumped"]:
        return
    state["dumped"] = True
    print("\n=== SERVICES RESOLVED — dumping GATT ===", flush=True)
    objs = om_objects()
    svcs = {p: i[GATT_SVC_IFACE] for p, i in objs.items()
            if GATT_SVC_IFACE in i and p.startswith(DEV_PATH)}
    report_chars = []
    for sp, sprops in sorted(svcs.items()):
        suuid = short(sprops.get("UUID"))
        svc_rec = {"uuid": suuid, "chars": []}
        print(f"Service {suuid}", flush=True)
        for cp, ci in sorted(objs.items()):
            c = ci.get(GATT_CHR_IFACE)
            if not c or not cp.startswith(sp):
                continue
            cuuid = short(c.get("UUID"))
            flags = list(c.get("Flags", []))
            crec = {"uuid": cuuid, "flags": flags, "path": cp}
            svc_rec["chars"].append(crec)
            print(f"  Char {cuuid} flags={flags}", flush=True)
            if "read" in flags:
                v = read_char(cp, cuuid)
                if v is not None:
                    label = ""
                    if cuuid == "00002a4b":
                        label = "HID_REPORT_MAP"
                    elif cuuid == "00002a00":
                        label = "name=" + v.decode("utf-8", "replace")
                    print(f"      = {v.hex()}  {label}", flush=True)
                    state["report"]["reads"].append({"uuid": cuuid, "hex": v.hex(), "label": label})
            if cuuid == "00002a4d" or cuuid == "00002a22":
                report_chars.append(cp)
        state["report"]["services"].append(svc_rec)

    # Subscribe to HID input report notifications to capture live button presses.
    for cp in report_chars:
        try:
            bus.add_signal_receiver(
                on_report_notify, dbus_interface=PROP_IFACE,
                signal_name="PropertiesChanged", path=cp, path_keyword="path")
            dbus.Interface(bus.get_object(BLUEZ, cp), GATT_CHR_IFACE).StartNotify()
            print(f"  subscribed to reports on {cp.split('/')[-1]}", flush=True)
        except Exception as e:
            print(f"  subscribe err {cp}: {e}", flush=True)

    print("\n=== CONNECTED — now PRESS EACH BUTTON to capture reports (40s) ===", flush=True)
    GLib.timeout_add(40000, lambda: (finish(), False)[1])


def finish():
    out = "tools/linux-ble/ganymede_live_gatt.json"
    try:
        with open(out, "w") as f:
            json.dump(state["report"], f, indent=2)
        print(f"\nWrote {out}", flush=True)
    except Exception as e:
        print(f"write err: {e}", flush=True)
    mainloop.quit()


def tick():
    if state["dumped"]:
        return True
    arm_connect()
    return True


def main(duration):
    global bus, mainloop
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    mainloop = GLib.MainLoop()

    Agent(bus, AGENT_PATH)
    am = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.AgentManager1")
    try:
        am.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
        am.RequestDefaultAgent(AGENT_PATH)
    except dbus.DBusException as e:
        print(f"[agent] {e}")

    ad = dbus.Interface(bus.get_object(BLUEZ, f"/org/bluez/{ADAPTER}"), ADAPTER_IFACE)
    try:
        ad.SetDiscoveryFilter({"Transport": "le"})
        ad.StartDiscovery()
        state["discovering"] = True
    except Exception as e:
        print(f"discovery: {e}")

    bus.add_signal_receiver(
        on_notify_props, dbus_interface=PROP_IFACE, signal_name="PropertiesChanged",
        arg0=DEVICE_IFACE, path_keyword="path")

    print(f">>> HOLD / repeatedly press the remote button for up to {duration:.0f}s <<<", flush=True)
    GLib.timeout_add(2000, tick)
    GLib.timeout_add(int(duration * 1000), lambda: (print("\n[overall timeout]"), finish())[1] if not state["dumped"] else False)
    mainloop.run()

    try:
        ad.StopDiscovery()
    except Exception:
        pass
    print(f"\nSummary: attempts={state['conn_attempts']} ever_connected={state['ever_connected']} "
          f"ever_paired={state['ever_paired']} services_dumped={state['dumped']} "
          f"notifications={len(state['report']['notifications'])}")
    return 0 if state["dumped"] else 4


if __name__ == "__main__":
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
    sys.exit(main(dur))
