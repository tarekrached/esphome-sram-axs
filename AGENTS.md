# esphome-sram-axs — agent guide

An ESPHome external component that decodes SRAM AXS battery broadcasts on an ESP32 for Home Assistant, plus the protocol teardown behind it. Working on this repo with an AI agent? Start here.

## Ground rules

- **`facts.yaml` is the single source of truth for protocol numbers.** Every offset, UUID, and constant the component uses must match it, and every entry carries a confidence label (`confirmed` vs `hypothesis`). Never promote a hypothesis to confirmed without capture evidence; never let component code and facts.yaml disagree.
- **`docs/protocol.md` is the human-readable teardown.** It cites facts.yaml's confidence levels — keep the two consistent in the same commit when either changes.
- **Raw captures stay out of the repo** (`captures/` is gitignored — they contain component serials). Findings from captures go into facts.yaml/docs with serials masked.
- **Decode corrections need evidence.** A PR changing a field interpretation should include the capture excerpt (masked serials) that justifies it.
- Component code lives in `components/sram_axs/`; a minimal user-facing config belongs in `example.yaml` and must stay in sync with the README quickstart.

## Layout

| Path | What |
|---|---|
| `docs/protocol.md` | Advertisement format teardown |
| `facts.yaml` | Protocol constants + confidence — source of truth |
| `components/` | The ESPHome external component |
| `tools/` | Capture utilities (macOS `bleak` scanner, Pi `btmon` script) |

## Prior art

[ShannoG/home-assistant-sram-axs](https://github.com/ShannoG/home-assistant-sram-axs) (passive, HA-side) and [karl-petter/sram-axs-for-ha](https://github.com/karl-petter/sram-axs-for-ha) (GATT-read, HA-side) both solve adjacent problems and are credited in the README. This project is the on-device lane. When a finding here affects their decodes, the courteous move is an upstream issue, not silence.
