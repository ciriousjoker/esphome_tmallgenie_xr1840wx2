import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from .. import BleMeshLight, ble_mesh_ns

BleMeshPairingStatusTextSensor = ble_mesh_ns.class_(
    "BleMeshPairingStatusTextSensor", text_sensor.TextSensor
)
CONFIG_SCHEMA = text_sensor.text_sensor_schema(BleMeshPairingStatusTextSensor).extend(
    {cv.Required("ble_mesh_light_id"): cv.use_id(BleMeshLight)}
)

async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    parent = await cg.get_variable(config["ble_mesh_light_id"])
    cg.add(parent.set_pairing_status_sensor(var))
