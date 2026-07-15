import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_LIGHT

from .. import BleMeshLight, ble_mesh_ns

CONF_BLE_MESH_LIGHT_ID = "ble_mesh_light_id"

BleMeshStateBinarySensor = ble_mesh_ns.class_(
    "BleMeshStateBinarySensor", binary_sensor.BinarySensor
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    BleMeshStateBinarySensor,
    device_class=DEVICE_CLASS_LIGHT,
).extend(
    {
        cv.Required(CONF_BLE_MESH_LIGHT_ID): cv.use_id(BleMeshLight),
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_BLE_MESH_LIGHT_ID])
    cg.add(parent.set_state_binary_sensor(var))
