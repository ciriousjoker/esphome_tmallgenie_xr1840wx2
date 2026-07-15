#include "ble_mesh_bridge.h"

#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ESP-IDF deliberately exposes this helper only behind its BLE Mesh self-test
// option. It performs exactly the operation needed here: enter a known Mesh
// network when all provisioning material was captured from our own lamp.
struct bt_mesh_device_network_info {
  uint8_t net_key[16];
  uint16_t net_idx;
  uint8_t flags;
  uint32_t iv_index;
  uint16_t unicast_addr;
  uint8_t dev_key[16];
  uint8_t app_key[16];
  uint16_t app_idx;
  uint16_t group_addr;
};

extern int bt_mesh_device_auto_enter_network(struct bt_mesh_device_network_info *info);
extern void bt_mesh_test_set_seq(uint32_t seq);

static const char *TAG = "ble_mesh_bridge";
static const uint16_t LOCAL_GROUP = 0xC000;

#define ALIGENIE_VENDOR_MODEL_CLIENT_ID 0x0001
#define ALIGENIE_OP_ATTR_GET_STATUS ESP_BLE_MESH_MODEL_OP_3(0xD0, 0x01A8)
#define ALIGENIE_OP_ATTR_SET_ACK ESP_BLE_MESH_MODEL_OP_3(0xD1, 0x01A8)
#define ALIGENIE_OP_ATTR_STATUS ESP_BLE_MESH_MODEL_OP_3(0xD3, 0x01A8)
#define ALIGENIE_OP_ATTR_INDICATION ESP_BLE_MESH_MODEL_OP_3(0xD4, 0x01A8)
#define ALIGENIE_ATTR_ONOFF 0x0100
#define ALIGENIE_ATTR_LIGHTNESS 0x0121
#define ALIGENIE_ATTR_TEMPERATURE 0x0122
#define ALIGENIE_ATTR_MAIN_LIGHT 0x0534

static uint8_t device_uuid[16] = {0x45, 0x53, 0x50, 0x48, 0x4f, 0x4d, 0x45, 0x2d,
                                  0x4d, 0x45, 0x53, 0x48, 0x2d, 0x30, 0x30, 0x32};
static uint8_t local_device_key[16];
static uint8_t net_key[16];
static uint8_t app_key[16];
static uint16_t target_address;
static uint16_t controller_address;
static uint16_t mesh_net_idx;
static uint16_t mesh_app_idx;
static uint32_t mesh_iv_index;
static uint8_t transaction_id;
static uint8_t vendor_transaction_id = 1;
static bool ready;
static bool onoff_request_in_flight;
static bool onoff_get_in_flight;
static bool onoff_request_queued;
static bool queued_onoff;
static bool active_onoff;
static uint8_t onoff_retries_remaining;
static esp_timer_handle_t onoff_drain_timer;
static bool lightness_request_in_flight;
static bool lightness_get_in_flight;
static bool lightness_request_queued;
static uint16_t queued_lightness;
static uint16_t active_lightness;
static uint8_t lightness_retries_remaining;
static esp_timer_handle_t lightness_drain_timer;
static bool ctl_request_in_flight;
static bool ctl_get_in_flight;
static bool ctl_request_queued;
static uint16_t queued_ctl_lightness;
static uint16_t queued_ctl_temperature;
static uint16_t active_ctl_lightness;
static uint16_t active_ctl_temperature;
static uint8_t ctl_retries_remaining;
static esp_timer_handle_t ctl_drain_timer;
static SemaphoreHandle_t sequence_mutex;
static uint32_t sequence_numbers_remaining;
static ble_mesh_bridge_ready_callback_t ready_callback;
static ble_mesh_bridge_onoff_callback_t onoff_callback;
static ble_mesh_bridge_lightness_callback_t lightness_callback;
static ble_mesh_bridge_temperature_callback_t temperature_callback;
static void *ready_context;

static esp_ble_mesh_client_t onoff_client;
static esp_ble_mesh_client_t lightness_client;
static esp_ble_mesh_client_t ctl_client;

static const esp_ble_mesh_client_op_pair_t vendor_op_pair[] = {
    {ALIGENIE_OP_ATTR_GET_STATUS, ALIGENIE_OP_ATTR_STATUS},
    {ALIGENIE_OP_ATTR_SET_ACK, ALIGENIE_OP_ATTR_STATUS},
};

static esp_ble_mesh_client_t vendor_client = {
    .op_pair_size = ARRAY_SIZE(vendor_op_pair),
    .op_pair = vendor_op_pair,
};

