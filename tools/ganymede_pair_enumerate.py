#!/usr/bin/env python3
"""Pair with and enumerate a Ganymede BLE remote through BlueZ bluetoothctl.

The script intentionally keeps raw command transcripts beside structured output
so every decoded detail can be traced back to a capture artifact.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import pexpect


PROMPT = r"(?:#|>)\s*"
MAC_RE = re.compile(r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b")
DEVICE_LINE_RE = re.compile(
    r"(?:\x1b\[[0-9;]*m)*\s*(?:\[[A-Z]+\]\s*)?Device\s+"
    r"(?P<mac>[0-9A-Fa-f:]{17})\s*(?P<name>.*)"
)
ATTRIBUTE_RE = re.compile(
    r"(?P<kind>Primary Service|Secondary Service|Characteristic|Descriptor)"
    r"\s+(?P<uuid>[0-9A-Fa-f-]{4,36})"
    r"(?:\s+\((?P<label>[^)]*)\))?"
    r".*?handle:\s*(?P<handle>0x[0-9A-Fa-f]+)",
    re.IGNORECASE,
)


def now_slug() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", text)


class BluetoothCtl:
    def __init__(self, transcript_path: Path, timeout: int = 20) -> None:
        self.transcript_path = transcript_path
        self.transcript = transcript_path.open("w", encoding="utf-8")
        self.child = pexpect.spawn(
            "bluetoothctl",
            encoding="utf-8",
            timeout=timeout,
            echo=False,
        )
        self.child.logfile_read = self.transcript
        self.child.expect(PROMPT)

    def close(self) -> None:
        try:
            self.cmd("quit", timeout=3)
        except Exception:
            pass
        try:
            self.child.close(force=True)
        finally:
            self.transcript.close()

    def cmd(self, command: str, timeout: int = 20) -> str:
        self.child.sendline(command)
        self.child.expect(PROMPT, timeout=timeout)
        return strip_ansi(self.child.before)

    def send_and_wait(
        self,
        command: str,
        timeout: int = 45,
        passkey: str | None = None,
        confirm: bool = True,
    ) -> str:
        self.child.sendline(command)
        chunks: list[str] = []
        deadline = time.monotonic() + timeout
        while True:
            remaining = max(1, int(deadline - time.monotonic()))
            if remaining <= 1 and time.monotonic() >= deadline:
                raise TimeoutError(f"timed out waiting for bluetoothctl after {command!r}")
            index = self.child.expect(
                [
                    PROMPT,
                    r"Confirm passkey .* \(yes/no\):",
                    r"Authorize service .* \(yes/no\):",
                    r"Request passkey",
                    r"Enter passkey.*:",
                    r"Request PIN code",
                    r"Enter PIN code.*:",
                    pexpect.TIMEOUT,
                ],
                timeout=remaining,
            )
            chunks.append(strip_ansi(self.child.before))
            if index == 0:
                return "".join(chunks)
            if index in (1, 2):
                self.child.sendline("yes" if confirm else "no")
                chunks.append(f"\n[AUTO-REPLY] {'yes' if confirm else 'no'}\n")
            elif index in (3, 4, 5, 6):
                if passkey is None:
                    raise RuntimeError(
                        "bluetoothctl requested a passkey/PIN; rerun with --passkey"
                    )
                self.child.sendline(passkey)
                chunks.append("\n[AUTO-REPLY] <redacted passkey>\n")
            else:
                chunks.append(strip_ansi(self.child.before))

    def scan(self, seconds: int) -> str:
        self.child.sendline("scan on")
        time.sleep(seconds)
        self.child.sendline("scan off")
        self.child.expect(PROMPT, timeout=20)
        return strip_ansi(self.child.before)


def start_btmon(raw_dir: Path, slug: str) -> tuple[subprocess.Popen[str] | None, Path | None]:
    btmon = shutil.which("btmon")
    if not btmon:
        return None, None
    path = raw_dir / f"ganymede_btmon_{slug}.snoop"
    proc = subprocess.Popen(
        [btmon, "-w", str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    time.sleep(0.5)
    if proc.poll() is not None:
        return None, path
    return proc, path


def stop_process(proc: subprocess.Popen[str] | None) -> None:
    if proc is None:
        return
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def parse_devices(text: str) -> list[dict[str, str]]:
    devices: list[dict[str, str]] = []
    for line in strip_ansi(text).splitlines():
        match = DEVICE_LINE_RE.search(line)
        if match:
            devices.append(
                {
                    "address": match.group("mac").upper(),
                    "name": match.group("name").strip(),
                }
            )
    return devices


def parse_info(text: str) -> dict[str, Any]:
    info: dict[str, Any] = {"raw": strip_ansi(text).strip(), "uuids": []}
    current_uuid: str | None = None
    for line in strip_ansi(text).splitlines():
        clean = line.strip()
        if not clean or clean.startswith("info "):
            continue
        if clean.startswith("UUID:"):
            value = clean.split("UUID:", 1)[1].strip()
            info["uuids"].append(value)
            current_uuid = value
            continue
        if current_uuid and clean.startswith("(") and clean.endswith(")"):
            info["uuids"][-1] = f"{info['uuids'][-1]} {clean}"
            current_uuid = None
            continue
        if ":" in clean:
            key, value = clean.split(":", 1)
            key = key.strip().lower().replace(" ", "_")
            value = value.strip()
            if key not in info:
                info[key] = value
    return info


def parse_attributes(text: str) -> list[dict[str, str]]:
    attributes: list[dict[str, str]] = []
    for line in strip_ansi(text).splitlines():
        match = ATTRIBUTE_RE.search(line)
        if match:
            attributes.append(
                {
                    "kind": match.group("kind"),
                    "uuid": match.group("uuid").lower(),
                    "label": (match.group("label") or "").strip(),
                    "handle": match.group("handle").lower(),
                    "raw": line.strip(),
                }
            )
    return attributes


def parse_read_value(text: str) -> dict[str, Any]:
    clean = strip_ansi(text).strip()
    values: list[str] = []
    for line in clean.splitlines():
        if "0x" in line:
            values.extend(re.findall(r"0x([0-9A-Fa-f]{2})", line))
        elif re.fullmatch(r"(?:[0-9A-Fa-f]{2}\s*)+", line.strip()):
            values.extend(re.findall(r"[0-9A-Fa-f]{2}", line))
    return {
        "ok": bool(values),
        "hex": " ".join(v.upper() for v in values),
        "raw": clean,
    }


def is_yes(info: dict[str, Any], field: str) -> bool:
    return str(info.get(field, "")).lower() == "yes"


def wait_for_info_field(
    ctl: BluetoothCtl,
    address: str,
    field: str,
    value: str,
    timeout: int,
    interval: float = 2.0,
) -> tuple[bool, dict[str, Any], list[str]]:
    deadline = time.monotonic() + timeout
    snapshots: list[str] = []
    last_info: dict[str, Any] = {}
    while time.monotonic() < deadline:
        raw = ctl.cmd(f"info {address}", timeout=10)
        snapshots.append(strip_ansi(raw).strip())
        last_info = parse_info(raw)
        if str(last_info.get(field, "")).lower() == value.lower():
            return True, last_info, snapshots
        time.sleep(interval)
    return False, last_info, snapshots


def markdown_report(data: dict[str, Any]) -> str:
    lines = [
        "# Ganymede Enumeration Report",
        "",
        f"- Generated: `{data['generated_at']}`",
        f"- Target address: `{data.get('address') or 'unknown'}`",
        f"- Target name filter: `{data['args'].get('name') or 'none'}`",
        f"- btmon capture: `{data.get('btmon_capture') or 'not captured'}`",
        f"- bluetoothctl transcript: `{data['transcript']}`",
        "",
        "## Device Info",
        "",
        "| Field | Value |",
        "|---|---|",
    ]
    info = data.get("device_info", {})
    for key in sorted(k for k in info.keys() if k not in {"raw", "uuids"}):
        lines.append(f"| `{key}` | `{info[key]}` |")
    lines.extend(["", "## UUIDs", ""])
    for uuid in info.get("uuids", []):
        lines.append(f"- `{uuid}`")
    if not info.get("uuids"):
        lines.append("- unknown")

    lines.extend(
        [
            "",
            "## GATT Attributes",
            "",
            "| Handle | Kind | UUID | Label | Read Result |",
            "|---|---|---|---|---|",
        ]
    )
    reads = {item["handle"]: item for item in data.get("reads", [])}
    for attr in data.get("attributes", []):
        read = reads.get(attr["handle"])
        if read is None:
            read_result = "not attempted"
        elif read.get("ok"):
            read_result = f"`{read.get('hex', '')}`"
        else:
            read_result = "failed or not readable"
        lines.append(
            f"| `{attr['handle']}` | {attr['kind']} | `{attr['uuid']}` | "
            f"{attr.get('label') or ''} | {read_result} |"
        )

    lines.extend(["", "## Raw bluetoothctl info", "", "```text"])
    lines.append(info.get("raw", ""))
    lines.extend(["```", ""])
    return "\n".join(lines)


def run(args: argparse.Namespace) -> int:
    root = Path(args.output_dir).resolve()
    raw_dir = root / "raw"
    export_dir = root / "exports"
    raw_dir.mkdir(parents=True, exist_ok=True)
    export_dir.mkdir(parents=True, exist_ok=True)

    slug = now_slug()
    transcript = raw_dir / f"ganymede_bluetoothctl_{slug}.log"
    btmon_proc = None
    btmon_path = None
    if args.btmon:
        btmon_proc, btmon_path = start_btmon(raw_dir, slug)

    data: dict[str, Any] = {
        "generated_at": dt.datetime.now(dt.UTC).isoformat(),
        "args": {
            "address": args.address,
            "name": args.name,
            "scan_seconds": args.scan_seconds,
            "pair": not args.no_pair,
            "btmon": args.btmon,
        },
        "transcript": str(transcript),
        "btmon_capture": str(btmon_path) if btmon_path else None,
        "devices_seen": [],
        "attributes": [],
        "reads": [],
        "errors": [],
    }

    ctl: BluetoothCtl | None = None
    try:
        ctl = BluetoothCtl(transcript, timeout=args.timeout)
        ctl.cmd("power on", timeout=10)
        ctl.cmd("agent KeyboardDisplay", timeout=10)
        ctl.cmd("default-agent", timeout=10)

        scan_output = ""
        if args.scan_seconds > 0:
            scan_output = ctl.scan(args.scan_seconds)
        scan_output += ctl.cmd("devices", timeout=10)
        devices = parse_devices(scan_output)
        data["devices_seen"] = devices

        address = args.address.upper() if args.address else None
        if address is None:
            matches = [
                item
                for item in devices
                if args.name.lower() in item.get("name", "").lower()
            ]
            if not matches:
                raise RuntimeError(
                    f"no device matching name {args.name!r}; rerun with --address"
                )
            address = matches[0]["address"]
        if not MAC_RE.fullmatch(address):
            raise RuntimeError(f"invalid BLE address: {address}")
        data["address"] = address

        data["pre_pair_info"] = parse_info(ctl.cmd(f"info {address}", timeout=10))

        if not args.no_pair:
            data["pair_output"] = ctl.send_and_wait(
                f"pair {address}",
                timeout=args.pair_timeout,
                passkey=args.passkey,
                confirm=not args.reject_confirm,
            )
            paired, pair_info, pair_snapshots = wait_for_info_field(
                ctl, address, "paired", "yes", args.pair_timeout
            )
            data["pair_wait_info"] = pair_info
            data["pair_wait_snapshots"] = pair_snapshots
            if not paired:
                data["errors"].append(
                    "pairing did not reach Paired: yes; put the remote in pairing mode "
                    "and rerun, or use --no-pair for bonded reconnect tests"
                )
            data["trust_output"] = ctl.cmd(f"trust {address}", timeout=10)

        data["connect_output"] = ctl.send_and_wait(
            f"connect {address}",
            timeout=args.connect_timeout,
            passkey=args.passkey,
            confirm=not args.reject_confirm,
        )
        connected, connect_info, connect_snapshots = wait_for_info_field(
            ctl, address, "connected", "yes", args.connect_timeout
        )
        data["connect_wait_info"] = connect_info
        data["connect_wait_snapshots"] = connect_snapshots
        if not connected:
            data["errors"].append(
                "connection did not reach Connected: yes; GATT enumeration skipped"
            )
            data["device_info"] = connect_info
            raise RuntimeError(data["errors"][-1])

        time.sleep(args.settle_seconds)
        data["device_info"] = parse_info(ctl.cmd(f"info {address}", timeout=10))

        ctl.cmd("menu gatt", timeout=10)
        attr_output = ctl.cmd("list-attributes", timeout=20)
        attributes = parse_attributes(attr_output)
        data["attributes"] = attributes
        data["raw_attributes"] = strip_ansi(attr_output).strip()

        handles = [item["handle"] for item in attributes]
        if args.max_reads > 0:
            handles = handles[: args.max_reads]
        for handle in handles:
            try:
                ctl.cmd(f"select-attribute {handle}", timeout=10)
                info = ctl.cmd("attribute-info", timeout=10)
                read_text = ctl.cmd("read", timeout=args.read_timeout)
                parsed = parse_read_value(read_text)
                parsed.update({"handle": handle, "attribute_info": strip_ansi(info).strip()})
                data["reads"].append(parsed)
            except Exception as exc:
                data["reads"].append(
                    {
                        "handle": handle,
                        "ok": False,
                        "hex": "",
                        "raw": "",
                        "error": str(exc),
                    }
                )

        try:
            ctl.cmd("menu main", timeout=5)
        except Exception:
            pass
    except Exception as exc:
        if str(exc) not in data["errors"]:
            data["errors"].append(str(exc))
        print(f"error: {exc}", file=sys.stderr)
    finally:
        if ctl is not None:
            ctl.close()
        stop_process(btmon_proc)
        if btmon_path and btmon_path.exists() and btmon_path.stat().st_size <= 16:
            data["errors"].append(
                "btmon capture contains no HCI events; rerun with permissions that "
                "allow btmon to monitor the controller"
            )

    json_path = export_dir / f"ganymede_enumeration_{slug}.json"
    md_path = export_dir / f"ganymede_enumeration_{slug}.md"
    json_path.write_text(json.dumps(data, indent=2, sort_keys=True), encoding="utf-8")
    md_path.write_text(markdown_report(data), encoding="utf-8")

    print(f"wrote {json_path}")
    print(f"wrote {md_path}")
    print(f"transcript {transcript}")
    if btmon_path:
        print(f"btmon {btmon_path}")
    return 1 if data["errors"] else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pair with and enumerate a Ganymede BLE remote using bluetoothctl."
    )
    parser.add_argument("--address", help="BLE address of the Ganymede remote.")
    parser.add_argument(
        "--name",
        default="Ganymede",
        help="Name substring used during scan when --address is omitted.",
    )
    parser.add_argument(
        "--output-dir",
        default="captures",
        help="Directory containing raw/ and exports/ output folders.",
    )
    parser.add_argument("--scan-seconds", type=int, default=20)
    parser.add_argument("--settle-seconds", type=float, default=3.0)
    parser.add_argument("--timeout", type=int, default=20)
    parser.add_argument("--pair-timeout", type=int, default=90)
    parser.add_argument("--connect-timeout", type=int, default=45)
    parser.add_argument("--read-timeout", type=int, default=15)
    parser.add_argument(
        "--max-reads",
        type=int,
        default=0,
        help="Maximum attributes to read. 0 means attempt all discovered attributes.",
    )
    parser.add_argument(
        "--passkey",
        help="Passkey/PIN to answer if pairing asks for one. Stored only as redacted transcript marker.",
    )
    parser.add_argument(
        "--reject-confirm",
        action="store_true",
        help="Reject bluetoothctl confirmation prompts instead of accepting them.",
    )
    parser.add_argument(
        "--no-pair",
        action="store_true",
        help="Skip pair/trust and only connect/enumerate.",
    )
    parser.add_argument(
        "--no-btmon",
        dest="btmon",
        action="store_false",
        help="Disable parallel btmon raw capture.",
    )
    parser.set_defaults(btmon=True)
    return parser.parse_args(argv)


if __name__ == "__main__":
    raise SystemExit(run(parse_args(sys.argv[1:])))
