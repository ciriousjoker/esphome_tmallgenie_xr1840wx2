#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_mesh_bridge_ready_callback_t)(bool ready, void *context);
typedef void (*ble_mesh_bridge_onoff_callback_t)(bool on, void *context);
typedef void (*ble_mesh_bridge_lightness_callback_t)(uint16_t lightness, void *context);
typedef void (*ble_mesh_bridge_temperature_callback_t)(uint16_t temperature, void *context);

int ble_mesh_bridge_init(const uint8_t net_key[16], const uint8_t app_key[16],
                         uint16_t target_address, uint16_t controller_address,
                         uint32_t iv_index, uint16_t net_idx, uint16_t app_idx,
                         ble_mesh_bridge_ready_callback_t ready_callback,
                         ble_mesh_bridge_onoff_callback_t onoff_callback,
                         ble_mesh_bridge_lightness_callback_t lightness_callback,
                         ble_mesh_bridge_temperature_callback_t temperature_callback,
                         void *callback_context);
int ble_mesh_bridge_send_onoff(bool on);
int ble_mesh_bridge_get_onoff(void);
int ble_mesh_bridge_send_lightness(uint16_t lightness);
int ble_mesh_bridge_get_lightness(void);
int ble_mesh_bridge_send_vendor_main_light(bool enabled);
int ble_mesh_bridge_send_ctl(uint16_t lightness, uint16_t temperature);
int ble_mesh_bridge_get_ctl(void);

#ifdef __cplusplus
}
#endif
