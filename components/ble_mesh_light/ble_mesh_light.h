#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_output.h"
#include "esphome/core/component.h"

namespace esphome::ble_mesh_light {

class BleMeshStateBinarySensor : public binary_sensor::BinarySensor {};

class BleMeshLight : public light::LightOutput, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->light_state_ = state; }
  void write_state(light::LightState *state) override;

  void set_net_key(const std::string &value) { this->net_key_hex_ = value; }
  void set_app_key(const std::string &value) { this->app_key_hex_ = value; }
  void set_target_address(uint16_t value) { this->target_address_ = value; }
  void set_controller_address(uint16_t value) { this->controller_address_ = value; }
  void set_iv_index(uint32_t value) { this->iv_index_ = value; }
  void set_net_idx(uint16_t value) { this->net_idx_ = value; }
  void set_app_idx(uint16_t value) { this->app_idx_ = value; }
  void set_state_binary_sensor(BleMeshStateBinarySensor *sensor) {
    this->state_binary_sensor_ = sensor;
  }
  void press_remote_button(uint8_t button);

 protected:
  bool decode_key_(const std::string &hex, std::array<uint8_t, 16> &key);
  void send_pending_state_();
  static void mesh_ready_callback_(bool ready, void *context);
  static void onoff_state_callback_(bool on, void *context);
  static void lightness_state_callback_(uint16_t lightness, void *context);
  static void temperature_state_callback_(uint16_t temperature, void *context);

  std::string net_key_hex_;
  std::string app_key_hex_;
  std::array<uint8_t, 16> net_key_{};
  std::array<uint8_t, 16> app_key_{};
  uint16_t target_address_{0};
  uint16_t controller_address_{0};
  uint32_t iv_index_{0};
  uint16_t net_idx_{0};
  uint16_t app_idx_{0};
  bool mesh_ready_{false};
  bool pending_state_{false};
  bool pending_on_{false};
  float pending_brightness_{1.0f};
  float pending_temperature_percent_{0.5f};
  BleMeshStateBinarySensor *state_binary_sensor_{nullptr};
  light::LightState *light_state_{nullptr};
  bool frontend_initialized_{false};
};

}  // namespace esphome::ble_mesh_light