static esp_ble_mesh_model_op_t vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(ALIGENIE_OP_ATTR_STATUS, 1),
    ESP_BLE_MESH_MODEL_OP(ALIGENIE_OP_ATTR_INDICATION, 1),
    ESP_BLE_MESH_MODEL_OP_END,
};

static int send_onoff_acknowledged(bool on);
static void drain_onoff_queue(void *context);
static int send_lightness_acknowledged(uint16_t lightness);
static void drain_lightness_queue(void *context);
static int send_ctl_acknowledged(uint16_t lightness, uint16_t temperature);
static void drain_ctl_queue(void *context);
static int prepare_sequence_number(void);

static void schedule_onoff_drain(void) {
  if (onoff_drain_timer == NULL || esp_timer_is_active(onoff_drain_timer))
    return;
  const esp_err_t err = esp_timer_start_once(onoff_drain_timer, 500000);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Unable to schedule queued Generic OnOff: %s",
             esp_err_to_name(err));
}

static void finish_onoff_request(bool response_received, bool present_onoff) {
  onoff_request_in_flight = false;

  if (!onoff_request_queued) {
    if (!response_received && onoff_retries_remaining > 0) {
      onoff_retries_remaining--;
      onoff_request_queued = true;
      queued_onoff = active_onoff;
      ESP_LOGW(TAG, "Retrying Generic OnOff %s (%u retries left)",
               active_onoff ? "ON" : "OFF", onoff_retries_remaining);
      schedule_onoff_drain();
    }
    return;
  }

  // Repeated presses of the same button while its request was in flight do not
  // need another Mesh transaction once the status already confirms the state.
  if (response_received && present_onoff == queued_onoff) {
    const bool satisfied_onoff = queued_onoff;
    onoff_request_queued = false;
    ESP_LOGD(TAG, "Queued Generic OnOff already satisfied (%s)",
             satisfied_onoff ? "ON" : "OFF");
    return;
  }

  // Do not invoke a second client transaction from inside the BTC callback.
  // The model can still be internally busy until that callback returns.
  schedule_onoff_drain();
}

static void drain_onoff_queue(void *context) {
  (void) context;
  if (!ready || onoff_request_in_flight || !onoff_request_queued)
    return;
  if (onoff_get_in_flight) {
    schedule_onoff_drain();
    return;
  }

  const bool requested_onoff = queued_onoff;
  onoff_request_queued = false;
  const int err = send_onoff_acknowledged(requested_onoff);
  if (err == ESP_OK)
    return;

  ESP_LOGW(TAG, "Unable to drain queued Generic OnOff (%s): %s",
           requested_onoff ? "ON" : "OFF", esp_err_to_name(err));
  if (onoff_retries_remaining > 0) {
    onoff_retries_remaining--;
    onoff_request_queued = true;
    queued_onoff = requested_onoff;
    schedule_onoff_drain();
  }
}

static void schedule_lightness_drain(void) {
  if (lightness_drain_timer == NULL || esp_timer_is_active(lightness_drain_timer))
    return;
  const esp_err_t err = esp_timer_start_once(lightness_drain_timer, 500000);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Unable to schedule queued Light Lightness request: %s",
             esp_err_to_name(err));
}

static void finish_lightness_request(bool response_received, uint16_t present_lightness) {
  lightness_request_in_flight = false;

  if (!lightness_request_queued) {
    if (!response_received && lightness_retries_remaining > 0) {
      lightness_retries_remaining--;
      lightness_request_queued = true;
      queued_lightness = active_lightness;
      ESP_LOGW(TAG, "Retrying Light Lightness %u (%u retries left)",
               active_lightness, lightness_retries_remaining);
      schedule_lightness_drain();
    }
    return;
  }

  if (response_received && present_lightness == queued_lightness) {
    lightness_request_queued = false;
    return;
  }

  schedule_lightness_drain();
}

static void drain_lightness_queue(void *context) {
  (void) context;
  if (!ready || lightness_request_in_flight || !lightness_request_queued)
    return;
  if (lightness_get_in_flight) {
    schedule_lightness_drain();
    return;
  }

  const uint16_t requested_lightness = queued_lightness;
  lightness_request_queued = false;
  const int err = send_lightness_acknowledged(requested_lightness);
  if (err == ESP_OK)
    return;

  ESP_LOGW(TAG, "Unable to drain queued Light Lightness %u: %s",
           requested_lightness, esp_err_to_name(err));
  if (lightness_retries_remaining > 0) {
    lightness_retries_remaining--;
    lightness_request_queued = true;
    queued_lightness = requested_lightness;
    schedule_lightness_drain();
  }
}

