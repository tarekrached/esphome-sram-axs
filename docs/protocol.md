# SRAM AXS BLE advertisement format

What AXS components broadcast, as observed across five live captures of a Rival XPLR / Reverb AXS build (rear derailleur, dropper post, two shift controllers). Field confidences are tracked in [`facts.yaml`](../facts.yaml); serials are masked here as A–D.

## Radio behavior

- Components advertise **only while awake**. Wake triggers, confirmed by controlled comparison: **an AXS-button press, or physical movement**. Shift-paddle clicks do *not* wake the radio — re-confirmed 2026-08-15 by a controlled single-click test on each paddle against a continuously-broadcasting canary device, with zero controller adverts recorded. The official app's own UI documents the wake mechanisms explicitly and agrees: the rear derailleur's is "Shake the bike"; controllers and the dropper are "Press AXS button".
- **Natural advertising window (AXS-pack components): ~5 minutes 20 seconds** after wake, at multiple advertisements per second, measured to natural silence with no app running (326 s and 318 s across the two pack devices). **Coin-cell components: ~20–24 seconds regardless of charge** (measured on the same controller with a dying cell and a fresh one) — a 13× asymmetry a listener must design for. Battery insertion is radio-silent: a fresh battery produces no power-on advertisement; only a button press or movement does. Caution: a low-charge coin component was twice observed to show its wake LED after a button press yet never advertise. **Open question (2026-08-15):** one rear derailleur was observed broadcasting for 10+ minutes in a session where two shift actuations happened mid-window, well past the measured ~5 min 20 s window — actuation (which physically moves the derailleur) may re-trigger the wake timer, or the window varies more than the two-sample measurement suggests. Unresolved.
- **The official app silences the broadcast.** Confirmed three times: the moment the AXS app connects, components stop advertising. A passive listener hears nothing during app sessions. **Nuance (2026-08-15):** in a capture taken with the app open, both pack devices went silent in near-lockstep ~50 s gaps starting within a second of each other, while one coin controller kept emitting bare manufacturer-data frames after its name and service-data announcements had already stopped — suppression may target scan responses / service data specifically rather than all advertising, and isn't always a total blackout.
- Advertisements require **no pairing, no bonding, no app** — any BLE scanner receives them.
- Local name: `SRAM <decimal serial>`.

## Advertisement contents

### Manufacturer data — company ID `0x0933`

12 bytes standard, e.g. `00 00 01 02 00 04 05 b6 1f 03 80 64`

