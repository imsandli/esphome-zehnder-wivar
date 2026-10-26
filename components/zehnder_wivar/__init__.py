import uuid

import esphome.codegen as cg
from esphome.components import ble_client, binary_sensor, climate, number, select, sensor, text_sensor
from esphome.components import time as time_
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import (
    CONF_HUMIDITY,
    CONF_ID,
    CONF_TARGET_TEMPERATURE,
    CONF_TEMPERATURE,
    CONF_TIME_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_WINDOW,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_SECOND,
)

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "number", "climate", "select"]

CONF_APP_UUID = "app_uuid"
CONF_NICKNAME = "nickname"
CONF_ROOM_INDEX = "room_index"
CONF_VALVE = "valve"
CONF_HEATING = "heating"
CONF_ROOM_NAME = "room_name"
CONF_RUN_MODE = "run_mode"
CONF_STATUS = "status"
CONF_WINDOW_OPEN = "window_open"
CONF_BATTERY_LOW = "battery_low"
CONF_OVERHEATED = "overheated"
CONF_ERROR = "error"
CONF_FIRMWARE_UNTESTED = "firmware_untested"
CONF_SIGNAL_STRENGTH = "signal_strength"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_THERMOSTAT = "thermostat"
CONF_CONNECTIONS = "connections"
CONF_SURFACE_TEMPERATURE = "surface_temperature"
CONF_MODE = "mode"
CONF_SCHEDULE = "schedule"
CONF_SCHEDULE_TODAY = "schedule_today"
CONF_NEXT_SWITCH = "next_switch"
CONF_SCHEDULE_WRITE = "schedule_write"
CONF_CLOCK_OFFSET = "clock_offset"
CONF_SYNC_CLOCK = "sync_clock"
# RunModeChoice order.
MODE_OPTIONS = ["Automatic", "Manual", "Antifreeze"]
CONF_CONNECTED_TIME = "connected_time"
CONF_CONNECT_TIMEOUT = "connect_timeout"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_MAX_RETRIES = "max_retries"
CONF_STALE_AFTER = "stale_after"
CONF_MIN_SETPOINT = "min_setpoint"
CONF_MAX_SETPOINT = "max_setpoint"
# Setpoint entities by eTempMode. Home Asleep (1) is left out: the Wivar never uses it and the app doesn't offer it
# (its value is still read and written back unchanged).
SETPOINTS = {0: ("home_setpoint", "mdi:home-thermometer"), 2: ("away_setpoint", "mdi:home-export-outline")}
# Range the Zehnder app offers; a config may narrow it, not widen it.
MIN_SETPOINT = 7.0
MAX_SETPOINT = 28.0
# The app's per-mode limits for the Wivar, by eTempMode (Asleep: not offered by the app, union of the others).
# Keep in sync with proto::SETPOINT_MIN / SETPOINT_MAX.
SETPOINT_LIMITS = ((15.0, 28.0), (10.0, 28.0), (10.0, 25.0))

zehnder_wivar_ns = cg.esphome_ns.namespace("zehnder_wivar")
ZehnderWivar = zehnder_wivar_ns.class_(
    "ZehnderWivar", ble_client.BLEClientNode, cg.PollingComponent
)
WivarSetpointNumber = zehnder_wivar_ns.class_("WivarSetpointNumber", number.Number)
WivarClimate = zehnder_wivar_ns.class_("WivarClimate", climate.Climate)
WivarModeSelect = zehnder_wivar_ns.class_("WivarModeSelect", select.Select)


def validate_uuid(value):
    value = cv.string_strict(value)
    try:
        return str(uuid.UUID(value))
    except ValueError as err:
        raise cv.Invalid(f"Invalid UUID: {err}") from err


def validate_setpoint_range(config):
    if config[CONF_MIN_SETPOINT] >= config[CONF_MAX_SETPOINT]:
        raise cv.Invalid(f"{CONF_MIN_SETPOINT} must be below {CONF_MAX_SETPOINT}")
    for mode, (key, _) in SETPOINTS.items():
        lo, hi = SETPOINT_LIMITS[mode]
        if max(lo, config[CONF_MIN_SETPOINT]) > min(hi, config[CONF_MAX_SETPOINT]):
            raise cv.Invalid(
                f"{CONF_MIN_SETPOINT}/{CONF_MAX_SETPOINT} leave no valid value for {key} ({lo}-{hi} °C)"
            )
    return config


