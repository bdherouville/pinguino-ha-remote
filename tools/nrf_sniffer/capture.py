#!/usr/bin/env python3
"""Capture BLE traffic from the nRF Sniffer to a Nordic-BLE .pcap (Wireshark-openable).

Why this exists instead of the stock extcap: the extcap's no-Wireshark-control-pipe
path busy-waits with `while True: pass`, which starves the SnifferAPI reader thread
(GIL) and writes ~0 packets. This uses the same SnifferAPI + Pcap encoders but yields
the GIL with time.sleep, so packets actually flow.

Usage:
    python3 capture.py [out.pcap] [seconds] [--port /dev/ttyACMx] [--follow AA:BB:CC:DD:EE:FF]
Defaults: out=capture_<unix>.pcap (pass a path), 20 s, port auto-detected by /dev/serial/by-id.
--follow locks onto a device so the sniffer hops WITH its connection (needed to capture a
CONNECT_IND + SMP/ATT, not just advertising). Open in Wireshark (DLT NORDIC_BLE 272) or: tshark -r out.pcap
"""
import glob, os, sys, time

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "extcap")
sys.path.insert(0, HERE)

from SnifferAPI import Filelock
Filelock.lock = lambda *a, **k: None      # /var/lock may be root-only; we control access
Filelock.unlock = lambda *a, **k: None
from SnifferAPI import Sniffer, Pcap


def find_port():
    for link in glob.glob("/dev/serial/by-id/*"):
        if "Sniffer" in link or "Sniffer".lower() in link.lower():
            return os.path.realpath(link)
    raise SystemExit("no nRF Sniffer tty found under /dev/serial/by-id (is it flashed/plugged?)")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--port"), None) or find_port()
    out = args[0] if args else "capture_%d.pcap" % int(time.time())
    dur = float(args[1]) if len(args) > 1 else 20.0

    fh = open(out, "wb", 0)
    fh.write(Pcap.get_global_header())
    n = 0

    def on_pkt(note):
        nonlocal n
        pkt = note.msg["packet"]
        fh.write(Pcap.create_packet(bytes([pkt.boardId] + pkt.getList()), pkt.time))
        n += 1

    # Optional follow (lock onto a device so the sniffer hops WITH its connection — needed to
    # capture CONNECT_IND + SMP/ATT, not just advertising):
    #   --follow AA:BB:CC:DD:EE:FF   by address
    #   --follow-name Ganymede      by advertised name
    follow_arg = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--follow"), None)
    follow_name = next((sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == "--follow-name"), None)
    target = [int(b, 16) for b in follow_arg.split(":")] if follow_arg else None

    def _name(d):
        n = getattr(d, "name", "") or ""
        return n.decode(errors="replace") if isinstance(n, (bytes, bytearray)) else str(n)

    s = Sniffer.Sniffer(portnum=port, baudrate=1000000)
    s.subscribe("NEW_BLE_PACKET", on_pkt)
    s.setAdvHopSequence([37, 38, 39])
    s.start(); time.sleep(0.5)
    s.scan()                                  # all advertising
    want = follow_arg or follow_name
    print("capturing %.0fs on %s -> %s%s" % (dur, port, out, " (following %s)" % want if want else ""))
    t0 = time.time(); following = not (target or follow_name)
    try:
        while time.time() - t0 < dur:
            if not following:
                for d in s.getDevices().asList():
                    hit = (target and list(d.address)[:6] == target) or \
                          (follow_name and _name(d) == follow_name)
                    if hit:
                        s.follow(d); following = True
                        print("following %s @ %.1fs — trigger the connection now" % (want, time.time() - t0))
                        break
            time.sleep(0.3)
    except KeyboardInterrupt:
        pass
    s.doExit(); fh.close()
    print("wrote %d packets%s" % (n, "" if following else " (target never seen advertising)"))


if __name__ == "__main__":
    main()
