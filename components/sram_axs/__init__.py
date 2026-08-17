"""sram_axs — passive BLE listener for SRAM AXS component battery broadcasts.

Decodes the advertisements documented in ../../facts.yaml (the protocol source of
truth — every offset and constant referenced from the C++ side cites it) and publishes
one entity set per configured serial. No pairing, bonding, or GATT connection: this is
a pure esp32_ble_tracker passive listener, so it never costs the component's own
battery.

Config shape (MULTI_CONF: one list item per physical AXS component):

    sram_axs:
      - serial: 1234567890
        name: "Rear Derailleur"
        battery_voltage:
          name: "Rear Derailleur Battery Voltage"
        battery_percent:
          name: "Rear Derailleur Battery Percent"   # AXS-pack components only, see below
        battery_status:
          name: "Rear Derailleur Battery Status"    # coin-cell components only, see below
        rssi:
          name: "Rear Derailleur Signal Strength"
        last_seen:
          name: "Rear Derailleur Last Seen"
        time_id: sntp_time                            # required only if last_seen: is set
        restore: true                                 # default — persist last state to NVS, republish on boot
        dump_unknown: false                           # verbose hex tier only — discovery INFO is always on

State restore: broadcasts are burst-on-wake (facts.yaml timing), so a reboot would
otherwise blank every entity until the bike is next touched. With restore: true (the
default) the last decoded state — battery fields and the last-seen timestamp — is
saved to NVS on every decoded advert (actual flash writes are batched by ESPHome's
auto-loaded preferences syncer: 60 s interval plus a shutdown flush, byte-identical
blobs skipped) and republished in setup(), so a power blip or OTA never empties the
dashboard. RSSI is deliberately not restored. restore: false stops saving and
loading but does not erase a previously written blob — re-enabling restore later can
republish that old snapshot once on the next boot. Restore is also unconditional on
age: a board unpowered for months republishes months-old values on boot — the
last_seen entity is the staleness signal, which is a reason to configure it.

Discovery: any 0x0933/0xFE51 advertiser whose serial is configured on no sram_axs
instance announces itself at INFO with the serial to add to the config — once per
serial per boot (plus, at most, one fuller repeat if its first advert carried no
battery fields; see facts.yaml timing.app_silences_broadcast bare frames) — always
on, no flag. `dump_unknown: true` additionally hex-dumps those adverts' raw
payloads at DEBUG, once per serial.

Reference implementations this module's shape was checked against, all read from the
installed ESPHome release this component targets (see DEPENDENCIES below — the
component binds the stable `esp32_ble_tracker` listener API, not the newer
`ble_device_base` split that exists only on ESPHome's dev branch):
  - esphome/components/atc_mithermometer/sensor.py — the closest architectural analog:
    one listener class per physical device, `mac_address:`-equivalent required key,
    several *optional* nested sensor.sensor_schema() sub-keys, all wired up in to_code
    with `if key := config.get(...): sens = await sensor.new_sensor(...); cg.add(var.set_x(sens))`.
  - esphome/components/ruuvi_ble/__init__.py — confirms the minimal listener binding
    idiom used here: `DEPENDENCIES = ["esp32_ble_tracker"]`, extend
    `esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA`, then
    `await esp32_ble_tracker.register_ble_device(var, config)`.
  - esphome/components/uart/__init__.py — confirms `MULTI_CONF = True` is what turns a
    top-level component key into a YAML list of independent instances (used here so a
    stranger can list several AXS components under one `sram_axs:` key).
  - esphome/components/sun/__init__.py — confirms the `cv.use_id(time.RealTimeClock)`
    pattern for a component that optionally needs wall-clock time.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble_tracker, sensor, text_sensor, time
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_VOLTAGE,
    CONF_ID,
    CONF_NAME,
    CONF_RESTORE,
    CONF_TIME_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TIMESTAMP,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_VOLT,
)

CODEOWNERS = ["@tarekrached"]
# The user declares `esp32_ble_tracker:` themselves (it is the shared radio hub, and
# its scan parameters are theirs to tune) — so this is a hard dependency, not AUTO_LOAD.
DEPENDENCIES = ["esp32_ble_tracker"]
# The entities here hang off this component's own config keys, not off `sensor:` /
# `text_sensor:` platform blocks, so nothing else drags those components into the
# build — AUTO_LOAD does (same reason dsmr does it). `time` is here too because
# sram_axs.h includes real_time_clock.h unconditionally while `time_id:` is optional.
AUTO_LOAD = ["sensor", "text_sensor", "time"]
# One list item per physical AXS component — see uart's MULTI_CONF for the precedent.
MULTI_CONF = True

sram_axs_ns = cg.esphome_ns.namespace("sram_axs")
SRAMAxsDevice = sram_axs_ns.class_(
    "SRAMAxsDevice", cg.Component, esp32_ble_tracker.ESPBTDeviceListener
)

# No core esphome.const entries exist for these; declared locally like most
# single-component config keys. battery_percent deliberately does NOT reuse core's
# CONF_BATTERY_LEVEL: that const's key string is "battery_level", and this field is
# named `battery_percent` in facts.yaml (the repo's protocol source of truth) and in
# every doc and example. Keeping one name end-to-end beats matching a core const whose
# spelling would silently contradict the documented config key.
CONF_SERIAL = "serial"
CONF_BATTERY_PERCENT = "battery_percent"
CONF_BATTERY_STATUS = "battery_status"
CONF_RSSI = "rssi"
CONF_LAST_SEEN = "last_seen"
CONF_DUMP_UNKNOWN = "dump_unknown"


def _validate_last_seen_needs_time(config):
    if CONF_LAST_SEEN in config and CONF_TIME_ID not in config:
        raise cv.Invalid(
            f"'{CONF_LAST_SEEN}' requires '{CONF_TIME_ID}:' pointing at a configured "
            "`time:` platform (e.g. `time_id: sntp_time` alongside "
            "`time: - platform: sntp id: sntp_time`)"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SRAMAxsDevice),
            # Decimal serial as printed in the advertised local name ("SRAM <serial>",
            # see docs/protocol.md "Local name") and independently confirmed against
            # service_data bytes 2-5 in facts.yaml (serial: confirmed on 4/4 devices).
            cv.Required(CONF_SERIAL): cv.int_range(min=1, max=0xFFFFFFFF),
            cv.Required(CONF_NAME): cv.string,
            # Entity category: battery readings are this component's whole product,
            # not diagnostic housekeeping, so battery_voltage/battery_percent/
            # battery_status/last_seen are left at the default (no category, i.e.
            # a primary entity). rssi is the one exception — it's link plumbing,
            # not a battery reading, so it keeps ENTITY_CATEGORY_DIAGNOSTIC below.
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # facts.yaml manufacturer_data.battery_percent_packs: manufacturer byte
            # 11. Only AXS-pack components (rear derailleur, dropper post) report a
            # percentage; coin-cell components (shift controllers) send the 0xFF
            # "not supported" sentinel, so on those this entity stays unknown
            # forever and battery_status (or battery_voltage) is the monitoring
            # signal instead (facts.yaml timing.coin_alert_thresholds_mv: warn
            # below 2.80 V, critical below 2.75 V).
            cv.Optional(CONF_BATTERY_PERCENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # Coin-cell components only, symmetric with battery_percent above: derived
            # from battery_voltage (mfr bytes 7-8) against the CR2032 thresholds in
            # facts.yaml timing.coin_alert_thresholds_mv (warn 2800 mV, critical 2750
            # mV), gated on the same 0xFF "not supported" sentinel (mfr byte 11) that
            # marks a coin device there. No device_class: there's no HA text-sensor
            # device class for a tri-state battery-health string.
            cv.Optional(CONF_BATTERY_STATUS): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_RSSI): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Burst-on-wake means this is genuinely "last time we heard this device",
            # not a liveness guarantee — see docs/protocol.md's advertising-window
            # numbers (~5m20s on AXS-pack power, ~24s on CR2032, and unreliable coin
            # wakes at low charge). Requires time_id: since the ESP32 has no wall
            # clock of its own.
            cv.Optional(CONF_LAST_SEEN): text_sensor.text_sensor_schema(
                device_class=DEVICE_CLASS_TIMESTAMP,
            ),
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # Persist last-known state (battery fields + last-seen epoch) to NVS
            # and republish it in setup(), so a reboot never blanks the entities
            # of a burst-on-wake data source (see the module docstring). Default
            # true because a blank dashboard after every power blip is the wrong
            # out-of-box behavior; false disables the preference entirely.
            cv.Optional(CONF_RESTORE, default=True): cv.boolean,
            # Verbose tier of discovery logging. The discovery line itself — one
            # INFO line per unconfigured serial per boot (at most one fuller
            # repeat if the first advert lacked battery fields) — is always on and
            # needs no flag; this additionally hex-dumps those adverts' full
            # mfr/service data (DEBUG level, plus MAC). The hex dump has its own
            # process-wide per-serial dedup, so it fires exactly once per serial
            # no matter which (or how many) devices carry this flag.
            cv.Optional(CONF_DUMP_UNKNOWN, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    _validate_last_seen_needs_time,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    cg.add(var.set_serial(config[CONF_SERIAL]))
    cg.add(var.set_device_name(config[CONF_NAME]))
    cg.add(var.set_dump_unknown(config[CONF_DUMP_UNKNOWN]))
    cg.add(var.set_restore(config[CONF_RESTORE]))

    if time_id := config.get(CONF_TIME_ID):
        time_var = await cg.get_variable(time_id)
        cg.add(var.set_time(time_var))

    if battery_voltage_config := config.get(CONF_BATTERY_VOLTAGE):
        sens = await sensor.new_sensor(battery_voltage_config)
        cg.add(var.set_battery_voltage_sensor(sens))
    if battery_percent_config := config.get(CONF_BATTERY_PERCENT):
        sens = await sensor.new_sensor(battery_percent_config)
        cg.add(var.set_battery_percent_sensor(sens))
    if battery_status_config := config.get(CONF_BATTERY_STATUS):
        sens = await text_sensor.new_text_sensor(battery_status_config)
        cg.add(var.set_battery_status_text_sensor(sens))
    if rssi_config := config.get(CONF_RSSI):
        sens = await sensor.new_sensor(rssi_config)
        cg.add(var.set_rssi_sensor(sens))
    if last_seen_config := config.get(CONF_LAST_SEEN):
        sens = await text_sensor.new_text_sensor(last_seen_config)
        cg.add(var.set_last_seen_text_sensor(sens))