SETPOINT = cv.All(cv.float_, cv.float_range(min=MIN_SETPOINT, max=MAX_SETPOINT))

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZehnderWivar),
            # App identity registered on the Wivar when pairing.
            cv.Required(CONF_APP_UUID): validate_uuid,
            cv.Optional(CONF_NICKNAME, default="ESPHome"): cv.All(
                cv.string_strict, cv.Length(max=32)
            ),
            cv.Optional(CONF_ROOM_INDEX, default=0): cv.int_range(min=0, max=15),
            # Scan + connect limit per session; the BLE tracker's scan duration must be at least this long.
            cv.Optional(CONF_CONNECT_TIMEOUT, default="40s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(minutes=2)),
            ),
            # A failed poll is retried this often, up to max_retries times, before waiting for the next update.
            cv.Optional(CONF_RETRY_INTERVAL, default="30s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=5)),
            ),
            cv.Optional(CONF_MAX_RETRIES, default=3): cv.int_range(min=0, max=10),
            # Readings older than this are published as unknown.
            cv.Optional(CONF_STALE_AFTER, default="20min"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(minutes=1)),
            ),
            cv.Optional(CONF_MIN_SETPOINT, default=MIN_SETPOINT): SETPOINT,
            cv.Optional(CONF_MAX_SETPOINT, default=MAX_SETPOINT): SETPOINT,
            # Clock for the heating timer countdown (the remote reports the end time as unix time), the schedule
            # sensors and the remote's clock check.
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            # Check the remote's clock once a day and set it (with the daylight-saving switches) when it is off,
            # like the app does on every connect. Needs time_id with a timezone.
            cv.Optional(CONF_SYNC_CLOCK, default=True): cv.boolean,
            # Run mode: switches like the app (Manual picks Home or Away).
            cv.Optional(CONF_MODE): select.select_schema(WivarModeSelect, icon="mdi:calendar-sync"),
            # The remote's schedule, read only (read at start, on "Refresh now" and once a day).
            cv.Optional(CONF_SCHEDULE): text_sensor.text_sensor_schema(
                icon="mdi:calendar-week",
            ),
            cv.Optional(CONF_SCHEDULE_TODAY): text_sensor.text_sensor_schema(
                icon="mdi:calendar-today",
            ),
            cv.Optional(CONF_NEXT_SWITCH): text_sensor.text_sensor_schema(
                icon="mdi:calendar-clock",
            ),
            # Outcome of the last schedule change (set_schedule): Writing..., OK, Unchanged, Refused/Failed: why.
            # A change may take a few sessions (one to write, one to read back; more after a lost connection).
            cv.Optional(CONF_SCHEDULE_WRITE): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:calendar-edit",
            ),
            # The remote's clock minus ours.
            cv.Optional(CONF_CLOCK_OFFSET): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-alert-outline",
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TARGET_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:thermostat",
            ),
            cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_HUMIDITY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VALVE): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:valve",
            ),
            cv.Optional(CONF_HEATING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_HEAT,
            ),
            cv.Optional(CONF_ROOM_NAME): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:home-thermometer",
            ),
            cv.Optional(CONF_RUN_MODE): text_sensor.text_sensor_schema(
                icon="mdi:calendar-clock",
            ),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:bluetooth-connect",
            ),
            cv.Optional(CONF_WINDOW_OPEN): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_WINDOW,
            ),
            # cDevice.overheatAlarm. The app only shows it for the Zenia heater; a Wivar has never been seen sending it.
            cv.Optional(CONF_OVERHEATED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
                icon="mdi:thermometer-alert",
            ),
            # The remote reports no battery level, only the BatteryLow device error.
            cv.Optional(CONF_BATTERY_LOW): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_BATTERY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # On when the remote runs firmware other than the version the protocol was verified against.
            cv.Optional(CONF_FIRMWARE_UNTESTED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ERROR): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
            ),
            cv.Optional(CONF_SIGNAL_STRENGTH): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:signal",
            ),
            cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:chip",
            ),
            # Surface temperature setting of the heater (app: "Surface temperature", 50 or 65 °C). Read only.
            cv.Optional(CONF_SURFACE_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_TEMPERATURE,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:radiator",
            ),
            # Connections since boot and the connected time of the last one: what the polling costs the remote.
            cv.Optional(CONF_CONNECTIONS): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:bluetooth-transfer",
            ),
            cv.Optional(CONF_CONNECTED_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:timer-outline",
            ),
            # Writes the setpoint of the active temperature mode.
            cv.Optional(CONF_THERMOSTAT): climate.climate_schema(WivarClimate),
            **{
                cv.Optional(key): number.number_schema(
                    WivarSetpointNumber,
                    unit_of_measurement=UNIT_CELSIUS,
                    device_class=DEVICE_CLASS_TEMPERATURE,
                    icon=icon,
                )
                for key, icon in SETPOINTS.values()
            },
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("60s")),
    validate_setpoint_range,
)


