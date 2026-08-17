#!/usr/bin/env bash
# BLE capture session for the feasibility spike, Pi edition (ground truth).
#
# Usage: ./pi-capture.sh <phase-label>     e.g. asleep | woken | app-paired
#
# Writes a timestamped directory under ~/captures/ containing:
#   btmon.snoop   — full HCI trace (btsnoop format; opens in Wireshark)
#   btmon.txt     — same trace, human-readable
#   scan.log      — bluetoothctl's view of discovered devices
#   meta.txt      — phase label, timestamps, adapter state
#
# Run one capture per phase, ~2 min each:
#   1. asleep      bike untouched for 10+ min beforehand
#   2. woken       shake the bike / click a shifter mid-capture
#   3. app-paired  connect the official app mid-capture (reveals the GATT flow)

set -euo pipefail

PHASE="${1:?usage: pi-capture.sh <phase-label>}"
DURATION="${DURATION:-120}"
OUT="$HOME/captures/$(date +%Y%m%d-%H%M%S)-${PHASE}"
mkdir -p "$OUT"

{
  echo "phase: ${PHASE}"
  echo "start: $(date -Is)"
  echo "duration_s: ${DURATION}"
  bluetoothctl show | sed 's/^/adapter: /'
} > "$OUT/meta.txt"

# Full HCI trace: catches every advertisement the radio hears, including
# ones bluetoothctl's dedup hides. This is the file that settles arguments.
sudo btmon -w "$OUT/btmon.snoop" > "$OUT/btmon.txt" 2>&1 &
BTMON_PID=$!
trap 'sudo kill "$BTMON_PID" 2>/dev/null || true' EXIT

sleep 1
echo "capturing '${PHASE}' for ${DURATION}s — do the phase's action now" >&2
bluetoothctl --timeout "$DURATION" scan on > "$OUT/scan.log" 2>&1 || true

echo "end: $(date -Is)" >> "$OUT/meta.txt"
echo "capture complete: $OUT" >&2
ls -lh "$OUT" >&2
