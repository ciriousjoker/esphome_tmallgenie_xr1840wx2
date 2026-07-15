import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from .. import BleMeshLight, ble_mesh_ns

CONF_BLE_MESH_LIGHT_ID = "ble_mesh_light_id"
CONF_REMOTE_BUTTON = "remote_button"

REMOTE_BUTTONS = {
    "on": 1,
    "off": 2,
    "brightness_up": 3,
    "temperature_down": 4,
    "setup": 5,
    "temperature_up": 6,
    "brightness_down": 7,
    "day_mode": 8,
    "night_mode": 9,
    "main_light_on": 10,
    "main_light_off": 11,
    # Backwards-compatible aliases used by the reverse-engineering firmware.
    "mode_a": 10,
    "mode_b": 11,
    "off_timer_60s": 12,
}

BleMeshButton = ble_mesh_ns.class_("BleMeshButton", button.Button)

CONFIG_SCHEMA = button.button_schema(BleMeshButton).extend(
    {
        cv.Required(CONF_BLE_MESH_LIGHT_ID): cv.use_id(BleMeshLight),
        cv.Required(CONF_REMOTE_BUTTON): cv.enum(REMOTE_BUTTONS, lower=True),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    parent = await cg.get_variable(config[CONF_BLE_MESH_LIGHT_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_button_number(config[CONF_REMOTE_BUTTON]))
