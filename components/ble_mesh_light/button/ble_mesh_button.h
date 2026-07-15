#pragma once

#include "esphome/components/button/button.h"
#include "../ble_mesh_light.h"

namespace esphome::ble_mesh_light {

class BleMeshButton : public button::Button {
 public:
  void set_parent(BleMeshLight *parent) { this->parent_ = parent; }
  void set_button_number(uint8_t button_number) { this->button_number_ = button_number; }
  void press_action() override;

 protected:
  BleMeshLight *parent_{nullptr};
  uint8_t button_number_{0};
};

}  // namespace esphome::ble_mesh_light
