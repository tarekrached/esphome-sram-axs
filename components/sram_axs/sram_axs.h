#pragma once

// SRAM AXS BLE advertisement listener.
//
// Protocol constants used here come from ../../facts.yaml (the single source of
// truth for this repo — see AGENTS.md) and are cited by section on every use.
// facts.yaml tracks confirmed-vs-hypothesis per field; anything not marked
// "confirmed" there is called out again at its use site here.

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/core/preferences.h"  // ESPPreferenceObject — NVS state restore

#include <cstdint>
#include <ctime>  // time_t (SRAMAxsStoredState.last_seen_epoch source type)
#include <string>

namespace esphome {
namespace sram_axs {

/// Last-known state persisted to NVS, one blob per configured device (restore:
/// true, the default). Exists because the product is burst-on-wake (facts.yaml
/// timing.natural_window_pack_s / natural_window_coin_s): broadcasts are the ONLY
/// data source, so without this a reboot (power blip, OTA) blanks every entity
/// until the bike is next touched — hours or days later.
///
/// Packed and trivially copyable on purpose: ESPPreferenceObject::save/load
/// (esphome/core/preference_backend.h) round-trips raw bytes of sizeof(T), so the
/// layout IS the storage format. Any layout change must bump the "v1" in the
/// preference-key string (see setup() in the .cpp) so old blobs are ignored as a
/// load-miss rather than misread as the new shape.
struct SRAMAxsStoredState {
  uint32_t last_seen_epoch;  // UTC epoch of last decoded advert; only meaningful with STORED_LAST_SEEN_VALID
  uint16_t battery_mv;       // facts.yaml manufacturer_data.battery_mv, stored verbatim
  uint8_t percent_raw;       // facts.yaml manufacturer_data.battery_percent_packs, raw mfr byte 11
                             // INCLUDING the 0xFF coin sentinel — restore re-derives coin-vs-pack from it
  uint8_t flags;             // STORED_* bits below; 0 = nothing valid
} __attribute__((packed));

/// SRAMAxsStoredState.flags bits. Separate bits because the two field groups go
/// stale independently: a bare mfr frame (facts.yaml timing.app_silences_broadcast)
/// can update last_seen without battery fields, and battery fields can be decoded
/// before SNTP has synced.
constexpr uint8_t STORED_BATTERY_VALID = 1 << 0;    // battery_mv + percent_raw are real decoded values
constexpr uint8_t STORED_LAST_SEEN_VALID = 1 << 1;  // last_seen_epoch came from a synced clock

/// One configured SRAM AXS component (rear derailleur, dropper post, shift
/// controller, ...), matched by the decimal serial printed in its advertised name
/// and confirmed against service_data bytes 2-5 (facts.yaml service_data.serial).
/// Serial matching prefers the real service-data AD structure; when an advert
/// carries no service data (e.g. the 25-byte extended manufacturer variant), the
/// serial is recovered from the embedded blob inside manufacturer data instead
/// (facts.yaml advert_variants.extended_svc).
///
/// AXS advertisers whose serial is configured on NO instance get an INFO discovery
/// line — once per serial per boot, plus at most one fuller repeat if the first
/// advert carried no battery fields — so users learn the serial to add to their
/// config without touching log levels; `dump_unknown: true` additionally hex-dumps
/// those adverts' raw payloads at DEBUG. See log_unknown_advert_().
class SRAMAxsDevice : public Component, public esp32_ble_tracker::ESPBTDeviceListener {
 public:
  /// Defined in the .cpp: besides storing the serial, it registers it into a
  /// process-wide configured-serials registry shared by every instance, so the
  /// discovery path can tell "configured on a sibling instance" apart from
  /// "configured nowhere". Called from generated setup code, before
  /// esp32_ble_tracker starts scanning, so the registry is complete before the
  /// first advertisement can reach parse_device().
  void set_serial(uint32_t serial);
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_dump_unknown(bool dump_unknown) { this->dump_unknown_ = dump_unknown; }
  void set_time(time::RealTimeClock *time_source) { this->time_ = time_source; }
  /// `restore:` config key (default true): persist last-known state to NVS and
  /// republish it on boot. When false, no preference is created, saved, or loaded.
  void set_restore(bool restore) { this->restore_ = restore; }

