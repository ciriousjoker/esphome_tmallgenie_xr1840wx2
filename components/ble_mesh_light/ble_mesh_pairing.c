#include "ble_mesh_pairing.h"
#include "ble_mesh_bridge.h"
#include "mesh_pairing_crypto.h"
#include <string.h>
#include "esp_ble_mesh_ble_api.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"

#define SESSION_MS 60000
#define FRAME_MS 240
#define MAX_FRAMES 16

static const char *TAG = "mesh_pairing";
typedef enum { IDLE, RANDOM, NETWORK, APP_KEY, COMPLETE, FAILED } pairing_state_t;
typedef struct { uint8_t length; uint8_t data[24]; int error; bool scan_result; bool confirmed_existing; } radio_event_t;
typedef struct {
  uint8_t tid, total, mask, lengths[2], data[2][18];
  uint64_t updated;
} fragments_t;
static QueueHandle_t rx_queue;
static pairing_state_t state;
static bool configured, radio_ready, have_peer_sequence;
static uint8_t target_mac[6], saved_net_key[16], saved_app_key[16], device_key[16], random_packet[20], network_packet[24];
static mesh_pairing_network_t network;
static uint16_t target_address, controller_address, pairing_address, app_index;
static uint32_t peer_sequence;
static uint64_t deadline, next_frame;
static uint8_t frames[MAX_FRAMES][24], frame_sizes[MAX_FRAMES], frame_count, frame_position, transaction;
static fragments_t fragments[4];
static ble_mesh_pairing_status_callback_t status_callback;
static void *status_context;

static uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }
bool ble_mesh_pairing_busy(void) { return state == RANDOM || state == NETWORK || state == APP_KEY; }
static void status(const char *message) {
  ESP_LOGI(TAG, "%s", message);
  if (status_callback) status_callback(message, status_context);
}
static void finish(bool success, const char *message) {
  state = success ? COMPLETE : FAILED;
  frame_count = 0;
  memset(device_key, 0, sizeof(device_key));
  memset(network_packet, 0, sizeof(network_packet));
  memset(random_packet, 0, sizeof(random_packet));
  status(message);
}

void ble_mesh_pairing_confirm_existing(void) {
  if (!rx_queue) return;
  radio_event_t event = {.confirmed_existing = true};
  xQueueSend(rx_queue, &event, 0);
}

static bool mac_matches(const uint8_t mac[6]) {
  bool direct = true, reverse = true;
  for (size_t i = 0; i < 6; i++) { direct &= mac[i] == target_mac[i]; reverse &= mac[i] == target_mac[5 - i]; }
  return direct || reverse;
}

static void radio_callback(esp_ble_mesh_ble_cb_event_t event, esp_ble_mesh_ble_cb_param_t *param) {
  if (!rx_queue || !param) return;
  radio_event_t item = {0};
  if (event == ESP_BLE_MESH_START_BLE_ADVERTISING_COMP_EVT && param->start_ble_advertising_comp.err_code) {
    item.error = param->start_ble_advertising_comp.err_code;
    xQueueSend(rx_queue, &item, 0);
  } else if (event == ESP_BLE_MESH_START_BLE_SCANNING_COMP_EVT) {
    item.error = param->start_ble_scan_comp.err_code;
    item.scan_result = true;
    xQueueSend(rx_queue, &item, 0);
  } else if (event == ESP_BLE_MESH_SCAN_BLE_ADVERTISING_PKT_EVT) {
    const esp_ble_mesh_ble_adv_rpt_t *r = &param->scan_ble_adv_pkt;
    if (!mac_matches(r->addr) || !r->data) return;
    for (size_t pos = 0; pos + 1 < r->length;) {
      size_t length = r->data[pos];
      if (!length || pos + length >= r->length) break;
      const uint8_t *p = r->data + pos;
      if (length >= 5 && length <= 27 && p[1] == 0xff && p[2] == 0x01 && p[3] == 0xa8 && p[4] == 0x0e) {
        item.length = length - 3;
        memcpy(item.data, p + 4, item.length);
        xQueueSend(rx_queue, &item, 0);
      }
      pos += length + 1;
    }
  }
}

