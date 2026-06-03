#!/usr/bin/env python3
"""Bond with Ganymede via BlueZ D-Bus directly (system python: dbus + GLib).

Registers a NoInputNoOutput "Just Works" pairing agent, discovers the device,
calls Device1.Pair() (the correct bonding primitive — returns on bond complete,
not on service resolution), then dumps the resolved GATT including the HID
Report Map. Designed for the sleepy remote held in PAIRING MODE.

Run with system python3 (has gi + dbus.mainloop.glib). Needs the BlueZ daemon
running (uses the system bus; pairing agent requires no root, but reading some
fields may).
"""
import os
import sys
import dbus
import dbus.service
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
GATT_SVC_IFACE = "org.bluez.GattService1"
GATT_CHR_IFACE = "org.bluez.GattCharacteristic1"
GATT_DSC_IFACE = "org.bluez.GattDescriptor1"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"
PROP_IFACE = "org.freedesktop.DBus.Properties"
AGENT_PATH = "/delonghi/agent"
TARGET = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
ADAPTER = "hci0"

bus = None
mainloop = None


class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        print("[agent] Release")

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        print(f"[agent] AuthorizeService {uuid} -> allow")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        print("[agent] RequestPinCode -> 0000")
        return "0000"

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        print("[agent] RequestPasskey -> 0")
        return dbus.UInt32(0)

    @dbus.service.method("org.bluez.Agent1", in_signature="ouq", out_signature="")
    def DisplayPasskey(self, device, passkey, entered):
        print(f"[agent] DisplayPasskey {passkey:06d} entered={entered}")

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def DisplayPinCode(self, device, pincode):
        print(f"[agent] DisplayPinCode {pincode}")

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        print(f"[agent] RequestConfirmation {passkey:06d} -> confirm")

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        print("[agent] RequestAuthorization -> allow")

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        print("[agent] Cancel")


def get_managed():
    om = dbus.Interface(bus.get_object(BLUEZ, "/"), OM_IFACE)
    return om.GetManagedObjects()


def find_device_path():
    for path, ifaces in get_managed().items():
        d = ifaces.get(DEVICE_IFACE)
        if d and str(d.get("Address", "")).upper() == TARGET:
            return path
    return None


def decode(uuid, val):
    b = bytes(val)
    u = uuid.lower()[:8]
    if u == "00002a4b":
        return "HID_REPORT_MAP " + b.hex()
    if u == "00002a00":
        return b.decode("utf-8", "replace")
    if u == "00002a19" and b:
        return f"battery {b[0]}%"
    return b.hex()


def dump_gatt(dev_path):
    objs = get_managed()
    print("\n=== GATT (from BlueZ cache) ===")
    svcs = {p: i[GATT_SVC_IFACE] for p, i in objs.items()
            if GATT_SVC_IFACE in i and p.startswith(dev_path)}
    for sp, sprops in sorted(svcs.items()):
        print(f"Service {sprops.get('UUID')}  {sp.split('/')[-1]}")
        for cp, ci in sorted(objs.items()):
            c = ci.get(GATT_CHR_IFACE)
            if not c or not cp.startswith(sp):
                continue
            flags = list(c.get("Flags", []))
            print(f"  Char {c.get('UUID')} flags={flags} {cp.split('/')[-1]}")
            if "read" in flags:
                try:
                    chr_obj = dbus.Interface(bus.get_object(BLUEZ, cp), GATT_CHR_IFACE)
                    val = chr_obj.ReadValue({})
                    print(f"      value = {decode(str(c.get('UUID')), val)}")
                except Exception as e:
                    print(f"      read error: {e}")


def main():
    global bus, mainloop
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    mainloop = GLib.MainLoop()

    # Register Just Works agent.
    Agent(bus, AGENT_PATH)
    am = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.AgentManager1")
    try:
        am.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
        am.RequestDefaultAgent(AGENT_PATH)
        print("[agent] registered NoInputNoOutput default agent")
    except dbus.DBusException as e:
        print(f"[agent] register note: {e}")

    adapter_path = f"/org/bluez/{ADAPTER}"
    adapter = dbus.Interface(bus.get_object(BLUEZ, adapter_path), ADAPTER_IFACE)
    adapter_props = dbus.Interface(bus.get_object(BLUEZ, adapter_path), PROP_IFACE)

    # Fresh LE discovery.
    try:
        adapter.SetDiscoveryFilter({"Transport": "le"})
    except Exception as e:
        print(f"filter note: {e}")
    try:
        adapter.StartDiscovery()
    except Exception as e:
        print(f"StartDiscovery note: {e}")

    print(">>> HOLD the remote in PAIRING MODE now <<<")
    print("Discovering device (up to 40s)...")

    state = {"path": None, "done": False, "rc": 3}

    def poll_discovery():
        p = find_device_path()
        if p:
            state["path"] = p
            print(f"Found device object: {p}")
            try:
                adapter.StopDiscovery()
            except Exception:
                pass
            GLib.timeout_add(500, start_pair)
            return False  # stop polling
        return True

    def start_pair():
        path = state["path"]
        dev = dbus.Interface(bus.get_object(BLUEZ, path), DEVICE_IFACE)
        props = dbus.Interface(bus.get_object(BLUEZ, path), PROP_IFACE)
        try:
            dev.Trusted = True
        except Exception:
            pass
        try:
            props.Set(DEVICE_IFACE, "Trusted", True)
        except Exception:
            pass
        print("Calling Pair() ... (stay in pairing mode)")

        def on_ok():
            print("PAIR OK")
            try:
                paired = props.Get(DEVICE_IFACE, "Paired")
                sr = props.Get(DEVICE_IFACE, "ServicesResolved")
                print(f"Paired={paired} ServicesResolved={sr}")
            except Exception as e:
                print(f"prop read: {e}")
            GLib.timeout_add(3000, finish)

        def on_err(e):
            print(f"PAIR ERROR: {e}")
            # Even if Pair reports error, try connecting to trigger discovery.
            try:
                dev.Connect()
            except Exception as ce:
                print(f"Connect after pair-err: {ce}")
            GLib.timeout_add(3000, finish)

        dev.Pair(reply_handler=on_ok, error_handler=on_err, timeout=60)
        return False

    def finish():
        if state["path"]:
            try:
                dump_gatt(state["path"])
                state["rc"] = 0
            except Exception as e:
                print(f"dump error: {e}")
        mainloop.quit()
        return False

    GLib.timeout_add(800, poll_discovery)
    GLib.timeout_add(75000, lambda: mainloop.quit())
    mainloop.run()

    try:
        adapter.StopDiscovery()
    except Exception:
        pass
    return state["rc"]


if __name__ == "__main__":
    sys.exit(main())
