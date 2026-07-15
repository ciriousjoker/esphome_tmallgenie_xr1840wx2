#include "ble_mesh_button.h"

#include "esphome/core/log.h"

namespace esphome::ble_mesh_light {

static const char *const TAG = "ble_mesh_light.button";

void BleMeshButton::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Button %u has no BLE Mesh parent", this->button_number_);
    return;
  }
  this->parent_->press_remote_button(this->button_number_);
}

}  // namespace esphome::ble_mesh_light