static void schedule_ctl_drain(void) {
  if (ctl_drain_timer == NULL || esp_timer_is_active(ctl_drain_timer))
    return;
  const esp_err_t err = esp_timer_start_once(ctl_drain_timer, 500000);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Unable to schedule queued Light CTL request: %s",
             esp_err_to_name(err));
}

static void finish_ctl_request(bool response_received, uint16_t confirmed_lightness,
                               uint16_t confirmed_temperature) {
  ctl_request_in_flight = false;

  if (!ctl_request_queued) {
    if (!response_received && ctl_retries_remaining > 0) {
      ctl_retries_remaining--;
      ctl_request_queued = true;
      queued_ctl_lightness = active_ctl_lightness;
      queued_ctl_temperature = active_ctl_temperature;
      ESP_LOGW(TAG, "Retrying Light CTL %u/%u (%u retries left)",
               active_ctl_lightness, active_ctl_temperature, ctl_retries_remaining);
      schedule_ctl_drain();
    }
    return;
  }

  if (response_received && confirmed_lightness == queued_ctl_lightness &&
      confirmed_temperature == queued_ctl_temperature) {
    ctl_request_queued = false;
    return;
  }

  schedule_ctl_drain();
}

static void drain_ctl_queue(void *context) {
  (void) context;
  if (!ready || ctl_request_in_flight || !ctl_request_queued)
    return;
  if (ctl_get_in_flight) {
    schedule_ctl_drain();
    return;
  }

  const uint16_t lightness = queued_ctl_lightness;
  const uint16_t temperature = queued_ctl_temperature;
  ctl_request_queued = false;
  const int err = send_ctl_acknowledged(lightness, temperature);
  if (err == ESP_OK)
    return;

  ESP_LOGW(TAG, "Unable to drain queued Light CTL %u/%u: %s",
           lightness, temperature, esp_err_to_name(err));
  if (ctl_retries_remaining > 0) {
    ctl_retries_remaining--;
    ctl_request_queued = true;
    queued_ctl_lightness = lightness;
    queued_ctl_temperature = temperature;
    schedule_ctl_drain();
  }
}

// Bluetooth Mesh receivers reject sequence numbers they have already seen from
// a source address. The captured-network entry helper starts at zero on every
// boot, so allocate NVS-backed ranges. Ranges are advanced both on reboot and
// while running, avoiding sequence reuse during long uptimes.
static int reserve_sequence_range_locked(void) {
  static const uint32_t SEQUENCE_STRIDE = 0x1000;
  nvs_handle_t handle;
  esp_err_t err = nvs_open("lamp_mesh", NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;

  uint32_t previous_base = 0;
  err = nvs_get_u32(handle, "seq_base", &previous_base);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    previous_base = 0;
  } else if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }

  if (previous_base > 0xFFFFFF - SEQUENCE_STRIDE) {
    nvs_close(handle);
    return ESP_ERR_INVALID_SIZE;
  }

  const uint32_t sequence_base = previous_base + SEQUENCE_STRIDE;
  err = nvs_set_u32(handle, "seq_base", sequence_base);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK)
    return err;

  bt_mesh_test_set_seq(sequence_base);
  sequence_numbers_remaining = SEQUENCE_STRIDE;
  ESP_LOGD(TAG, "Reserved Mesh sequence range 0x%06" PRIX32 "-0x%06" PRIX32,
           sequence_base, sequence_base + SEQUENCE_STRIDE - 1);
  return ESP_OK;
}

