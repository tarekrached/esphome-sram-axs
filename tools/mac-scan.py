#!/usr/bin/env python3
"""BLE advertisement scanner for the feasibility spike, macOS edition.

Run:  uv run --with bleak tools/mac-scan.py [seconds]

Prints every BLE advertiser seen: name, RSSI, manufacturer data (company ID +
payload hex), service UUIDs, service data. Lines matching NAME_HINTS are
flagged so SRAM gear stands out; everything is printed regardless, because the
whole point is discovering what's on the air, not confirming a guess.

macOS caveats (CoreBluetooth): device addresses are opaque per-host UUIDs, not
real MACs, and the OS may coalesce or rate-limit advertisements. Fine for
"does the bike transmit anything at all" — the Pi capture is the ground truth.
"""

import asyncio
import sys
from datetime import datetime

from bleak import BleakScanner

NAME_HINTS = ("sram", "axs", "etap", "reverb", "eagle")
seen: dict[str, str] = {}


def render(device, adv) -> str:
    parts = [f"rssi={adv.rssi:>4}"]
    if adv.local_name:
        parts.append(f"name={adv.local_name!r}")
    for cid, payload in (adv.manufacturer_data or {}).items():
        parts.append(f"mfr=0x{cid:04x}:{payload.hex()}")
    for uuid, payload in (adv.service_data or {}).items():
        parts.append(f"svcdata={uuid}:{payload.hex()}")
    if adv.service_uuids:
        parts.append(f"svcs={','.join(adv.service_uuids)}")
    return " ".join(parts)


def on_adv(device, adv):
    line = render(device, adv)
    if seen.get(device.address) == line:
        return  # unchanged since last print; skip repeats
    seen[device.address] = line
    hot = any(h in (adv.local_name or "").lower() for h in NAME_HINTS)
    marker = ">>> " if hot else "    "
    stamp = datetime.now().strftime("%H:%M:%S")
    print(f"{marker}{stamp} {device.address} {line}", flush=True)


async def main(duration: float):
    print(f"scanning for {duration:.0f}s — wake the bike (shake it, click a shifter) partway through", flush=True)
    scanner = BleakScanner(detection_callback=on_adv)
    await scanner.start()
    await asyncio.sleep(duration)
    await scanner.stop()
    print(f"done — {len(seen)} distinct advertisers seen", flush=True)


if __name__ == "__main__":
    asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 60.0))