| Bytes | Meaning | Confidence |
|---|---|---|
| 0–1 | `00 00` — constant | observed constant |
| 2–3 | **Firmware version** | **confirmed on 4/4 devices**: every component read `01 01` before its firmware update and `01 02` after, across three separate update sessions. Byte encoding still unknown |
| 4–6 | `00 04 05` — constant | observed constant |
| 7–8 | **Battery voltage, mV, little-endian** (`b6 1f` → 8118 mV) | **confirmed**: live ±5 mV wobble, stable across hours, three device classes land exactly on 2S-pack and CR2032 chemistry, and sagged ~30 mV after a radio-heavy firmware update |
| 9 | `0x03` on all four of our components, but `0x02` in [ShannoG's published dropper capture](https://github.com/ShannoG/home-assistant-sram-axs/tree/main/advertised-data) (their RD reads `0x03`) — so neither globally constant nor a simple device-type code | unknown; varies across fleets |
| 10 | `0x80` — constant in our captures and both external captures | unknown |
| 11 | **Battery percent, pack devices only** — `0x64` (100) on our ~8.2 V full packs, constant across repeated own-fleet captures taken concurrent with the official app reading "Battery Good"; declines with charge in ShannoG's external captures (`0x13` = 19 at 7479 mV, `0x17` = 23 at 7523 mV); `0xFF` on coin devices as a not-supported sentinel, constant regardless of charge (coins report no percentage) | **confirmed cross-fleet** (2026-08-15, external corroboration); pack anchor confirmed own-fleet 2026-08-15 |

The official AXS app itself never displays a percentage at all — only three bands (good / low above 20% / charge-now below 5%) — so this byte, like the raw voltage in bytes 7–8, carries strictly more resolution than the app's own UI exposes.

**Variants:** three manufacturer-data forms observed. (1) The 12-byte standard form above. (2) A 25-byte form: the standard bytes + `01 01 09` + the 9 service-data bytes + `01` — and this can be a device's *only* advertising mode for an entire session (nameless, with **no separate service-data payload at all**), so parsers must extract the serial from the embedded blob or they will miss whole sessions. (3) A 26-byte form seen immediately after a fleet firmware update: the standard bytes + `02 04 0a 02 37 01` + ASCII `gdef20` + `64 02`, with byte-identical tails across devices — hypothesis: a shared firmware build tag (git-describe style, `g` + short SHA), of obvious interest to anyone building update-availability sensing. **Revised 2026-08-15:** not pack-exclusive after all — a coin controller was captured emitting it too, on post-update firmware; the earlier "coins never emit it" reading was an artifact of coins' short broadcast bursts giving fewer chances to observe it.

**Stack framing (confirmed 2026-08-15, ESP32 `esp32_ble_tracker`, all four components):** the 25- and 26-byte forms above reach the ESP-IDF stack as **two separate manufacturer-data records** under company ID `0x0933`: the 12-byte standard record plus a separate 13-byte record (`01 01 09` + the service-data bytes + `01`) or 14-byte record (`02 04 0a 02 37 01` + the build tag + `02`). macOS CoreBluetooth (the `mac-scan.py` tooling below) coalesces these into the single 25/26-byte blobs described above before an app ever sees them. Same bytes, different framing depending on the receiving stack (whether the split is two AD structures in one PDU or advertisement-vs-scan-response payloads is not yet pinned down — a raw `btmon` sniff would settle it). Parsers must accept both presentations, not just the one CoreBluetooth happens to produce.

**Monitoring guidance** (from observed behavior, not SRAM documentation): alert on *voltage*, not percentages — coin devices report no percentage at all. Useful CR2032 thresholds: warn at ~2.80 V (past the discharge knee); treat ~2.75 V as critical — at and below that, controllers were repeatedly observed to light their wake LED yet fail to transmit, i.e. the radio dies before the shifter does. A firmware update measurably sags the battery (30 mV on a pack; 200 mV on a fresh CR2032, recovering with rest; 56 mV on a dying one — which still completed its DFU successfully). The official app's category display called two ~2.7 V cells "Battery Good"; voltage-level monitoring exists precisely because that category is generous.

### Service data — 16-bit UUID `0xFE51` (Bluetooth SIG member UUID: SRAM)

9 bytes, e.g. `0d 03 9f fc 91 3c 32 04 81`

| Bytes | Meaning | Confidence |
|---|---|---|
| 0 | `0x0d` — constant | observed constant |
| 1 | `0x03` on our rear derailleur, `0x02` on our dropper and both controllers — but `0x03` on ShannoG's dropper, so it does not track device class across fleets | unknown; device-class candidate weakened |
| 2–5 | **Component serial, uint32 little-endian** — matches the decimal serial in the advertised name | confirmed on 4/4 devices |
| 6 | **NOT battery percent** (a controlled battery swap, 2.71 V → fresh 3.19 V cell, left it unchanged at 10, including 30+ min later). Static per device: our controllers 10/11 (adjacent on the L/R pair), dropper 45, RD 50. But cross-fleet values differ by device (ShannoG's RD 36, dropper 1), so if it's an identifier it is model- or instance-level, not device-type-level | revised 2026-08-13, weakened further 2026-08-15; earlier percent reading was coincidence |
| 7 | `0x04` — constant | observed constant, holds in external captures |
| 8 | `0x81` (RD), `0x84` (dropper), `0x00`/`0x01` (controllers) — high bit set only on pack devices so far. Same per-class values in ShannoG's unrelated fleet (`0x81` RD, `0x84` dropper): currently the best device-class discriminator in the payload | cross-fleet corroboration 2026-08-15 |

## Observed device classes

| Device | Power | Battery reading | svcdata byte 6 |
|---|---|---|---|
| A — rear derailleur | AXS pack | 8117–8158 mV | 50 |
| B — dropper post | AXS pack | 8209–8221 mV | 45 |
| C — shift controller | CR2032 | 2752 mV | 11 |
| D — shift controller | CR2032 | 2714–2716 mV | 10 |

A monitoring note that motivated this project: with both coin cells at ~10%, the official app still displayed "Battery Good" for every component.

## Relationship to other decodes

[ShannoG/home-assistant-sram-axs](https://github.com/ShannoG/home-assistant-sram-axs) reads the manufacturer data's last byte as battery percent. That decode is right **for pack devices** — their published `advertised-data/` captures and ours corroborate each other across fleets (see the table) — and inapplicable for coin-cell devices, which broadcast `0xFF` (not supported) there even when near-dead. Their captures also disagree with ours on bytes the tables flag (manufacturer byte 9, service-data bytes 1 and 6), which is exactly why cross-fleet data is valuable. [karl-petter/sram-axs-for-ha](https://github.com/karl-petter/sram-axs-for-ha) connects and reads the standard GATT Battery Service (`0x180F`/`0x2A19`) unauthenticated, a working alternative lane with different battery-cost tradeoffs.

## Reproducing a capture

- macOS: `uv run --with bleak tools/mac-scan.py 90`, then wake the bike (AXS button or shake — not a shift click). CoreBluetooth hides real MACs and coalesces adverts; fine for discovery.
- Linux/Pi (ground truth): `tools/pi-capture.sh <phase>` records a Wireshark-openable `btmon` trace.

Corrections and additional captures welcome — especially from component types not listed above (front derailleur, blips, brake levers), from Eagle/Red/Force builds, and from anyone whose readings disagree with the tables.