static int initialize_sequence_range(void) {
  if (sequence_mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(sequence_mutex, portMAX_DELAY);
  const int err = reserve_sequence_range_locked();
  xSemaphoreGive(sequence_mutex);
  return err;
}

static int prepare_sequence_number(void) {
  if (sequence_mutex == NULL)
    return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(sequence_mutex, portMAX_DELAY);
  int err = ESP_OK;
  if (sequence_numbers_remaining == 0)
    err = reserve_sequence_range_locked();
  if (err == ESP_OK)
    sequence_numbers_remaining--;
  xSemaphoreGive(sequence_mutex);
  return err;
}

static esp_ble_mesh_cfg_srv_t config_server = {
    // Unacknowledged control messages do not occupy a client transaction slot.
    // Repeat every packet at the network layer to tolerate Wi-Fi/BLE coexistence
    // and the lamp's unusually narrow receive windows.
    .net_transmit = ESP_BLE_MESH_TRANSMIT(5, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 3,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_client),
    ESP_BLE_MESH_MODEL_LIGHT_LIGHTNESS_CLI(NULL, &lightness_client),
    ESP_BLE_MESH_MODEL_LIGHT_CTL_CLI(NULL, &ctl_client),
};

static esp_ble_mesh_model_t vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(0x01A8, ALIGENIE_VENDOR_MODEL_CLIENT_ID,
                              vendor_ops, NULL, &vendor_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vendor_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = 0x02E5,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

static esp_ble_mesh_prov_t provisioning = {
    .uuid = device_uuid,
};

static void report_ready(bool value) {
  ready = value;
  if (ready_callback != NULL)
    ready_callback(value, ready_context);
}

static void report_onoff(bool value) {
  if (onoff_callback != NULL)
    onoff_callback(value, ready_context);
}

static void report_lightness(uint16_t value) {
  if (lightness_callback != NULL)
    lightness_callback(value, ready_context);
}

static void report_temperature(uint16_t value) {
  if (temperature_callback != NULL)
    temperature_callback(value, ready_context);
}

static void generic_client_callback(esp_ble_mesh_generic_client_cb_event_t event,
                                    esp_ble_mesh_generic_client_cb_param_t *param) {
  if (param == NULL || param->params == NULL) {
    ESP_LOGE(TAG, "Generic client callback without parameters (event=%u)", event);
    return;
  }

  const uint32_t opcode = param->params->opcode;
  const uint16_t address = param->params->ctx.addr;

  switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
      if (param->error_code != ESP_OK) {
        ESP_LOGW(TAG, "Mesh response error: event=%u opcode=0x%04" PRIX32
                      " address=0x%04X error=%d",
                 event, opcode, address, param->error_code);
        if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET)
          finish_onoff_request(false, false);
        else if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
          onoff_get_in_flight = false;
          schedule_onoff_drain();
        }
        return;
      }
      if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ||
          opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
        const bool present_onoff = param->status_cb.onoff_status.present_onoff;
        ESP_LOGD(TAG, "Status from 0x%04X: Generic OnOff is %s%s", address,
                 present_onoff ? "ON" : "OFF",
                 param->status_cb.onoff_status.op_en ? " (transition active)" : "");
        if (address == target_address)
          report_onoff(present_onoff);
        if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET)
          finish_onoff_request(true, present_onoff);
        else {
          onoff_get_in_flight = false;
          schedule_onoff_drain();
        }
      } else {
        ESP_LOGD(TAG, "Mesh response from 0x%04X: opcode=0x%04" PRIX32,
                 address, opcode);
      }
      break;
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
      if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
        const bool present_onoff = param->status_cb.onoff_status.present_onoff;
        ESP_LOGD(TAG, "OnOff publication from 0x%04X: %s", address,
                 present_onoff ? "ON" : "OFF");
        if (address == target_address)
          report_onoff(present_onoff);
      } else {
        ESP_LOGD(TAG, "Mesh publish from 0x%04X for opcode=0x%04" PRIX32,
                 address, opcode);
      }
      break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
      ESP_LOGW(TAG, "No response from 0x%04X for opcode=0x%04" PRIX32,
               address, opcode);
      if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET)
        finish_onoff_request(false, false);
      else if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
        onoff_get_in_flight = false;
        schedule_onoff_drain();
      }
      break;
    default:
      ESP_LOGD(TAG, "Unexpected Generic Client event=%u opcode=0x%04" PRIX32,
               event, opcode);
      break;
  }
}

