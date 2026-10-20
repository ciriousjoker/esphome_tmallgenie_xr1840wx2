import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from .. import (
    BleMeshLight,
    CONF_APP_IDX,
    CONF_APP_KEY,
    CONF_CONTROLLER_ADDRESS,
    CONF_IV_INDEX,
    CONF_NET_IDX,
    CONF_NET_KEY,
    CONF_TARGET_ADDRESS,
    validate_key,
    validate_key_index,
    validate_u16,
    validate_u32,
)


CONF_PAIRING_MAC = "pairing_mac"
CONF_PAIRING_ADDRESS = "pairing_controller_address"

def validate_addresses(config):
    target = config[CONF_TARGET_ADDRESS]
    controller = config[CONF_CONTROLLER_ADDRESS]
    if target >= 0x7FFF:
        raise cv.Invalid("target_address must leave room for the CTL temperature element")
    if controller in (target, target + 1):
        raise cv.Invalid(
            "controller_address must not overlap the lamp's primary or CTL element"
        )
    if CONF_PAIRING_MAC in config:
        if CONF_PAIRING_ADDRESS not in config:
            raise cv.Invalid("pairing_controller_address is required with pairing_mac")
        if config[CONF_PAIRING_ADDRESS] in (target, target + 1, controller):
            raise cv.Invalid("pairing_controller_address must be a separate unused unicast address")
        if config[CONF_NET_IDX] != 0 or config[CONF_IV_INDEX] > 255:
            raise cv.Invalid("Vendor pairing supports NetKey index 0 and IV indexes 0..255")
    elif CONF_PAIRING_ADDRESS in config:
        raise cv.Invalid("pairing_mac is required with pairing_controller_address")
    return config

CONFIG_SCHEMA = cv.All(
    light.light_schema(
        BleMeshLight,
        light.LightType.BINARY,
        default_restore_mode="RESTORE_DEFAULT_OFF",
    ).extend(
        {
            cv.Required(CONF_NET_KEY): cv.sensitive(validate_key),
            cv.Required(CONF_APP_KEY): cv.sensitive(validate_key),
            cv.Required(CONF_TARGET_ADDRESS): validate_u16,
            cv.Required(CONF_CONTROLLER_ADDRESS): validate_u16,
            cv.Optional(CONF_PAIRING_MAC): cv.mac_address,
            cv.Optional(CONF_PAIRING_ADDRESS): validate_u16,
            cv.Optional(CONF_IV_INDEX, default="0"): validate_u32,
            cv.Optional(CONF_NET_IDX, default="0"): validate_key_index,
            cv.Optional(CONF_APP_IDX, default="0"): validate_key_index,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_addresses,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    cg.add(var.set_net_key(config[CONF_NET_KEY]))
    cg.add(var.set_app_key(config[CONF_APP_KEY]))
    cg.add(var.set_target_address(config[CONF_TARGET_ADDRESS]))
    cg.add(var.set_controller_address(config[CONF_CONTROLLER_ADDRESS]))
    cg.add(var.set_iv_index(config[CONF_IV_INDEX]))
    cg.add(var.set_net_idx(config[CONF_NET_IDX]))
    cg.add(var.set_app_idx(config[CONF_APP_IDX]))

    if CONF_PAIRING_MAC in config:
        cg.add(var.set_pairing_mac(str(config[CONF_PAIRING_MAC])))
        cg.add(var.set_pairing_controller_address(config[CONF_PAIRING_ADDRESS]))
