#include "sram_axs.h"

#include "esphome/core/alloc_helpers.h"  // format_hex_pretty(vector<uint8_t>)
#include "esphome/core/helpers.h"        // encode_uint16 / encode_uint32, fnv1a_hash(_extend)
#include "esphome/core/log.h"
#include "esphome/core/time.h"           // ESPTime::from_epoch_utc — restore-path last_seen

#include <cinttypes>  // PRIu32 — uint32_t is `long unsigned int` on xtensa, so %u warns
#include <cstdio>     // snprintf — the discovery line's parenthetical is assembled piecewise
#include <vector>

namespace esphome {
namespace sram_axs {

static const char *const TAG = "sram_axs";

namespace {

// facts.yaml ble.manufacturer_id: 0x0933 — confirmed on all AXS adverts.
// Argument order is ESPBTUUID::contains(low_byte, high_byte): for a 16-bit UUID it
// checks `(uuid16 >> 8) == data2 && (uuid16 & 0xFF) == data1` (esp32_ble/ble_uuid.cpp),
// so the *first* argument is the low byte. 0x0933 -> contains(0x33, 0x09).
constexpr uint8_t AXS_MFR_ID_LOW = 0x33;
constexpr uint8_t AXS_MFR_ID_HIGH = 0x09;

// facts.yaml ble.service_uuid_16: 0xFE51 — confirmed (Bluetooth SIG member UUID: SRAM).
constexpr uint8_t AXS_SERVICE_UUID_LOW = 0x51;
constexpr uint8_t AXS_SERVICE_UUID_HIGH = 0xFE;

// facts.yaml timing.coin_alert_thresholds_mv: CR2032 discharge-knee thresholds
// (warn past the knee; critical a margin above the ~2.7 V floor where a dying cell
// was still operating), not from SRAM documentation. Integer mV, named after warn/critical
// keys, and compared with plain integer `>` below — no float thresholds, so there's
// no float-precision surprise in a comparison against a value that only ever needs
// to answer "above/below a fixed knee, once, per advert."
constexpr uint16_t COIN_BATTERY_WARN_MV = 2800;      // facts.yaml timing.coin_alert_thresholds_mv.warn
constexpr uint16_t COIN_BATTERY_CRITICAL_MV = 2750;  // facts.yaml timing.coin_alert_thresholds_mv.critical

/// The one home for the coin-cell status-string derivation, shared by the live
/// path (parse_device) and the NVS restore path (setup) so the two can never
/// drift apart on thresholds or wording. The three exact strings are published
/// state HA automations match on — plain ASCII, documented in example.yaml.
const char *coin_status_from_mv(uint16_t battery_mv) {
  if (battery_mv > COIN_BATTERY_WARN_MV)
    return "OK";
  if (battery_mv > COIN_BATTERY_CRITICAL_MV)
    return "Low - replace soon";
  return "Critical - replace now";
}

/// One discovery-line dedup entry per serial ever logged, remembering whether the
/// line it got was "full" — percent_raw was present at log time, so classification
/// and voltage made it into the parenthetical.
struct DiscoveryLogged {
  uint32_t serial;
  bool full;
};

/// Decides whether the INFO discovery line fires for this sighting, and records the
/// outcome. Shared (function-local static) across every configured SRAMAxsDevice
/// instance: esp32_ble_tracker hands each parsed advertisement to every registered
/// listener, so without this dedup an unknown serial would be logged once per
/// *configured* device rather than once, ever.
///
/// Semantics are log-once-then-upgrade-once, not a plain set: facts.yaml
/// timing.app_silences_broadcast documents coin controllers emitting bare 12/13-byte
/// mfr frames (no service data) while the app suppresses the rest, and the 13-byte
/// split record yields a serial with no battery fields — a serial first sighted in
/// that mode would otherwise be stuck with a battery-less discovery line until
/// reboot. So: first sighting always logs and records fullness; after a full line
/// the serial is silent forever; after a partial line, the first advert that DOES
/// carry percent_raw logs one fuller repeat and marks the serial full, while
/// further partial adverts stay silent (bare-frame mode must not spam one INFO per
/// advert).
bool should_log_discovery(uint32_t serial, bool full) {
  static std::vector<DiscoveryLogged> logged;
  for (auto &entry : logged) {
    if (entry.serial == serial) {
      if (!entry.full && full) {
        entry.full = true;  // upgrade: a partial line is already out, this advert can better it
        return true;
      }
      return false;
    }
  }
  logged.push_back({serial, full});
  return true;
}

/// True once this serial's raw payloads have already been hex-dumped. A plain
/// once-per-serial set, deliberately independent of should_log_discovery() above
/// (and with no upgrade semantics — the dump prints whatever the triggering advert
/// carries): listeners run in config order, so if the two tiers shared one dedup
/// slot, a flag-less earlier-listed instance would consume it on the INFO line and
/// the hex tier on a later dump_unknown-flagged instance would never fire, all boot.
bool already_hex_dumped_serial(uint32_t serial) {
  static std::vector<uint32_t> dumped;
  for (uint32_t s : dumped) {
    if (s == serial)
      return true;
  }
  dumped.push_back(serial);
  return false;
}

/// Process-wide registry of every serial configured on ANY SRAMAxsDevice instance,
/// same function-local-static sharing pattern as the dedup stores above. Needed
/// because esp32_ble_tracker offers each advert to every listener: "not MY serial"
/// alone can't tell a sibling instance's configured device from a genuinely
/// unconfigured one, and without this registry instance A would report instance
/// B's own device as discovered-but-unconfigured.
std::vector<uint32_t> &configured_serials() {
  static std::vector<uint32_t> serials;
  return serials;
}

/// True when this serial is configured on some instance (this one or a sibling).
bool is_configured_serial(uint32_t serial) {
  for (uint32_t s : configured_serials()) {
    if (s == serial)
      return true;
  }
  return false;
}

}  // namespace

void SRAMAxsDevice::set_serial(uint32_t serial) {
  this->serial_ = serial;
  // Generated setup code calls this before esp32_ble_tracker starts scanning, so
  // every configured serial is registered before the first advertisement can reach
  // any parse_device() — no startup window where a sibling's device looks
  // unconfigured.
  configured_serials().push_back(serial);
}

void SRAMAxsDevice::setup() {
  if (!this->restore_)
    return;

  // Preference key: FNV-1a (helpers.h's preferred hash for new code) over a
  // format-version prefix, extended with the serial's raw bytes. The "v1" is the
  // stored-struct format version: any SRAMAxsStoredState layout change bumps it,
  // so old blobs under the old key simply miss on load (silent first-boot path
  // below) instead of being byte-reinterpreted as the new shape. Per-serial key,
  // one independent blob per configured device.
  const uint32_t key = fnv1a_hash_extend(fnv1a_hash("sram_axs_v1_"), this->serial_);
  // `true` = NVS flash (the two-arg default on ESP32 too, but explicit here:
  // this state must survive full power loss, not just a warm reset).
  this->pref_ = global_preferences->make_preference<SRAMAxsStoredState>(key, true);

  SRAMAxsStoredState loaded{};
  if (!this->pref_.load(&loaded) || loaded.flags == 0) {
    // First boot, restore previously disabled, or a format-version bump — all
    // expected, all silent (VERBOSE at most, per the restore contract).
    ESP_LOGV(TAG, "%s: no stored state to restore", this->device_name_.c_str());
    return;
  }
  // Seed the in-RAM copy so the write path's partial updates (e.g. a bare frame
  // refreshing only last_seen) save alongside these restored fields instead of
  // clobbering them with zeros.
  this->stored_ = loaded;

  // Republish last-known state so a reboot (power blip, OTA) never blanks the
  // dashboard: broadcasts are burst-on-wake (facts.yaml timing), so fresh data
  // only arrives when the bike is next touched. Same publish gates as the live
  // path in parse_device — percent for packs, status for coins, both keyed on
  // the stored raw 0xFF sentinel (facts.yaml manufacturer_data.battery_percent_packs).
  // Deliberately NO warn-path here: the percent-on-coin / status-on-pack
  // one-shot warnings stay live-path-only, where they describe a real advert.
  // RSSI is deliberately NOT restored — a stale RSSI is meaningless.
  if (loaded.flags & STORED_BATTERY_VALID) {
    if (this->battery_voltage_sensor_ != nullptr)
      this->battery_voltage_sensor_->publish_state(loaded.battery_mv / 1000.0f);
    if (loaded.percent_raw != 0xFF) {
      if (this->battery_percent_sensor_ != nullptr)
        this->battery_percent_sensor_->publish_state(loaded.percent_raw);
    } else if (this->battery_status_text_sensor_ != nullptr) {
      this->battery_status_text_sensor_->publish_state(coin_status_from_mv(loaded.battery_mv));
    }
  }

  // last_seen republishes the STORED epoch, not now: the entity means "last time
  // we heard this device", and that moment predates the reboot. from_epoch_utc
  // needs no synced clock (the epoch is absolute), so this works before SNTP —
  // unlike the live path, which must skip until sync.
  char ts[32] = "";  // "2026-08-17T00:00:00Z" is 20 chars + NUL
  if (loaded.flags & STORED_LAST_SEEN_VALID) {
    auto t = ESPTime::from_epoch_utc(static_cast<time_t>(loaded.last_seen_epoch));
    if (t.is_valid()) {
      t.strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ");
      if (this->last_seen_text_sensor_ != nullptr)
        this->last_seen_text_sensor_->publish_state(ts);
    }
  }

  // One INFO line per device per boot, assembled piecewise like the discovery
  // line: only fields the blob actually holds — omitted, never placeholder-printed.
  // Worst pre-strip case is "battery 65.53 V, Critical - replace now, last seen
  // 2106-02-07T06:28:15Z, " — 73 bytes plus NUL — so 96 can never truncate and
  // the unclamped `pos` arithmetic is safe (same reasoning as the discovery line).
  char details[96];
  int pos = 0;
  if (loaded.flags & STORED_BATTERY_VALID) {
    pos += snprintf(details + pos, sizeof(details) - pos, "battery %.2f V, ", loaded.battery_mv / 1000.0f);
    if (loaded.percent_raw != 0xFF) {
      pos += snprintf(details + pos, sizeof(details) - pos, "%u%%, ", static_cast<unsigned>(loaded.percent_raw));
    } else {
      pos += snprintf(details + pos, sizeof(details) - pos, "%s, ", coin_status_from_mv(loaded.battery_mv));
    }
  }
  if ((loaded.flags & STORED_LAST_SEEN_VALID) && ts[0] != '\0') {
    pos += snprintf(details + pos, sizeof(details) - pos, "last seen %s, ", ts);
  }
  if (pos >= 2) {
    details[pos - 2] = '\0';  // strip the trailing ", " left by whichever piece came last
    ESP_LOGI(TAG, "%s: restored last known state (%s)", this->device_name_.c_str(), details);
  }
}

void SRAMAxsDevice::dump_config() {
  ESP_LOGCONFIG(TAG, "SRAM AXS device '%s' (serial %" PRIu32 ")", this->device_name_.c_str(), this->serial_);
  LOG_SENSOR("  ", "Battery Voltage", this->battery_voltage_sensor_);
  LOG_SENSOR("  ", "Battery Percent (pack devices only)", this->battery_percent_sensor_);
  LOG_TEXT_SENSOR("  ", "Battery Status (coin devices only)", this->battery_status_text_sensor_);
  LOG_SENSOR("  ", "RSSI", this->rssi_sensor_);
  LOG_TEXT_SENSOR("  ", "Last Seen", this->last_seen_text_sensor_);
  if (this->dump_unknown_) {
    ESP_LOGCONFIG(TAG, "  Hex-dump unknown AXS advertisers: yes");
  }
}

void SRAMAxsDevice::log_unknown_advert_(const esp32_ble_tracker::ESPBTDevice &device, uint32_t serial,
                                        optional<uint8_t> device_id, optional<uint16_t> battery_mv,
                                        optional<uint8_t> percent_raw) {
  // The two tiers below carry independent dedups — see already_hex_dumped_serial()
  // for why sharing one slot would let a flag-less instance starve the hex tier.

  // One INFO discovery line per serial per boot (plus at most one fuller repeat if
  // the first advert carried no battery fields — see should_log_discovery()),
  // always on: the serial is the value a user needs to copy into their config, so
  // it can't hide behind a debug flag or the DEBUG log level. The parenthetical
  // assembles only fields this advert actually carried — omitted, never
  // placeholder-printed. Classification comes from the percent byte alone
  // (facts.yaml manufacturer_data.battery_percent_packs: 0xFF is the coin-cell
  // "not supported" sentinel, anything else is a pack percentage) — deliberately
  // NOT from a voltage threshold. No MAC here either: the serial is the identity
  // users act on; the MAC stays in the dump_unknown_ DEBUG detail below. Plain
  // ASCII throughout (published-strings precedent).
  //
  // Buffer sizing: worst case is "coin-cell, 65.53 V, device_id=255, rssi -127 dBm"
  // — 48 bytes plus NUL — so 96 can never truncate and the unclamped `pos`
  // arithmetic below is safe (snprintf's would-be-length return only exceeds the
  // space remaining on truncation).
  if (should_log_discovery(serial, percent_raw.has_value())) {
    char details[96];
    int pos = 0;
    if (percent_raw.has_value()) {
      pos += snprintf(details + pos, sizeof(details) - pos, "%s, ", *percent_raw == 0xFF ? "coin-cell" : "pack");
    }
    if (battery_mv.has_value()) {
      pos += snprintf(details + pos, sizeof(details) - pos, "%.2f V, ", *battery_mv / 1000.0f);
    }
    if (device_id.has_value()) {
      pos += snprintf(details + pos, sizeof(details) - pos, "device_id=%u, ", static_cast<unsigned>(*device_id));
    }
    snprintf(details + pos, sizeof(details) - pos, "rssi %d dBm", device.get_rssi());
    ESP_LOGI(TAG, "Discovered SRAM AXS serial=%" PRIu32 " (%s) - add to your sram_axs config to publish sensors",
             serial, details);
  }

  // Verbose tier, opt-in via dump_unknown: raw payload hex plus the MAC, for
  // protocol work rather than discovery. Its dedup is checked only inside this
  // branch, so it can only ever be consumed by an instance that actually carries
  // the flag — regardless of where that instance sits in listener order.
  if (!this->dump_unknown_)
    return;
  if (already_hex_dumped_serial(serial))
    return;

  char addr[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  // facts.yaml service_data.device_id: static per device (left/right controllers
  // differ only by this byte, 10 vs 11). Omitted, not printed as a placeholder,
  // when unknown (e.g. an advert that failed every serial-yielding gate).
  if (device_id.has_value()) {
    ESP_LOGD(TAG, "Unmapped AXS advertiser: serial=%" PRIu32 " mac=%s rssi=%d device_id=%u", serial,
             device.address_str_to(addr), device.get_rssi(), static_cast<unsigned>(*device_id));
  } else {
    ESP_LOGD(TAG, "Unmapped AXS advertiser: serial=%" PRIu32 " mac=%s rssi=%d", serial, device.address_str_to(addr),
             device.get_rssi());
  }

  for (const auto &mfr : device.get_manufacturer_datas()) {
    if (mfr.uuid.contains(AXS_MFR_ID_LOW, AXS_MFR_ID_HIGH)) {
      ESP_LOGD(TAG, "  manufacturer data (%zu bytes): %s", mfr.data.size(), format_hex_pretty(mfr.data).c_str());
    }
  }
  for (const auto &svc : device.get_service_datas()) {
    if (svc.uuid.contains(AXS_SERVICE_UUID_LOW, AXS_SERVICE_UUID_HIGH)) {
      ESP_LOGD(TAG, "  service data (%zu bytes): %s", svc.data.size(), format_hex_pretty(svc.data).c_str());
    }
  }
}

bool SRAMAxsDevice::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  // Cheap pre-filter: only look further at adverts carrying SRAM's manufacturer ID
  // or service UUID. Every AXS device seen so far broadcasts both together in one
  // merged advertisement (facts.yaml timing.advert_rate, docs/protocol.md example),
  // but this is deliberately an OR so a partial/split capture doesn't get dropped
  // silently before we even try the service-data branch below.
  bool is_axs = false;
  for (const auto &mfr : device.get_manufacturer_datas()) {
    if (mfr.uuid.contains(AXS_MFR_ID_LOW, AXS_MFR_ID_HIGH)) {
      is_axs = true;
      break;
    }
  }
  if (!is_axs) {
    for (const auto &svc : device.get_service_datas()) {
      if (svc.uuid.contains(AXS_SERVICE_UUID_LOW, AXS_SERVICE_UUID_HIGH)) {
        is_axs = true;
        break;
      }
    }
  }
  if (!is_axs)
    return false;

  // Collect-then-decide: walk both AD structure kinds first, deciding nothing
  // until every field this advert can yield has been gathered. The fallback rule
  // below (serial from the embedded manufacturer blob, only when no real service
  // data was seen) needs that decision made *after* both walks, which the old
  // two-loop shape (match on service data, then a second loop for battery) could
  // not express.
  //
  // The manufacturer-data walk below examines every 0x0933 record in the advert,
  // not just the first: facts.yaml advert_variants.stack_framing (confirmed
  // 2026-08-15) documents that on ESP-IDF (esp32_ble_tracker) an extended_svc or
  // build_tag advert arrives as TWO separate 0x0933 records — the 12-byte standard
  // record plus a separate 13- or 14-byte record — rather than one coalesced
  // 25/26-byte blob the way macOS CoreBluetooth presented it. Stopping at the
  // first record examined (as CoreBluetooth-shaped code can get away with) misses
  // whichever half wasn't first under this framing.
  //
  // Serial precedence: real service data wins (facts.yaml service_data.serial is
  // the confirmed decode on 4/4 devices). The embedded blob inside the 25-byte
  // extended manufacturer variant (advert_variants.extended_svc) is a fallback,
  // whether coalesced into one record or split per stack_framing above. It's
  // needed for a device advertising in that mode with no separate service data at
  // all, a real, observed device state (facts.yaml timing.variant_choice_varies),
  // not a hypothetical.
  optional<uint32_t> serial;
  optional<uint8_t> device_id;
  optional<uint16_t> battery_mv;
  optional<uint8_t> percent_raw;

  for (const auto &svc : device.get_service_datas()) {
    if (!svc.uuid.contains(AXS_SERVICE_UUID_LOW, AXS_SERVICE_UUID_HIGH))
      continue;

    // facts.yaml service_data is 9 bytes nominal; bounds-check before every
    // indexed read rather than trusting that length.
    if (svc.data.size() < 6) {
      ESP_LOGV(TAG, "SRAM AXS service data too short for serial (%zu bytes)", svc.data.size());
      continue;
    }
    // facts.yaml service_data.serial: bytes 2-5, uint32 little-endian —
    // confirmed on 4/4 devices, matches the decimal serial in the advertised name.
    serial = encode_uint32(svc.data[5], svc.data[4], svc.data[3], svc.data[2]);

    if (svc.data.size() >= 7) {
      // facts.yaml service_data.device_id: byte 6 — static per device (10/11 for
      // the two controllers, 45 dropper, 50 RD). NOT a battery percentage (see the
      // manufacturer_data.battery_percent_packs decode below for the real one);
      // this is logged only, via log_unknown_advert_(), and never published.
      device_id = svc.data[6];
    }
    break;
  }

  for (const auto &mfr : device.get_manufacturer_datas()) {
    if (!mfr.uuid.contains(AXS_MFR_ID_LOW, AXS_MFR_ID_HIGH))
      continue;
    const auto &data = mfr.data;

    // No `break`/early-return in this loop: an advert can carry more than one
    // 0x0933 record (facts.yaml advert_variants.stack_framing), so every record
    // must be examined. Fields already set from an earlier record in this same
    // walk are never overwritten — first-wins, same precedence rule the class doc
    // comment above describes for serial.

    // Standard-shape branch. Shape check: bytes 0-1 ("00 00", facts.yaml
    // manufacturer_data.prefix) and 4-6 ("00 04 05", facts.yaml
    // manufacturer_data.mid) are constant across all three known variants
    // (facts.yaml advert_variants: standard/extended_svc/build_tag all carry
    // this 12-byte prefix). size() >= 12 is the smallest of the three.
    //
    // A split-framing 13-byte extended_svc record (facts.yaml
    // advert_variants.stack_framing) correctly fails this check — its byte 0 is
    // 0x01, not 0x00 — so it falls through to the dedicated branch below.
    if (data.size() >= 12 && data[0] == 0x00 && data[1] == 0x00 && data[4] == 0x00 && data[5] == 0x04 &&
        data[6] == 0x05) {
      if (!battery_mv.has_value()) {
        // facts.yaml manufacturer_data.battery_mv: bytes 7-8, uint16 little-endian —
        // confirmed.
        battery_mv = encode_uint16(data[8], data[7]);
      }
      if (!percent_raw.has_value()) {
        // facts.yaml manufacturer_data.battery_percent_packs: byte 11. Pack devices
        // only; 0xFF is the "not supported" sentinel coin-cell devices send. facts.yaml
        // marks this CONFIRMED (cross-fleet) 2026-08-15: our own packs read 0x64=100
        // at full charge, and external ShannoG advertised-data captures show the byte
        // declining with voltage on partially discharged packs (0x13=19 at 7479 mV,
        // 0x17=23 at 7523 mV). Published verbatim regardless.
        // What to do about that sentinel is decided after both walks, once serial
        // matching has confirmed this advert is even worth publishing (see below).
        percent_raw = data[11];
      }

      if (!serial.has_value() && data.size() == 25) {
        // facts.yaml advert_variants.extended_svc: 25-byte manufacturer payload
        // embedding a byte-identical copy of the 9-byte service-data blob at
        // offsets 15-23, behind a "01 01 09" marker (offsets 12-14) and a trailing
        // "01" (offset 24). Only trusted as a serial source when no real service
        // data was seen above — a device advertising only this variant for a whole
        // session has no other way to be matched (see the class doc comment).
        // This is the CoreBluetooth-coalesced presentation of extended_svc; under
        // ESP-IDF split framing (stack_framing) the same bytes arrive as a
        // separate 13-byte record instead and are handled by the branch below.
        //
        // Four-condition gate, ANDed, rather than the length check alone: exact
        // size, the marker, the embedded blob's own constant byte 0 (facts.yaml
        // service_data.byte0, 0x0d), and the trailer. This makes a misparse
        // structurally impossible instead of merely unlikely — the 26-byte
        // build-tag variant (facts.yaml advert_variants.build_tag) is also a
        // 12-byte-prefix-plus-tail shape and fails every one of these four checks
        // (size; marker; blob byte 0, which is 0x02 there; trailer). Its tail is
        // deliberately left unparsed; see advert_variants.build_tag.
        if (data[12] == 0x01 && data[13] == 0x01 && data[14] == 0x09 && data[15] == 0x0d && data[24] == 0x01) {
          // facts.yaml service_data.serial, embedded at manufacturer offsets 17-20
          // (blob offset 2-5, blob starting at manufacturer offset 15).
          serial = encode_uint32(data[20], data[19], data[18], data[17]);
          // facts.yaml service_data.device_id, embedded at manufacturer offset 21.
          device_id = data[21];
        }
      }
      continue;
    }

    // Split-framing branch: facts.yaml advert_variants.stack_framing (confirmed
    // 2026-08-15) documents that on ESP-IDF the extended_svc variant's tail can
    // arrive as its own 13-byte 0x0933 record — "01 01 09" + the 9 service-data
    // bytes + "01" — rather than appended to the 12-byte standard record above.
    // Only trusted as a serial source under the same precedence rule as the
    // 25-byte embedded fallback: real service data wins, this is the fallback for
    // a session advertising only this variant.
    //
    // Four-condition gate, ANDed, mirroring the 25-byte branch above: exact size
    // (13), the "01 01 09" marker (offsets 0-2, vs 12-14 in the coalesced blob),
    // the embedded service-data blob's own constant byte 0 (facts.yaml
    // service_data.byte0, 0x0d, at offset 3, vs offset 15 coalesced), and the
    // trailing "01" (offset 12, vs offset 24 coalesced). The 14-byte build_tag
    // split record (stack_framing) starts "02 ..." and fails data[0] == 0x01
    // immediately, so it can never satisfy this gate — it falls through to the
    // ESP_LOGV skip below, same as the 26-byte coalesced build_tag payload does
    // against the standard-shape branch above.
    if (data.size() == 13 && data[0] == 0x01 && data[1] == 0x01 && data[2] == 0x09 && data[3] == 0x0d &&
        data[12] == 0x01) {
      if (!serial.has_value()) {
        // facts.yaml service_data.serial, embedded at record offsets 5-8 (blob
        // offset 2-5, blob starting at record offset 3).
        serial = encode_uint32(data[8], data[7], data[6], data[5]);
        // facts.yaml service_data.device_id, embedded at record offset 9.
        device_id = data[9];
      }
      continue;
    }

    ESP_LOGV(TAG, "%s: manufacturer data failed shape check (%zu bytes)", this->device_name_.c_str(), data.size());
  }

  if (!serial.has_value())
    return false;

  if (*serial != this->serial_) {
    // Not this device's serial. If it's configured on ANY instance, stay silent —
    // esp32_ble_tracker offers every advert to every listener, so the owning
    // sibling will claim it, and logging it here would misreport a configured
    // device as undiscovered (first listener in registration order would win the
    // dedup slot). Configured nowhere → discovery logging, unconditionally:
    // dump_unknown_ only controls the DEBUG hex tier inside log_unknown_advert_().
    if (!is_configured_serial(*serial))
      this->log_unknown_advert_(device, *serial, device_id, battery_mv, percent_raw);
    return false;
  }

  // facts.yaml manufacturer_data.battery_mv — publish whenever the shape check
  // above passed (battery_mv is only ever set inside that branch). Unchanged
  // publish rule, just relocated: the widened `size() >= 12` guard (was `>= 9`)
  // is needed because byte 11 (percent) is now read too.
  if (battery_mv.has_value() && this->battery_voltage_sensor_ != nullptr)
    this->battery_voltage_sensor_->publish_state(*battery_mv / 1000.0f);

  // facts.yaml manufacturer_data.battery_percent_packs — 0xFF is the "not
  // supported" sentinel coin-cell devices send; publish nothing (not NAN, not
  // 255) rather than a misleading reading, and warn once (percent_unsupported_logged_)
  // if the user configured a percent sensor anyway, pointing at voltage instead
  // (facts.yaml timing.coin_alert_thresholds_mv). Otherwise publish the raw byte
  // verbatim: no clamping, no range validation, only 0xFF is a sentinel.
  if (percent_raw.has_value()) {
    if (*percent_raw == 0xFF) {
      if (this->battery_percent_sensor_ != nullptr && !this->percent_unsupported_logged_) {
        ESP_LOGW(TAG,
                 "%s: battery_percent is configured, but this component reports no percentage (coin-cell "
                 "device, mfr byte 11 = 0xFF). Use battery_status (or battery_voltage) instead — warn below "
                 "2.80 V, critical below 2.75 V.",
                 this->device_name_.c_str());
        this->percent_unsupported_logged_ = true;
      }
    } else if (this->battery_percent_sensor_ != nullptr) {
      this->battery_percent_sensor_->publish_state(*percent_raw);
    }
  }

  // battery_status: the coin-only counterpart to battery_percent above, gated on
  // the same 0xFF sentinel (facts.yaml manufacturer_data.battery_percent_packs) and
  // the same percent_raw decoded from this same advert — a coin device's identity
  // is established the same way in both branches. Publish nothing (not "Unknown",
  // not a stale value) when percent_raw wasn't decoded at all, since without it
  // there's no way to tell a coin device from a pack device.
  if (this->battery_status_text_sensor_ != nullptr && percent_raw.has_value()) {
    if (*percent_raw == 0xFF) {
      // Coin device. Requires battery_mv from this same advert
      // (manufacturer_data.battery_mv, bytes 7-8) — always present alongside
      // percent_raw once the standard-shape branch above has matched, but guarded
      // rather than assumed.
      if (battery_mv.has_value()) {
        this->battery_status_text_sensor_->publish_state(coin_status_from_mv(*battery_mv));
      }
    } else if (!this->battery_status_unsupported_logged_) {
      // Pack device (real percentage, not the sentinel): battery_status is
      // configured on a device that doesn't need it, symmetric with the
      // battery_percent-on-a-coin-device warning above.
      ESP_LOGW(TAG,
               "%s: battery_status is configured, but this component reports a real battery percentage "
               "(mfr byte 11 = %u, not the coin-cell 0xFF sentinel). Use battery_percent instead.",
               this->device_name_.c_str(), static_cast<unsigned>(*percent_raw));
      this->battery_status_unsupported_logged_ = true;
    }
  }

  if (this->rssi_sensor_ != nullptr)
    this->rssi_sensor_->publish_state(device.get_rssi());

  // HA's timestamp device_class (see __init__.py's last_seen schema) requires an
  // RFC3339 string or it silently rejects the state and the entity reads "unknown"
  // forever. Publish UTC with an explicit Z suffix rather than local time — HA
  // renders it as a relative time ("5 minutes ago") in the UI regardless.
  //
  // The epoch is captured whenever the time source exists and is synced — not
  // just when the last_seen sensor is configured — because the NVS write path
  // below persists it too.
  optional<time_t> now_epoch;
  if (this->time_ != nullptr) {
    auto now = this->time_->utcnow();
    if (now.is_valid()) {
      now_epoch = now.timestamp;
      if (this->last_seen_text_sensor_ != nullptr)
        this->last_seen_text_sensor_->publish_state(now.strftime("%Y-%m-%dT%H:%M:%SZ"));
    } else if (this->last_seen_text_sensor_ != nullptr) {
      ESP_LOGV(TAG, "%s: time not yet synced, skipping last_seen update", this->device_name_.c_str());
    }
  }

  // NVS write path (restore: true, the default). Only ever reached for this
  // instance's own matched serial — the unknown-serial/discovery path returned
  // above and never writes. Fold this advert into the in-RAM blob first: battery
  // fields only when this advert carried both (a bare mfr frame updates neither),
  // the epoch only when the clock was synced — the two validity bits go stale
  // independently (see SRAMAxsStoredState in the header).
  if (this->restore_) {
    bool updated = false;
    if (battery_mv.has_value() && percent_raw.has_value()) {
      this->stored_.battery_mv = *battery_mv;
      this->stored_.percent_raw = *percent_raw;  // raw byte 11, 0xFF coin sentinel stored verbatim
      this->stored_.flags |= STORED_BATTERY_VALID;
      updated = true;
    }
    if (now_epoch.has_value()) {
      this->stored_.last_seen_epoch = static_cast<uint32_t>(*now_epoch);
      this->stored_.flags |= STORED_LAST_SEEN_VALID;
      updated = true;
    }
    // Save on every qualifying advert — deliberately unthrottled, because this
    // is NOT a per-advert flash write. ESP32PreferenceBackend::save()
    // (components/esp32/preferences.cpp) only upserts the bytes into an in-RAM
    // pending-save vector; the real nvs_set_blob happens in
    // ESP32Preferences::sync(), driven by the auto-loaded preferences
    // IntervalSyncer (flash_write_interval, default 60 s) and by on_shutdown()
    // — and sync() skips byte-identical blobs (is_changed_) before writing.
    // Worst case is therefore ~5-6 NVS writes per ~5-min pack wake burst
    // (facts.yaml timing.natural_window_pack_s = 320 s), a few dozen writes per
    // day on a daily-ridden bike — trivial against wear-leveled NVS endurance.
    // What per-advert saving buys: the banked state is genuinely the LAST
    // decoded advert — at most 60 s of freshness lost on a hard power cut, none
    // on a clean shutdown or OTA (on_shutdown syncs the pending queue).
    if (updated)
      this->pref_.save(&this->stored_);
  }

  ESP_LOGD(TAG, "%s (serial %" PRIu32 "): rssi=%d dBm", this->device_name_.c_str(), this->serial_,
           device.get_rssi());

  // Return-value contract, per the consumer in esp32_ble_tracker.cpp: the tracker ORs
  // every listener's result into a `found` flag and, when nothing claimed the advert
  // and scanning is non-continuous, falls back to print_bt_device_info(). So `true`
  // means "this advertisement was mine, don't dump it as an unknown device" — not
  // "I published something". Serial matched, so claim it even on a config with every
  // sensor left unset (publishing nothing is a legitimate config, not a parse failure).
  return true;
}

}  // namespace sram_axs
}  // namespace esphome