static void light_client_callback(esp_ble_mesh_light_client_cb_event_t event,
                                  esp_ble_mesh_light_client_cb_param_t *param) {
  if (param == NULL || param->params == NULL) {
    ESP_LOGE(TAG, "Light client callback without parameters (event=%u)", event);
    return;
  }

  const uint32_t opcode = param->params->opcode;
  const uint16_t address = param->params->ctx.addr;

  if (event == ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT) {
    ESP_LOGW(TAG, "No light response from 0x%04X for opcode=0x%04" PRIX32,
             address, opcode);
    if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET)
      finish_lightness_request(false, 0);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET) {
      lightness_get_in_flight = false;
      schedule_lightness_drain();
    }
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET)
      finish_ctl_request(false, 0, 0);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET) {
      ctl_get_in_flight = false;
      schedule_ctl_drain();
    }
    return;
  }
  if (param->error_code != ESP_OK) {
    ESP_LOGW(TAG, "Light response error: event=%u opcode=0x%04" PRIX32
                  " address=0x%04X error=%d",
             event, opcode, address, param->error_code);
    if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET)
      finish_lightness_request(false, 0);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET) {
      lightness_get_in_flight = false;
      schedule_lightness_drain();
    }
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET)
      finish_ctl_request(false, 0, 0);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET) {
      ctl_get_in_flight = false;
      schedule_ctl_drain();
    }
    return;
  }

  if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET ||
      opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET ||
      opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
    const esp_ble_mesh_light_lightness_status_cb_t *status =
        &param->status_cb.lightness_status;
    ESP_LOGD(TAG, "Lightness status from 0x%04X: present=%u target=%u transition=%s",
             address, status->present_lightness,
             status->op_en ? status->target_lightness : status->present_lightness,
             status->op_en ? "yes" : "no");
    if (address == target_address)
      report_lightness(status->op_en ? status->target_lightness
                                     : status->present_lightness);
    if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET)
      finish_lightness_request(true,
                               status->op_en ? status->target_lightness
                                             : status->present_lightness);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET) {
      lightness_get_in_flight = false;
      schedule_lightness_drain();
    }
  } else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_RANGE_GET ||
             opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_RANGE_STATUS) {
    const esp_ble_mesh_light_lightness_range_status_cb_t *status =
        &param->status_cb.lightness_range_status;
    ESP_LOGD(TAG, "Lightness range from 0x%04X: status=%u min=%u max=%u",
             address, status->status_code, status->range_min, status->range_max);
  } else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET ||
             opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET ||
             opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_STATUS) {
    const esp_ble_mesh_light_ctl_status_cb_t *status = &param->status_cb.ctl_status;
    ESP_LOGD(TAG, "CTL status from 0x%04X: present=%u/%uK target=%u/%uK transition=%s",
             address, status->present_ctl_lightness, status->present_ctl_temperature,
             status->op_en ? status->target_ctl_lightness : status->present_ctl_lightness,
             status->op_en ? status->target_ctl_temperature : status->present_ctl_temperature,
             status->op_en ? "yes" : "no");
    if (address == target_address)
      report_temperature(status->op_en ? status->target_ctl_temperature
                                       : status->present_ctl_temperature);
    if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET)
      finish_ctl_request(true,
                         status->op_en ? status->target_ctl_lightness
                                       : status->present_ctl_lightness,
                         status->op_en ? status->target_ctl_temperature
                                       : status->present_ctl_temperature);
    else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET) {
      ctl_get_in_flight = false;
      schedule_ctl_drain();
    }
  } else {
    ESP_LOGD(TAG, "Light response from 0x%04X: opcode=0x%04" PRIX32,
             address, opcode);
  }
}

static void parse_aligenie_vendor_status(uint16_t source, const uint8_t *data, uint16_t length) {
  if (source != target_address || data == NULL || length < 1)
    return;

  const uint8_t tid = data[0];
  uint16_t offset = 1;
  while (offset + 2 <= length) {
    const uint16_t attribute = data[offset] | ((uint16_t) data[offset + 1] << 8);
    offset += 2;
    if (attribute == ALIGENIE_ATTR_ONOFF && offset + 1 <= length) {
      const bool on = data[offset++] != 0;
      ESP_LOGD(TAG, "AliGenie status from 0x%04X tid=%u: OnOff=%s",
               source, tid, on ? "ON" : "OFF");
      report_onoff(on);
    } else if (attribute == ALIGENIE_ATTR_LIGHTNESS && offset + 2 <= length) {
      const uint16_t lightness = data[offset] | ((uint16_t) data[offset + 1] << 8);
      offset += 2;
      ESP_LOGD(TAG, "AliGenie status from 0x%04X tid=%u: lightness=%u",
               source, tid, lightness);
      report_lightness(lightness);
    } else if (attribute == ALIGENIE_ATTR_TEMPERATURE && offset + 2 <= length) {
      const uint16_t temperature = data[offset] | ((uint16_t) data[offset + 1] << 8);
      offset += 2;
      ESP_LOGD(TAG, "AliGenie status from 0x%04X tid=%u: temperature=%u",
               source, tid, temperature);
      report_temperature(temperature);
    } else if (attribute == ALIGENIE_ATTR_MAIN_LIGHT && offset + 1 <= length) {
      const bool enabled = data[offset++] != 0;
      ESP_LOGD(TAG, "AliGenie status from 0x%04X tid=%u: MainLight=%s",
               source, tid, enabled ? "ON" : "OFF");
    } else {
      ESP_LOGD(TAG, "AliGenie status from 0x%04X tid=%u: unknown attr=0x%04X",
               source, tid, attribute);
      break;
    }
  }
}