int ble_mesh_pairing_configure(const uint8_t mac[6], uint16_t supplied_pairing_address,
                               const uint8_t net_key[16], const uint8_t app_key[16],
                               uint16_t target, uint16_t controller, uint32_t iv,
                               uint16_t net_index, uint16_t supplied_app_index,
                               ble_mesh_pairing_status_callback_t callback, void *context) {
  if (!mac || !supplied_pairing_address || supplied_pairing_address > 0x7fff || net_index || iv > 255 ||
      supplied_app_index > 0xfff || supplied_pairing_address == target || supplied_pairing_address == target + 1 ||
      supplied_pairing_address == controller) return ESP_ERR_INVALID_ARG;
  if (!mesh_pairing_network_init(&network, net_key, iv)) return ESP_FAIL;
  rx_queue = xQueueCreate(16, sizeof(radio_event_t));
  if (!rx_queue) return ESP_ERR_NO_MEM;
  memcpy(target_mac, mac, 6); memcpy(saved_net_key, net_key, 16); memcpy(saved_app_key, app_key, 16);
  target_address = target; controller_address = controller; pairing_address = supplied_pairing_address;
  app_index = supplied_app_index; status_callback = callback; status_context = context;
  configured = true;
  status("Idle");
  return ESP_OK;
}

int ble_mesh_pairing_radio_start(void) {
  if (!configured) return ESP_OK;
  int err = esp_ble_mesh_register_ble_callback(radio_callback);
  if (err) return err;
  esp_ble_mesh_ble_scan_param_t params = {0};
  err = esp_ble_mesh_start_ble_scanning(&params);
  return err;
}

int ble_mesh_pairing_start(void) {
  if (!configured || !radio_ready || ble_mesh_pairing_busy()) return ESP_ERR_INVALID_STATE;
  uint8_t random[8];
  esp_fill_random(random, sizeof(random));
  random_packet[0] = 0x0e; random_packet[1] = 0;
  memcpy(random_packet + 2, target_mac + 4, 2);
  memcpy(random_packet + 4, random, 8); memcpy(random_packet + 12, random, 8);
  memset(random, 0, sizeof(random));
  memset(fragments, 0, sizeof(fragments));
  xQueueReset(rx_queue);
  // Leave the normal ON check and its retries a clear receive window first.
  state = RANDOM; deadline = now_ms() + SESSION_MS; next_frame = now_ms() + 12000;
  frame_count = 0;
  status("Waiting for lamp");
  return ESP_OK;
}

static bool reserve_pairing_sequence(uint32_t *sequence) {
  nvs_handle_t handle;
  if (nvs_open("lamp_mesh", NVS_READWRITE, &handle) != ESP_OK) return false;
  uint32_t previous = 0x1ff000;
  int err = nvs_get_u32(handle, "pair_seq", &previous);
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK && previous > 0xffffff - 0x1000) err = ESP_ERR_INVALID_SIZE;
  if (err == ESP_OK) { *sequence = previous + 0x1000; err = nvs_set_u32(handle, "pair_seq", *sequence); }
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

