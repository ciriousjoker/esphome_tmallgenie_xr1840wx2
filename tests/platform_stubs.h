#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_LOGI(tag, ...) ((void) (tag))
typedef void *QueueHandle_t;
#define pdTRUE 1
#define NVS_READWRITE 1
typedef int nvs_handle_t;
QueueHandle_t xQueueCreate(size_t, size_t);
int xQueueSend(QueueHandle_t, const void *, int);
int xQueueReceive(QueueHandle_t, void *, int);
int xQueueReset(QueueHandle_t);
int nvs_open(const char *, int, nvs_handle_t *);
int nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
int nvs_set_u32(nvs_handle_t, const char *, uint32_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
void esp_fill_random(void *, size_t);
int64_t esp_timer_get_time(void);
#define ESP_BLE_MESH_BLE_ADV_PRIO_HIGH 1
#define ESP_BLE_MESH_BLE_ADV_PRIO_LOW 0
#define ESP_BLE_MESH_START_BLE_ADVERTISING_COMP_EVT 1
#define ESP_BLE_MESH_START_BLE_SCANNING_COMP_EVT 2
#define ESP_BLE_MESH_SCAN_BLE_ADVERTISING_PKT_EVT 3
typedef int esp_ble_mesh_ble_cb_event_t;
typedef struct { uint8_t addr[6]; uint8_t *data; uint16_t length; } esp_ble_mesh_ble_adv_rpt_t;
typedef union {
 struct { int err_code; uint8_t index; } start_ble_advertising_comp;
 struct { int err_code; } start_ble_scan_comp;
 esp_ble_mesh_ble_adv_rpt_t scan_ble_adv_pkt;
} esp_ble_mesh_ble_cb_param_t;
typedef void (*esp_ble_mesh_ble_cb_t)(int, esp_ble_mesh_ble_cb_param_t *);
typedef struct { uint16_t interval; uint8_t adv_type, own_addr_type; uint16_t duration, period, count; uint8_t priority; } esp_ble_mesh_ble_adv_param_t;
typedef struct { uint8_t adv_data_len, adv_data[31]; } esp_ble_mesh_ble_adv_data_t;
typedef struct { uint32_t duration; } esp_ble_mesh_ble_scan_param_t;
int esp_ble_mesh_register_ble_callback(esp_ble_mesh_ble_cb_t);
int esp_ble_mesh_start_ble_scanning(esp_ble_mesh_ble_scan_param_t *);
int esp_ble_mesh_start_ble_advertising(const esp_ble_mesh_ble_adv_param_t *, const esp_ble_mesh_ble_adv_data_t *);