def _final_validate(config):
    # The component connects through the tracker's scan: a scan shorter than the connect timeout ends early.
    tracker = fv.full_config.get().get("esp32_ble_tracker")
    if isinstance(tracker, list):
        tracker = tracker[0] if tracker else None
    duration = ((tracker or {}).get("scan_parameters") or {}).get("duration")
    if duration is not None and duration.total_milliseconds < config[CONF_CONNECT_TIMEOUT].total_milliseconds:
        raise cv.Invalid(
            f"esp32_ble_tracker scan duration ({duration}) is shorter than {CONF_CONNECT_TIMEOUT} "
            f"({config[CONF_CONNECT_TIMEOUT]})"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_app_uuid(list(uuid.UUID(config[CONF_APP_UUID]).bytes)))
    cg.add(var.set_nickname(config[CONF_NICKNAME]))
    cg.add(var.set_room_index(config[CONF_ROOM_INDEX]))
    cg.add(var.set_connect_timeout(config[CONF_CONNECT_TIMEOUT].total_milliseconds))
    cg.add(var.set_retry_interval(config[CONF_RETRY_INTERVAL].total_milliseconds))
    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))
    cg.add(var.set_stale_after(config[CONF_STALE_AFTER].total_milliseconds))
    cg.add(var.set_setpoint_range(config[CONF_MIN_SETPOINT], config[CONF_MAX_SETPOINT]))
    if time_id := config.get(CONF_TIME_ID):
        cg.add(var.set_time(await cg.get_variable(time_id)))
    cg.add(var.set_time_sync(CONF_TIME_ID in config, CONF_TIME_ID in config and config[CONF_SYNC_CLOCK]))
    if conf := config.get(CONF_MODE):
        sel = await select.new_select(conf, options=MODE_OPTIONS)
        cg.add(sel.set_parent(var))
        cg.add(var.set_mode_select(sel))

    for key, setter in (
        (CONF_TEMPERATURE, var.set_temperature_sensor),
        (CONF_TARGET_TEMPERATURE, var.set_target_temperature_sensor),
        (CONF_HUMIDITY, var.set_humidity_sensor),
        (CONF_VALVE, var.set_valve_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await sensor.new_sensor(conf)))
    for key, setter in (
        (CONF_HEATING, var.set_heating_binary_sensor),
        (CONF_WINDOW_OPEN, var.set_window_open_binary_sensor),
        (CONF_BATTERY_LOW, var.set_battery_low_binary_sensor),
        (CONF_OVERHEATED, var.set_overheated_binary_sensor),
        (CONF_FIRMWARE_UNTESTED, var.set_firmware_untested_binary_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await binary_sensor.new_binary_sensor(conf)))
    for key, setter in (
        (CONF_SIGNAL_STRENGTH, var.set_signal_strength_sensor),
        (CONF_CONNECTIONS, var.set_connections_sensor),
        (CONF_SURFACE_TEMPERATURE, var.set_surface_temperature_sensor),
        (CONF_CLOCK_OFFSET, var.set_clock_offset_sensor),
        (CONF_CONNECTED_TIME, var.set_connected_time_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await sensor.new_sensor(conf)))
    for key, setter in (
        (CONF_ROOM_NAME, var.set_room_name_text_sensor),
        (CONF_RUN_MODE, var.set_run_mode_text_sensor),
        (CONF_STATUS, var.set_status_text_sensor),
        (CONF_FIRMWARE_VERSION, var.set_firmware_text_sensor),
        (CONF_ERROR, var.set_error_text_sensor),
        (CONF_SCHEDULE, var.set_schedule_text_sensor),
        (CONF_SCHEDULE_TODAY, var.set_schedule_today_text_sensor),
        (CONF_NEXT_SWITCH, var.set_next_switch_text_sensor),
        (CONF_SCHEDULE_WRITE, var.set_schedule_write_text_sensor),
    ):
        if conf := config.get(key):
            cg.add(setter(await text_sensor.new_text_sensor(conf)))
    for mode, (key, _) in SETPOINTS.items():
        if conf := config.get(key):
            lo, hi = SETPOINT_LIMITS[mode]
            num = await number.new_number(
                conf,
                min_value=max(lo, config[CONF_MIN_SETPOINT]),
                max_value=min(hi, config[CONF_MAX_SETPOINT]),
                step=0.5,
            )
            cg.add(num.set_parent(var, mode))
            cg.add(var.set_setpoint_number(mode, num))
    if conf := config.get(CONF_THERMOSTAT):
        therm = await climate.new_climate(conf)
        cg.add(therm.set_parent(var))
        cg.add(var.set_climate(therm))