static void custom_model_callback(esp_ble_mesh_model_cb_event_t event,
                                  esp_ble_mesh_model_cb_param_t *param) {
  if (param == NULL)
    return;

  switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
      if (param->model_operation.opcode == ALIGENIE_OP_ATTR_STATUS ||
          param->model_operation.opcode == ALIGENIE_OP_ATTR_INDICATION) {
        parse_aligenie_vendor_status(param->model_operation.ctx->addr,
                                     param->model_operation.msg,
                                     param->model_operation.length);
      }
      break;
    case ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT:
      if (param->client_recv_publish_msg.opcode == ALIGENIE_OP_ATTR_STATUS ||
          param->client_recv_publish_msg.opcode == ALIGENIE_OP_ATTR_INDICATION) {
        parse_aligenie_vendor_status(param->client_recv_publish_msg.ctx->addr,
                                     param->client_recv_publish_msg.msg,
                                     param->client_recv_publish_msg.length);
      }
      break;
    case ESP_BLE_MESH_CLIENT_MODEL_SEND_TIMEOUT_EVT:
      ESP_LOGW(TAG, "AliGenie vendor timeout from 0x%04X for opcode=0x%06" PRIX32,
               param->client_send_timeout.ctx->addr,
               param->client_send_timeout.opcode);
      break;
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
      if (param->model_send_comp.err_code != ESP_OK)
        ESP_LOGE(TAG, "AliGenie vendor send failed: %d",
                 param->model_send_comp.err_code);
      break;
    default:
      break;
  }
}

static void provisioning_callback(esp_ble_mesh_prov_cb_event_t event,
                                  esp_ble_mesh_prov_cb_param_t *param) {
  if (event != ESP_BLE_MESH_PROV_REGISTER_COMP_EVT)
    return;

  if (param->prov_register_comp.err_code != ESP_OK) {
    ESP_LOGE(TAG, "Mesh registration failed: %d", param->prov_register_comp.err_code);
    report_ready(false);
    return;
  }

  struct bt_mesh_device_network_info info = {0};
  memcpy(info.net_key, net_key, sizeof(info.net_key));
  info.net_idx = mesh_net_idx;
  info.flags = 0;
  info.iv_index = mesh_iv_index;
  info.unicast_addr = controller_address;
  memcpy(info.dev_key, local_device_key, sizeof(info.dev_key));
  memcpy(info.app_key, app_key, sizeof(info.app_key));
  info.app_idx = mesh_app_idx;
  info.group_addr = LOCAL_GROUP;

  int err = bt_mesh_device_auto_enter_network(&info);
  if (err != 0) {
    ESP_LOGE(TAG, "Unable to enter captured Mesh network: %d", err);
    report_ready(false);
    return;
  }

  err = initialize_sequence_range();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Unable to reserve persistent Mesh sequence range: %s",
             esp_err_to_name(err));
    report_ready(false);
    return;
  }

  ESP_LOGI(TAG, "Joined captured Mesh network as 0x%04X", controller_address);
  report_ready(true);
}

static esp_ble_mesh_client_common_param_t common_params(esp_ble_mesh_model_t *model,
                                                         uint32_t opcode) {
  esp_ble_mesh_client_common_param_t common = {0};
  common.opcode = opcode;
  common.model = model;
  common.ctx.net_idx = mesh_net_idx;
  common.ctx.app_idx = mesh_app_idx;
  common.ctx.addr = target_address;
  common.ctx.send_ttl = 3;
  common.msg_timeout = 0;
  return common;
}

