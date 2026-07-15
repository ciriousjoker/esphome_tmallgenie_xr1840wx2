import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light as esphome_light
from esphome.components.esp32 import add_idf_sdkconfig_option

CONF_NET_KEY = "net_key"
CONF_APP_KEY = "app_key"
CONF_TARGET_ADDRESS = "target_address"
CONF_CONTROLLER_ADDRESS = "controller_address"
CONF_IV_INDEX = "iv_index"
CONF_NET_IDX = "net_idx"
CONF_APP_IDX = "app_idx"

ble_mesh_ns = cg.esphome_ns.namespace("ble_mesh_light")
BleMeshLight = ble_mesh_ns.class_("BleMeshLight", esphome_light.LightOutput, cg.Component)


def validate_key(value):
    value = cv.string_strict(value).replace(":", "").replace(" ", "").upper()
    if not re.fullmatch(r"[0-9A-F]{32}", value):
        raise cv.Invalid("Bluetooth Mesh keys must contain exactly 16 hexadecimal bytes")
    return value


def validate_u16(value):
    value = cv.string_strict(value)
    number = int(value, 0)
    if number < 1 or number > 0x7FFF:
        raise cv.Invalid("Unicast addresses must be between 0x0001 and 0x7FFF")
    return number


def validate_u32(value):
    value = cv.string_strict(value)
    number = int(value, 0)
    if number < 0 or number > 0xFFFFFFFF:
        raise cv.Invalid("IV Index must fit in 32 bits")
    return number


def validate_key_index(value):
    value = cv.string_strict(value)
    number = int(value, 0)
    if number < 0 or number > 0x0FFF:
        raise cv.Invalid("Bluetooth Mesh key indexes must be between 0x000 and 0xFFF")
    return number


DEPENDENCIES = ["esp32"]
MULTI_CONF = False
CONFIG_SCHEMA = cv.All(cv.Schema({}), cv.only_with_esp_idf)


async def to_code(config):
    # Keep all ESP-IDF requirements inside the external component so users do
    # not need to copy a fragile sdkconfig_options block into their YAML.
    options = {
        "CONFIG_BT_ENABLED": True,
        "CONFIG_BT_BLUEDROID_ENABLED": True,
        "CONFIG_BT_NIMBLE_ENABLED": False,
        "CONFIG_BT_BLE_ENABLED": True,
        "CONFIG_BLE_MESH": True,
        "CONFIG_BLE_MESH_NODE": True,
        "CONFIG_BLE_MESH_PROVISIONER": False,
        "CONFIG_BLE_MESH_CFG_CLI": False,
        "CONFIG_BLE_MESH_SETTINGS": False,
        "CONFIG_BLE_MESH_SELF_TEST": True,
        "CONFIG_BLE_MESH_TEST_AUTO_ENTER_NETWORK": True,
        "CONFIG_BLE_MESH_GENERIC_ONOFF_CLI": True,
        "CONFIG_BLE_MESH_LIGHT_LIGHTNESS_CLI": True,
        "CONFIG_BLE_MESH_LIGHT_CTL_CLI": True,
    }
    for option, value in options.items():
        add_idf_sdkconfig_option(option, value)
