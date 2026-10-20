#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_mesh_pairing_status_callback_t)(const char *status, void *context);
int ble_mesh_pairing_configure(const uint8_t mac[6], uint16_t pairing_address,
                               const uint8_t net_key[16], const uint8_t app_key[16],
                               uint16_t target, uint16_t controller, uint32_t iv,
                               uint16_t net_index, uint16_t app_index,
                               ble_mesh_pairing_status_callback_t callback, void *context);
int ble_mesh_pairing_radio_start(void);
int ble_mesh_pairing_start(void);
void ble_mesh_pairing_poll(void);
bool ble_mesh_pairing_busy(void);
// Only call after an authenticated application response from the configured lamp.
void ble_mesh_pairing_confirm_existing(void);
#ifdef __cplusplus
}
#endif
