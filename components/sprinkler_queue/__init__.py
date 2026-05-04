"""ESPHome external component: sprinkler_queue.

A queue-based sprinkler controller. Guarantees that at most one zone valve
is open at any time by serializing requests through an internal FIFO queue
with a configurable inter-valve pause.

Each zone is exposed as a native HA `valve.*` entity. Per-valve duration
is exposed as a `number.*` entity (persisted to flash). A global pause
between valves is exposed as another `number.*`. An optional master valve
is exposed as a `binary_sensor.*` reflecting the master relay state.

Hardware-agnostic: any ESPHome pin source (direct GPIO, sn74hc595,
mcp23017, pcf8574, ...) is accepted via the standard `pins.gpio_output_pin_schema`.

Targets ESPHome 2026.4+ where EntityBase setters are private and entities
are configured via the new setup_entity() helper which calls configure_entity_().
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, number, valve
from esphome.components.number import NumberMode
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ICON,
    CONF_ID,
    CONF_NAME,
    CONF_MIN_VALUE,
    CONF_MAX_VALUE,
    CONF_PIN,
)
from esphome import pins
from esphome.core import CORE
from esphome.core.entity_helpers import setup_entity

AUTO_LOAD = ["binary_sensor", "number", "valve"]
CODEOWNERS = ["@cristoforocervino"]
DOMAIN = "sprinkler_queue"

# ── Custom config keys ─────────────────────────────────────────────────────
CONF_PAUSE_BETWEEN_VALVES = "pause_between_valves"
CONF_MASTER_VALVE = "master_valve"
CONF_VALVES = "valves"
CONF_INITIAL_VALUE = "initial_value"
CONF_INITIAL_DURATION = "initial_duration"
CONF_RESTORE_VALUE = "restore_value"

# ── Default entity names (used when the user omits `name:`) ───────────────
DEFAULT_NAME_PAUSE = "Pause between valves"
DEFAULT_NAME_MASTER = "Master valve"
DURATION_SUFFIX = "duration"

# ── C++ namespace / class declarations ────────────────────────────────────
sprinkler_queue_ns = cg.esphome_ns.namespace("sprinkler_queue")

SprinklerQueueController = sprinkler_queue_ns.class_(
    "SprinklerQueueController", cg.Component
)
ZoneValve = sprinkler_queue_ns.class_("ZoneValve", valve.Valve, cg.Component)
ZoneNumber = sprinkler_queue_ns.class_("ZoneNumber", number.Number, cg.Component)
MasterBinarySensor = sprinkler_queue_ns.class_(
    "MasterBinarySensor", binary_sensor.BinarySensor, cg.Component
)
PauseNumber = sprinkler_queue_ns.class_("PauseNumber", number.Number, cg.Component)

# ── Pause number sub-schema ────────────────────────────────────────────────
# Uses the official number_schema so users get the full set of number fields
# (mode, unit_of_measurement, step, etc.) for free.
PAUSE_NUMBER_SCHEMA = (
    number.number_schema(PauseNumber)
    .extend(
        {
            cv.Optional(CONF_INITIAL_VALUE, default=5): cv.positive_int,
            cv.Optional(CONF_MIN_VALUE, default=0): cv.int_range(min=0, max=3600),
            cv.Optional(CONF_MAX_VALUE, default=120): cv.int_range(min=0, max=3600),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

# ── Master valve sub-schema (optional block) ───────────────────────────────
MASTER_VALVE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_NAME): cv.string,  # default injected before validation
        cv.Optional(CONF_ICON, default="mdi:water-pump"): cv.icon,
        cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
        cv.GenerateID(): cv.declare_id(MasterBinarySensor),
    }
)

# ── Per-valve sub-schema ───────────────────────────────────────────────────
VALVE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_INITIAL_DURATION, default=300): cv.int_range(
            min=1, max=3600
        ),
        cv.GenerateID("valve_id"): cv.declare_id(ZoneValve),
        cv.GenerateID("number_id"): cv.declare_id(ZoneNumber),
    }
)


def _inject_default_names(config):
    """Inject default names BEFORE schema validation runs.

    The pause and master_valve sub-schemas use number_schema/entity_base
    validators that require ``name:`` (or a manual ``id:``) to be present.
    Users shouldn't have to spell out the name when the default is fine, so
    we fill in the default here.

    Explicit ``name:`` in YAML always wins (we only inject when missing).
    """
    if not isinstance(config, dict):
        return config

    pause_conf = config.get(CONF_PAUSE_BETWEEN_VALVES)
    if isinstance(pause_conf, dict) and CONF_NAME not in pause_conf:
        pause_conf[CONF_NAME] = DEFAULT_NAME_PAUSE

    master_conf = config.get(CONF_MASTER_VALVE)
    if isinstance(master_conf, dict) and CONF_NAME not in master_conf:
        master_conf[CONF_NAME] = DEFAULT_NAME_MASTER

    return config


CONFIG_SCHEMA = cv.All(
    _inject_default_names,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SprinklerQueueController),
            cv.Required(CONF_PAUSE_BETWEEN_VALVES): PAUSE_NUMBER_SCHEMA,
            cv.Optional(CONF_MASTER_VALVE): MASTER_VALVE_SCHEMA,
            cv.Required(CONF_VALVES): cv.All(
                cv.ensure_list(VALVE_SCHEMA),
                cv.Length(min=1),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


def _entity_config(name: str, icon: str | None = None) -> dict:
    """Build a minimal entity config dict for setup_entity()."""
    cfg = {
        CONF_NAME: name,
        CONF_DISABLED_BY_DEFAULT: False,
    }
    if icon is not None:
        cfg[CONF_ICON] = icon
    return cfg


# ── Default icons ──────────────────────────────────────────────────────────
# NOTE: zone valves intentionally have NO icon override — HA's valve domain
# natively renders state-based icons (mdi:valve-closed / mdi:valve-open) that
# change with the entity state. Setting an icon would freeze it.
ICON_DURATION = "mdi:timer-outline"
ICON_PAUSE = "mdi:timer-pause-outline"


# ── Code generation ────────────────────────────────────────────────────────
async def to_code(config):
    # ── Main controller ───────────────────────────────────────────────────
    ctrl = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(ctrl, config)

    # ── Pause number ──────────────────────────────────────────────────────
    pause_conf = config[CONF_PAUSE_BETWEEN_VALVES]
    if CONF_ICON not in pause_conf:
        pause_conf[CONF_ICON] = ICON_PAUSE

    pause_num = cg.new_Pvariable(pause_conf[CONF_ID])
    await cg.register_component(pause_num, pause_conf)
    await number.register_number(
        pause_num,
        pause_conf,
        min_value=float(pause_conf[CONF_MIN_VALUE]),
        max_value=float(pause_conf[CONF_MAX_VALUE]),
        step=1.0,
    )
    cg.add(pause_num.set_initial_value(float(pause_conf[CONF_INITIAL_VALUE])))
    cg.add(pause_num.set_restore_value(pause_conf[CONF_RESTORE_VALUE]))
    cg.add(ctrl.set_pause_number(pause_num))

    # ── Master valve (optional) ───────────────────────────────────────────
    if CONF_MASTER_VALVE in config:
        master_conf = config[CONF_MASTER_VALVE]
        master_name = master_conf[CONF_NAME]  # injected by _inject_default_names if missing
        master_icon = master_conf[CONF_ICON]

        # Master output pin (any GPIOPin source — direct or expander)
        master_pin = await cg.gpio_pin_expression(master_conf[CONF_PIN])
        cg.add(ctrl.set_master_pin(master_pin))

        # Master state sensor (read-only binary_sensor)
        master_bs = cg.new_Pvariable(master_conf[CONF_ID])
        await cg.register_component(master_bs, {})
        cg.add(cg.App.register_binary_sensor(master_bs))
        CORE.register_platform_component("binary_sensor", master_bs)
        await setup_entity(
            master_bs,
            _entity_config(master_name, master_icon),
            "binary_sensor",
        )
        cg.add(ctrl.set_master_binary_sensor(master_bs))

    # ── Per-zone entities ─────────────────────────────────────────────────
    for valve_conf in config[CONF_VALVES]:
        valve_name = valve_conf[CONF_NAME]

        # Zone output pin
        zone_pin = await cg.gpio_pin_expression(valve_conf[CONF_PIN])

        # Zone valve — native HA valve entity.
        # No icon override: HA uses state-based valve icons by default
        # (closed -> mdi:valve-closed, open -> mdi:valve-open).
        zone_valve = cg.new_Pvariable(valve_conf["valve_id"])
        await cg.register_component(zone_valve, {})
        cg.add(cg.App.register_valve(zone_valve))
        CORE.register_platform_component("valve", zone_valve)
        await setup_entity(
            zone_valve,
            _entity_config(valve_name),
            "valve",
        )

        # Zone duration number (rendered as numeric input via mode=BOX)
        zone_num = cg.new_Pvariable(valve_conf["number_id"])
        await cg.register_component(zone_num, {})
        cg.add(zone_num.traits.set_min_value(1.0))
        cg.add(zone_num.traits.set_max_value(3600.0))
        cg.add(zone_num.traits.set_step(1.0))
        cg.add(zone_num.traits.set_mode(NumberMode.NUMBER_MODE_BOX))
        cg.add(cg.App.register_number(zone_num))
        CORE.register_platform_component("number", zone_num)
        await setup_entity(
            zone_num,
            _entity_config(f"{valve_name} {DURATION_SUFFIX}", ICON_DURATION),
            "number",
        )
        cg.add(zone_num.set_initial_value(float(valve_conf[CONF_INITIAL_DURATION])))
        cg.add(zone_num.set_restore_value(True))

        cg.add(ctrl.add_zone(zone_valve, zone_num, zone_pin))