int ble_mesh_bridge_init(const uint8_t supplied_net_key[16], const uint8_t supplied_app_key[16],
                         uint16_t supplied_target_address, uint16_t supplied_controller_address,
                         uint32_t supplied_iv_index, uint16_t supplied_net_idx,
                         uint16_t supplied_app_idx,
                         ble_mesh_bridge_ready_callback_t supplied_ready_callback,
                         ble_mesh_bridge_onoff_callback_t supplied_onoff_callback,
                         ble_mesh_bridge_lightness_callback_t supplied_lightness_callback,
                         ble_mesh_bridge_temperature_callback_t supplied_temperature_callback,
                         void *callback_context) {
  memcpy(net_key, supplied_net_key, sizeof(net_key));
  memcpy(app_key, supplied_app_key, sizeof(app_key));
  target_address = supplied_target_address;
  controller_address = supplied_controller_address;
  mesh_iv_index = supplied_iv_index;
  mesh_net_idx = supplied_net_idx;
  mesh_app_idx = supplied_app_idx;
  ready_callback = supplied_ready_callback;
  onoff_callback = supplied_onoff_callback;
  lightness_callback = supplied_lightness_callback;
  temperature_callback = supplied_temperature_callback;
  ready_context = callback_context;

  // This key protects only the ESP32's own Config Server. It is deliberately
  // generated at runtime and is never the lamp's DeviceKey.
  esp_fill_random(local_device_key, sizeof(local_device_key));

  if (sequence_mutex == NULL) {
    sequence_mutex = xSemaphoreCreateMutex();
    if (sequence_mutex == NULL)
      return ESP_ERR_NO_MEM;
  }

  if (onoff_drain_timer == NULL) {
    const esp_timer_create_args_t timer_args = {
        .callback = drain_onoff_queue,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_onoff_queue",
        .skip_unhandled_events = true,
    };
    const esp_err_t timer_err = esp_timer_create(&timer_args, &onoff_drain_timer);
    if (timer_err != ESP_OK)
      return timer_err;
  }

  if (lightness_drain_timer == NULL) {
    const esp_timer_create_args_t timer_args = {
        .callback = drain_lightness_queue,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_lightness_queue",
        .skip_unhandled_events = true,
    };
    const esp_err_t timer_err = esp_timer_create(&timer_args, &lightness_drain_timer);
    if (timer_err != ESP_OK)
      return timer_err;
  }

  if (ctl_drain_timer == NULL) {
    const esp_timer_create_args_t timer_args = {
        .callback = drain_ctl_queue,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_ctl_queue",
        .skip_unhandled_events = true,
    };
    const esp_err_t timer_err = esp_timer_create(&timer_args, &ctl_drain_timer);
    if (timer_err != ESP_OK)
      return timer_err;
  }

  esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  err = esp_bt_controller_init(&bt_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  err = esp_bluedroid_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  err = esp_bluedroid_enable();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;

  esp_ble_mesh_register_prov_callback(provisioning_callback);
  err = esp_ble_mesh_register_generic_client_callback(generic_client_callback);
  if (err != ESP_OK)
    return err;
  err = esp_ble_mesh_register_light_client_callback(light_client_callback);
  if (err != ESP_OK)
    return err;
  err = esp_ble_mesh_register_custom_model_callback(custom_model_callback);
  if (err != ESP_OK)
    return err;
  err = esp_ble_mesh_init(&provisioning, &composition);
  if (err != ESP_OK)
    return err;
  return esp_ble_mesh_client_model_init(&vendor_models[0]);
}

static int send_onoff_acknowledged(bool on) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[1], ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET);
  common.msg_timeout = 2500;
  esp_ble_mesh_generic_client_set_state_t set = {0};
  set.onoff_set.op_en = false;
  set.onoff_set.onoff = on;
  set.onoff_set.tid = transaction_id++;
  err = esp_ble_mesh_generic_client_set_state(&common, &set);
  if (err == ESP_OK) {
    active_onoff = on;
    onoff_request_in_flight = true;
  }
  return err;
}

int ble_mesh_bridge_send_onoff(bool on) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;

  if (onoff_request_in_flight || onoff_request_queued || onoff_get_in_flight) {
    onoff_request_queued = true;
    queued_onoff = on;
    onoff_retries_remaining = 2;
    ESP_LOGD(TAG, "Queued Generic OnOff %s while awaiting ACK",
             on ? "ON" : "OFF");
    return ESP_OK;
  }

  onoff_retries_remaining = 2;
  const int err = send_onoff_acknowledged(on);
  if (err != ESP_OK) {
    onoff_request_queued = true;
    queued_onoff = on;
    schedule_onoff_drain();
  }
  return err;
}

int ble_mesh_bridge_get_onoff(void) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  if (onoff_request_in_flight || onoff_request_queued || onoff_get_in_flight)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[1], ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
  common.msg_timeout = 2500;
  esp_ble_mesh_generic_client_get_state_t get = {0};
  err = esp_ble_mesh_generic_client_get_state(&common, &get);
  if (err == ESP_OK)
    onoff_get_in_flight = true;
  return err;
}

static int send_lightness_acknowledged(uint16_t lightness) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[2], ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET);
  common.msg_timeout = 2500;
  esp_ble_mesh_light_client_set_state_t set = {0};
  // This matches the official Tmall Genie app: acknowledged Lightness Actual
  // Set with a one-second transition (0x41) and no delay.
  set.lightness_set.op_en = true;
  set.lightness_set.lightness = lightness;
  set.lightness_set.tid = transaction_id++;
  set.lightness_set.trans_time = 0x41;
  set.lightness_set.delay = 0;
  err = esp_ble_mesh_light_client_set_state(&common, &set);
  if (err == ESP_OK) {
    active_lightness = lightness;
    lightness_request_in_flight = true;
  }
  return err;
}