static bool prepare_app_key(void) {
  uint8_t access[20] = {0}, packets[3][29], sizes[3];
  uint32_t sequence = 0;
  if (!reserve_pairing_sequence(&sequence)) return false;
  // Packed 12-bit NetKey index (zero) followed by the configured AppKey index.
  access[2] = (app_index & 15) << 4; access[3] = app_index >> 4;
  memcpy(access + 4, saved_app_key, 16);
  size_t count = mesh_pairing_access_encode(&network, device_key, pairing_address, target_address, sequence, 5,
                                           access, sizeof(access), packets, sizes);
  memset(access, 0, sizeof(access));
  if (!count) return false;
  frame_count = frame_position = 0;
  for (size_t i = 0; i < count; i++) {
    transaction++;
    size_t parts = (sizes[i] + 17) / 18;
    for (size_t repeat = 0; repeat < 4; repeat++) {
      for (size_t part = 0; part < parts; part++) {
        if (frame_count >= MAX_FRAMES) return false;
        uint8_t *f = frames[frame_count];
        size_t length = sizes[i] - part * 18; if (length > 18) length = 18;
        f[0] = 0x0e; f[1] = 6; f[2] = network.nid; f[3] = transaction;
        f[4] = (parts << 4) | (part + 1); f[5] = length;
        memcpy(f + 6, packets[i] + part * 18, length);
        frame_sizes[frame_count++] = length + 6;
      }
    }
  }
  return true;
}

static void receive_mesh(const uint8_t *data, size_t length) {
  mesh_pairing_packet_t packet;
  if (!mesh_pairing_network_decode(&network, data, length, &packet) || packet.source != target_address ||
      packet.control || !packet.lower_length) return;
  bool application = (packet.lower[0] & 0x40) != 0;
  if ((!application && (state != APP_KEY || packet.destination != pairing_address)) ||
      (application && packet.destination != controller_address && packet.destination != pairing_address &&
       packet.destination != 0xf000 && packet.destination != 0xc000)) return;
  uint8_t access[16]; size_t access_length = sizeof(access);
  if (!mesh_pairing_access_decode(&network, &packet, application ? saved_app_key : device_key,
                                  access, &access_length)) return;
  if (application && have_peer_sequence && packet.sequence <= peer_sequence) return;
  if (!have_peer_sequence || packet.sequence > peer_sequence) peer_sequence = packet.sequence;
  have_peer_sequence = true;
  if (application) ble_mesh_pairing_confirm_existing();
  if (!application && access_length == 6 && access[0] == 0x80 && access[1] == 0x03) {
    uint16_t net_idx = access[3] | ((access[4] & 15) << 8);
    uint16_t app_idx = (access[4] >> 4) | ((uint16_t) access[5] << 4);
    if (net_idx != 0 || app_idx != app_index) return;
    if (access[2] == 0) finish(true, "Paired");
    else finish(false, "Failed: lamp rejected application key");
  } else if (application && access_length >= 3 && access[0] == 0x82 && access[1] == 0x04) {
    ble_mesh_bridge_receive_authenticated_onoff(access[2] != 0);
  } else if (application && access_length >= 4 && (access[0] == 0xd3 || access[0] == 0xd4) &&
             access[1] == 0xa8 && access[2] == 0x01) {
    ble_mesh_bridge_receive_authenticated_vendor(access + 3, access_length - 3);
  }
}

static void receive_fragment(const uint8_t *p, size_t length) {
  if (length < 7 || p[2] != network.nid) return;
  uint8_t total = p[4] >> 4, index = p[4] & 15, size = p[5];
  if (!total || total > 2 || !index || index > total || !size || size > 18 || length != (size_t) size + 6) return;
  fragments_t *g = NULL;
  uint64_t now = now_ms();
  for (size_t i = 0; i < 4; i++) {
    if (fragments[i].total && fragments[i].tid == p[3] && fragments[i].updated + 2000 > now) { g = &fragments[i]; break; }
  }
  if (!g) {
    g = &fragments[0];
    for (size_t i = 1; i < 4; i++) if (fragments[i].updated < g->updated) g = &fragments[i];
    memset(g, 0, sizeof(*g)); g->tid = p[3]; g->total = total;
  }
  if (g->total != total) return;
  g->updated = now; g->lengths[index - 1] = size;
  memcpy(g->data[index - 1], p + 6, size); g->mask |= 1 << (index - 1);
  if (g->mask != (1 << total) - 1) return;
  uint8_t packet[36]; size_t packet_length = 0;
  for (size_t i = 0; i < total; i++) { memcpy(packet + packet_length, g->data[i], g->lengths[i]); packet_length += g->lengths[i]; }
  receive_mesh(packet, packet_length);
}