  void set_battery_voltage_sensor(sensor::Sensor *battery_voltage_sensor) {
    this->battery_voltage_sensor_ = battery_voltage_sensor;
  }
  void set_battery_percent_sensor(sensor::Sensor *battery_percent_sensor) {
    this->battery_percent_sensor_ = battery_percent_sensor;
  }
  void set_battery_status_text_sensor(text_sensor::TextSensor *battery_status_text_sensor) {
    this->battery_status_text_sensor_ = battery_status_text_sensor;
  }
  void set_rssi_sensor(sensor::Sensor *rssi_sensor) { this->rssi_sensor_ = rssi_sensor; }
  void set_last_seen_text_sensor(text_sensor::TextSensor *last_seen_text_sensor) {
    this->last_seen_text_sensor_ = last_seen_text_sensor;
  }

  /// Restore path: loads the NVS blob (when restore_ is set) and republishes the
  /// last-known state, so a reboot never blanks the dashboard until the bike's
  /// next wake. Runs at setup_priority::DATA (600), i.e. before wifi/API exist —
  /// sensors retain published state, and the native API sends current states to
  /// HA on connect, so publishing this early is safe and idiomatic.
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Called by esp32_ble_tracker for every parsed advertisement seen on the radio,
  /// not just ones addressed to this device — bounds-checks and serial-matches
  /// everything itself. Returns true only when the advertisement carried this
  /// device's serial; see the return-contract note in parse_device()'s definition.
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

 protected:
  /// Discovery logging for an AXS advertiser whose serial is configured on no
  /// instance. Two tiers with independent per-serial process-wide dedups (see the
  /// dedup stores in the .cpp for why they must not share a slot): an INFO line
  /// naming the serial and whatever fields the advert carried — once per serial
  /// per boot, upgraded with one fuller repeat if the first sighting lacked
  /// battery fields (facts.yaml timing.app_silences_broadcast bare frames) — and,
  /// when dump_unknown_ is set, a once-per-serial DEBUG hex dump of the advert's
  /// raw manufacturer/service data (plus MAC).
  void log_unknown_advert_(const esp32_ble_tracker::ESPBTDevice &device, uint32_t serial,
                           optional<uint8_t> device_id, optional<uint16_t> battery_mv,
                           optional<uint8_t> percent_raw);

  uint32_t serial_{0};
  std::string device_name_;
  /// `restore:` config key — see set_restore().
  bool restore_{true};
  /// NVS handle for this device's SRAMAxsStoredState blob; only ever bound (in
  /// setup()) when restore_ is set. Default-constructed = null backend, so a
  /// stray save() on a restore:false instance is a harmless no-op by design.
  ESPPreferenceObject pref_;
  /// In-RAM copy of the stored blob. Seeded from NVS on boot (when the load
  /// succeeds) so a later partial advert — e.g. a bare frame updating only
  /// last_seen — saves alongside the still-valid restored battery fields
  /// instead of clobbering them with zeros. Saved on every qualifying advert —
  /// deliberately unthrottled; see the save comment in parse_device() for the
  /// pending-queue + interval-sync mechanism that batches real flash writes.
  SRAMAxsStoredState stored_{};
  /// Verbose tier only: the one-per-serial INFO discovery line is always on; this
  /// additionally hex-dumps unconfigured advertisers' raw payloads (and MAC — kept
  /// out of the INFO line, where the serial is the identity users act on) at DEBUG.
  bool dump_unknown_{false};
  time::RealTimeClock *time_{nullptr};
  /// One-shot WARN guard: fires once when a configured battery_percent sensor
  /// meets a device reporting the 0xFF "not supported" sentinel (facts.yaml
  /// manufacturer_data.battery_percent_packs).
  bool percent_unsupported_logged_{false};
  /// One-shot WARN guard, mirroring percent_unsupported_logged_ above: fires once
  /// when a configured battery_status sensor meets a pack device (mfr byte 11 !=
  /// 0xFF) — battery_status is coin-only, symmetric with battery_percent being
  /// pack-only.
  bool battery_status_unsupported_logged_{false};

  sensor::Sensor *battery_voltage_sensor_{nullptr};
  sensor::Sensor *battery_percent_sensor_{nullptr};
  text_sensor::TextSensor *battery_status_text_sensor_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  text_sensor::TextSensor *last_seen_text_sensor_{nullptr};
};

}  // namespace sram_axs
}  // namespace esphome
