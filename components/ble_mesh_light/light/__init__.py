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


def validate_addresses(config):
    target = config[CONF_TARGET_ADDRESS]
    controller = config[CONF_CONTROLLER_ADDRESS]
    if target >= 0x7FFF:
        raise cv.Invalid("target_address must leave room for the CTL temperature element")
    if controller in (target, target + 1):
        raise cv.Invalid(
            "controller_address must not overlap the lamp's primary or CTL element"
        )
    return config

CONFIG_SCHEMA = cv.All(
    light.LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(BleMeshLight),
            cv.Required(CONF_NET_KEY): validate_key,
            cv.Required(CONF_APP_KEY): validate_key,
            cv.Required(CONF_TARGET_ADDRESS): validate_u16,
            cv.Required(CONF_CONTROLLER_ADDRESS): validate_u16,
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