static void receive(const radio_event_t *event) {
  const uint8_t *p = event->data;
  if (event->confirmed_existing) {
    if (state == RANDOM) {
      ESP_LOGI(TAG, "Existing pairing confirmed by lamp response");
      finish(true, "Paired");
    }
    return;
  }
  if (event->scan_result) radio_ready = event->error == 0;
  if (event->error) {
    if (ble_mesh_pairing_busy()) finish(false, "Failed: Bluetooth radio error");
    else if (event->scan_result) status("Failed: Bluetooth radio error");
    return;
  }
  if (event->length < 2 || p[0] != 0x0e) return;
  if (p[1] == 7) { receive_fragment(p, event->length); return; }
  if (state == RANDOM && event->length == 20 && p[1] == 1 && memcmp(p + 2, target_mac + 4, 2) == 0) {
    uint8_t mask[22], plain[22] = {0};
    if (!mesh_pairing_session(p + 4, device_key, mask)) { finish(false, "Failed: session derivation"); return; }
    memcpy(plain, target_mac + 4, 2); memcpy(plain + 3, saved_net_key, 16);
    plain[19] = network.iv_index; plain[20] = target_address >> 8; plain[21] = target_address;
    network_packet[0] = 0x0e; network_packet[1] = 2;
    for (size_t i = 0; i < 22; i++) network_packet[i + 2] = plain[i] ^ mask[i];
    memset(mask, 0, sizeof(mask)); memset(plain, 0, sizeof(plain));
    state = NETWORK; next_frame = 0; status("Restoring network");
  } else if (state == NETWORK && event->length == 8 && p[1] == 3 && memcmp(p + 2, target_mac, 6) == 0) {
    if (!prepare_app_key()) { finish(false, "Failed: preparing application key"); return; }
    have_peer_sequence = false;
    state = APP_KEY; next_frame = 0; status("Installing application key");
  }
}

static int advertise(const uint8_t *payload, size_t length) {
  // FIFO priority preserves random -> network data -> AppKey ordering even
  // when normal Mesh packets temporarily delay the advertising queue.
  esp_ble_mesh_ble_adv_param_t params = {
      .interval = 0x20, .adv_type = 0, .own_addr_type = 0,
      .duration = 200, .period = 0, .count = 0, .priority = ESP_BLE_MESH_BLE_ADV_PRIO_LOW};
  esp_ble_mesh_ble_adv_data_t data = {0};
  // Flags and the same connectable manufacturer advertisement used by the SDK.
  const uint8_t prefix[] = {2, 1, 6, length + 3, 0xff, 0x01, 0xa8};
  memcpy(data.adv_data, prefix, sizeof(prefix)); memcpy(data.adv_data + sizeof(prefix), payload, length);
  data.adv_data_len = sizeof(prefix) + length;
  return esp_ble_mesh_start_ble_advertising(&params, &data);
}

void ble_mesh_pairing_poll(void) {
  if (!configured) return;
  radio_event_t event;
  for (size_t i = 0; i < 16 && xQueueReceive(rx_queue, &event, 0) == pdTRUE; i++) receive(&event);
  if (!ble_mesh_pairing_busy()) return;
  uint64_t now = now_ms();
  if (now >= deadline) { finish(false, "Failed: pairing timed out"); return; }
  if (now < next_frame) return;
  int err;
  if (state == RANDOM) err = advertise(random_packet, sizeof(random_packet));
  else if (state == NETWORK) err = advertise(network_packet, sizeof(network_packet));
  else {
    if (!frame_count) { finish(false, "Failed: no application key packets"); return; }
    err = advertise(frames[frame_position], frame_sizes[frame_position]);
    if (!err) frame_position = (frame_position + 1) % frame_count;
  }
  next_frame = now + (err ? 500 : FRAME_MS);
  if (err && err != ESP_ERR_NO_MEM) finish(false, "Failed: unable to advertise");
}