int ble_mesh_bridge_send_lightness(uint16_t lightness) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;

  if (lightness_request_in_flight || lightness_request_queued || lightness_get_in_flight) {
    lightness_request_queued = true;
    queued_lightness = lightness;
    lightness_retries_remaining = 2;
    ESP_LOGD(TAG, "Queued Light Lightness %u while awaiting ACK", lightness);
    return ESP_OK;
  }

  lightness_retries_remaining = 2;
  const int err = send_lightness_acknowledged(lightness);
  if (err != ESP_OK) {
    lightness_request_queued = true;
    queued_lightness = lightness;
    schedule_lightness_drain();
  }
  return err;
}

int ble_mesh_bridge_get_lightness(void) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  if (lightness_request_in_flight || lightness_request_queued || lightness_get_in_flight)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[2], ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET);
  common.msg_timeout = 2500;
  esp_ble_mesh_light_client_get_state_t get = {0};
  err = esp_ble_mesh_light_client_get_state(&common, &get);
  if (err == ESP_OK)
    lightness_get_in_flight = true;
  return err;
}

int ble_mesh_bridge_send_vendor_main_light(bool enabled) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;

  esp_ble_mesh_msg_ctx_t context = {0};
  context.net_idx = mesh_net_idx;
  context.app_idx = mesh_app_idx;
  context.addr = target_address;
  context.send_ttl = 3;

  // Exact packet emitted by the official Tmall Genie app for its MainLight
  // property: TID, attribute 0x0534 little-endian, boolean value.
  uint8_t payload[4] = {
      vendor_transaction_id++,
      ALIGENIE_ATTR_MAIN_LIGHT & 0xFF,
      (ALIGENIE_ATTR_MAIN_LIGHT >> 8) & 0xFF,
      enabled ? 1 : 0,
  };
  return esp_ble_mesh_client_model_send_msg(
      &vendor_models[0], &context, ALIGENIE_OP_ATTR_SET_ACK, sizeof(payload), payload,
      3000, true, ROLE_NODE);
}

static int send_ctl_acknowledged(uint16_t lightness, uint16_t temperature) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[3], ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET);
  common.msg_timeout = 2500;
  esp_ble_mesh_light_client_set_state_t set = {0};
  set.ctl_set.op_en = true;
  set.ctl_set.ctl_lightness = lightness;
  set.ctl_set.ctl_temperature = temperature;
  set.ctl_set.ctl_delta_uv = 0;
  set.ctl_set.tid = transaction_id++;
  set.ctl_set.trans_time = 0x41;
  set.ctl_set.delay = 0;
  err = esp_ble_mesh_light_client_set_state(&common, &set);
  if (err == ESP_OK) {
    active_ctl_lightness = lightness;
    active_ctl_temperature = temperature;
    ctl_request_in_flight = true;
  }
  return err;
}

int ble_mesh_bridge_send_ctl(uint16_t lightness, uint16_t temperature) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;

  if (ctl_request_in_flight || ctl_request_queued || ctl_get_in_flight) {
    ctl_request_queued = true;
    queued_ctl_lightness = lightness;
    queued_ctl_temperature = temperature;
    ctl_retries_remaining = 2;
    ESP_LOGD(TAG, "Queued Light CTL %u/%u while awaiting ACK", lightness, temperature);
    return ESP_OK;
  }

  ctl_retries_remaining = 2;
  const int err = send_ctl_acknowledged(lightness, temperature);
  if (err != ESP_OK) {
    ctl_request_queued = true;
    queued_ctl_lightness = lightness;
    queued_ctl_temperature = temperature;
    schedule_ctl_drain();
  }
  return err;
}

int ble_mesh_bridge_get_ctl(void) {
  if (!ready)
    return ESP_ERR_INVALID_STATE;
  if (ctl_request_in_flight || ctl_request_queued || ctl_get_in_flight)
    return ESP_ERR_INVALID_STATE;
  int err = prepare_sequence_number();
  if (err != ESP_OK)
    return err;
  esp_ble_mesh_client_common_param_t common =
      common_params(&root_models[3], ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET);
  common.msg_timeout = 2500;
  esp_ble_mesh_light_client_get_state_t get = {0};
  err = esp_ble_mesh_light_client_get_state(&common, &get);
  if (err == ESP_OK)
    ctl_get_in_flight = true;
  return err;
}
