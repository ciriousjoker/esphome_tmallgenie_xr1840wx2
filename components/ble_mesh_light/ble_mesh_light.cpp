#include "ble_mesh_light.h"

#include <algorithm>
#include <cmath>

#include "ble_mesh_bridge.h"
#include "esphome/core/log.h"
#include "esp_err.h"

namespace esphome::ble_mesh_light {

static const char *const TAG = "ble_mesh_light";

bool BleMeshLight::decode_key_(const std::string &hex, std::array<uint8_t, 16> &key) {
  if (hex.size() != 32)
    return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < key.size(); i++) {
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    key[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

void BleMeshLight::setup() {
  if (!this->decode_key_(this->net_key_hex_, this->net_key_) ||
      !this->decode_key_(this->app_key_hex_, this->app_key_)) {
    ESP_LOGE(TAG, "Invalid Bluetooth Mesh key");
    this->mark_failed();
    return;
  }

  const int err = ble_mesh_bridge_init(this->net_key_.data(), this->app_key_.data(),
                                       this->target_address_, this->controller_address_,
                                       this->iv_index_, this->net_idx_, this->app_idx_,
                                       &BleMeshLight::mesh_ready_callback_,
                                       &BleMeshLight::onoff_state_callback_,
                                       &BleMeshLight::lightness_state_callback_,
                                       &BleMeshLight::temperature_state_callback_, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BLE Mesh initialization failed: %s", esp_err_to_name(err));
    this->mark_failed();
  }
}

void BleMeshLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Bluetooth Mesh Light:");
  ESP_LOGCONFIG(TAG, "  Controller address: 0x%04X", this->controller_address_);
  ESP_LOGCONFIG(TAG, "  Target address: 0x%04X", this->target_address_);
  ESP_LOGCONFIG(TAG, "  IV Index: 0x%08" PRIX32, this->iv_index_);
  ESP_LOGCONFIG(TAG, "  NetKey index: 0x%03X", this->net_idx_);
  ESP_LOGCONFIG(TAG, "  AppKey index: 0x%03X", this->app_idx_);
  ESP_LOGCONFIG(TAG, "  Mesh ready: %s", YESNO(this->mesh_ready_));
}

light::LightTraits BleMeshLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  // The lamp and official app use the complete Bluetooth Mesh CTL range.
  traits.set_min_mireds(50.0f);    // 20,000 K
  traits.set_max_mireds(1250.0f);  // 800 K
  return traits;
}

void BleMeshLight::write_state(light::LightState *state) {
  // LightState emits one restore/default write during ESPHome startup. The
  // lamp is authoritative, so do not change a real ceiling light on reboot.
  if (!this->frontend_initialized_) {
    this->frontend_initialized_ = true;
    return;
  }
  this->pending_on_ = state->current_values.is_on();
  this->pending_brightness_ = state->current_values.get_brightness();
  const float temperature_kelvin = state->current_values.get_color_temperature_kelvin();
  if (std::isfinite(temperature_kelvin) && temperature_kelvin > 0.0f) {
    this->pending_temperature_percent_ =
        (std::clamp(temperature_kelvin, 800.0f, 20000.0f) - 800.0f) / 19200.0f;
  }
  this->pending_state_ = true;
  this->send_pending_state_();
}

void BleMeshLight::press_remote_button(uint8_t button) {
  if (!this->mesh_ready_) {
    ESP_LOGW(TAG, "Remote button %u ignored while Mesh is not ready", button);
    return;
  }

  const auto send_level_and_temperature = [this]() {
    const uint16_t lightness = static_cast<uint16_t>(std::lround(
        std::clamp(this->pending_brightness_, 0.0f, 1.0f) * 65535.0f));
    const uint16_t temperature = static_cast<uint16_t>(std::lround(
        800.0f + std::clamp(this->pending_temperature_percent_, 0.0f, 1.0f) * 19200.0f));
    // This lamp accepts CTL temperature but ignores the CTL packet's lightness
    // field. Send Light Lightness Actual separately, like the official app.
    ble_mesh_bridge_send_ctl(lightness, temperature);
    ble_mesh_bridge_send_lightness(lightness);
    this->pending_on_ = true;
  };

  const auto send_actual_lightness = [this]() {
    const uint16_t lightness = static_cast<uint16_t>(std::lround(
        std::clamp(this->pending_brightness_, 0.0f, 1.0f) * 65535.0f));
    return ble_mesh_bridge_send_lightness(lightness);
  };

  switch (button) {
    case 1:
      this->pending_on_ = true;
      ESP_LOGI(TAG, "Remote button 1 (ON): %s",
               esp_err_to_name(ble_mesh_bridge_send_onoff(true)));
      break;
    case 2:
      this->pending_on_ = false;
      ESP_LOGI(TAG, "Remote button 2 (OFF): %s",
               esp_err_to_name(ble_mesh_bridge_send_onoff(false)));
      break;
    case 3:
      this->pending_brightness_ = std::min(1.0f, this->pending_brightness_ + 0.2f);
      ESP_LOGI(TAG, "Remote button 3 (brightness +): %.0f%% -> %s",
               this->pending_brightness_ * 100.0f,
               esp_err_to_name(send_actual_lightness()));
      break;
    case 4:
      this->pending_temperature_percent_ =
          std::max(0.0f, this->pending_temperature_percent_ - 0.1f);
      send_level_and_temperature();
      ESP_LOGI(TAG, "Remote button 4 (K-): %.0f%%", this->pending_temperature_percent_ * 100.0f);
      break;
    case 5:
      ESP_LOGW(TAG, "Remote button 5 (SETUP) is intentionally disabled to protect Mesh provisioning");
      break;
    case 6:
      this->pending_temperature_percent_ =
          std::min(1.0f, this->pending_temperature_percent_ + 0.1f);
      send_level_and_temperature();
      ESP_LOGI(TAG, "Remote button 6 (K+): %.0f%%", this->pending_temperature_percent_ * 100.0f);
      break;
    case 7:
      this->pending_brightness_ = std::max(0.05f, this->pending_brightness_ - 0.2f);
      ESP_LOGI(TAG, "Remote button 7 (brightness -): %.0f%% -> %s",
               this->pending_brightness_ * 100.0f,
               esp_err_to_name(send_actual_lightness()));
      break;
    case 8:
      this->pending_brightness_ = 1.0f;
      this->pending_temperature_percent_ = 1.0f;
      send_level_and_temperature();
      ble_mesh_bridge_send_onoff(true);
      ESP_LOGI(TAG, "Remote button 8 (day mode)");
      break;
    case 9:
      this->pending_brightness_ = 0.05f;
      this->pending_temperature_percent_ = 0.0f;
      send_level_and_temperature();
      ble_mesh_bridge_send_onoff(true);
      ESP_LOGI(TAG, "Remote button 9 (night mode)");
      break;
    case 10:
      ESP_LOGI(TAG, "Remote button 10 (MainLight ON): %s",
               esp_err_to_name(ble_mesh_bridge_send_vendor_main_light(true)));
      break;
    case 11:
      ESP_LOGI(TAG, "Remote button 11 (MainLight OFF): %s",
               esp_err_to_name(ble_mesh_bridge_send_vendor_main_light(false)));
      break;
    case 12:
      this->set_timeout("remote-off-timer", 60000, [this]() {
        this->pending_on_ = false;
        ESP_LOGI(TAG, "Remote button 12 timer elapsed: %s",
                 esp_err_to_name(ble_mesh_bridge_send_onoff(false)));
      });
      ESP_LOGI(TAG, "Remote button 12: OFF scheduled in 60 seconds");
      break;
    default:
      ESP_LOGE(TAG, "Invalid remote button number: %u", button);
      break;
  }
}

void BleMeshLight::mesh_ready_callback_(bool ready, void *context) {
  auto *self = static_cast<BleMeshLight *>(context);
  // ESP-IDF invokes this callback on its Bluetooth task. Move all ESPHome
  // scheduler and entity work back onto the main loop.
  self->defer("mesh-ready-state", [self, ready]() {
    self->mesh_ready_ = ready;
    if (!ready) {
      ESP_LOGE(TAG, "Bluetooth Mesh controller setup failed");
      self->mark_failed();
      return;
    }

    ESP_LOGI(TAG, "Bluetooth Mesh controller is ready");
    self->send_pending_state_();

    const auto log_query_error = [](const char *name, int err) {
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "%s query failed: %s", name, esp_err_to_name(err));
    };
    self->set_timeout("mesh-initial-onoff", 1000, [log_query_error]() {
      log_query_error("Initial OnOff", ble_mesh_bridge_get_onoff());
    });
    self->set_timeout("mesh-initial-lightness", 4000, [log_query_error]() {
      log_query_error("Initial lightness", ble_mesh_bridge_get_lightness());
    });
    self->set_timeout("mesh-initial-ctl", 8000, [log_query_error]() {
      log_query_error("Initial CTL", ble_mesh_bridge_get_ctl());
    });

    // Poll all state models, staggered to coexist reliably with Wi-Fi. This
    // also reflects changes made with the original physical remote in HA.
    self->set_interval("mesh-state-poll", 30000, [self, log_query_error]() {
      log_query_error("Periodic OnOff", ble_mesh_bridge_get_onoff());
      self->set_timeout("mesh-poll-lightness", 3000, [log_query_error]() {
        log_query_error("Periodic lightness", ble_mesh_bridge_get_lightness());
      });
      self->set_timeout("mesh-poll-ctl", 6000, [log_query_error]() {
        log_query_error("Periodic CTL", ble_mesh_bridge_get_ctl());
      });
    });
  });
}

void BleMeshLight::onoff_state_callback_(bool on, void *context) {
  auto *self = static_cast<BleMeshLight *>(context);
  self->defer("mesh-onoff-state", [self, on]() {
    self->pending_on_ = on;
    if (self->state_binary_sensor_ != nullptr)
      self->state_binary_sensor_->publish_state(on);
    if (self->light_state_ != nullptr) {
      self->light_state_->current_values.set_state(on);
      self->light_state_->remote_values.set_state(on);
      self->light_state_->publish_state();
    }
  });
}

void BleMeshLight::lightness_state_callback_(uint16_t lightness, void *context) {
  auto *self = static_cast<BleMeshLight *>(context);
  self->defer("mesh-lightness-state", [self, lightness]() {
    const float brightness = static_cast<float>(lightness) / 65535.0f;
    self->pending_brightness_ = brightness;
    if (self->light_state_ != nullptr) {
      self->light_state_->current_values.set_brightness(brightness);
      self->light_state_->remote_values.set_brightness(brightness);
      self->light_state_->publish_state();
    }
  });
}

void BleMeshLight::temperature_state_callback_(uint16_t temperature, void *context) {
  auto *self = static_cast<BleMeshLight *>(context);
  self->defer("mesh-temperature-state", [self, temperature]() {
    if (temperature < 800 || temperature > 20000)
      return;
    self->pending_temperature_percent_ =
        (static_cast<float>(temperature) - 800.0f) / 19200.0f;
    if (self->light_state_ != nullptr) {
      const float mireds = 1000000.0f / static_cast<float>(temperature);
      self->light_state_->current_values.set_color_mode(light::ColorMode::COLOR_TEMPERATURE);
      self->light_state_->remote_values.set_color_mode(light::ColorMode::COLOR_TEMPERATURE);
      self->light_state_->current_values.set_color_temperature(mireds);
      self->light_state_->remote_values.set_color_temperature(mireds);
      self->light_state_->publish_state();
    }
  });
}

void BleMeshLight::send_pending_state_() {
  if (!this->mesh_ready_ || !this->pending_state_)
    return;
  this->pending_state_ = false;

  if (!this->pending_on_) {
    const int err = ble_mesh_bridge_send_onoff(false);
    ESP_LOGI(TAG, "ON/OFF=OFF -> 0x%04X: %s", this->target_address_, esp_err_to_name(err));
    return;
  }

  const uint16_t lightness = static_cast<uint16_t>(std::lround(
      std::clamp(this->pending_brightness_, 0.0f, 1.0f) * 65535.0f));
  const uint16_t temperature = static_cast<uint16_t>(std::lround(
      800.0f + std::clamp(this->pending_temperature_percent_, 0.0f, 1.0f) * 19200.0f));
  const int lightness_err = ble_mesh_bridge_send_lightness(lightness);
  const int ctl_err = ble_mesh_bridge_send_ctl(lightness, temperature);
  const int onoff_err = ble_mesh_bridge_send_onoff(true);
  ESP_LOGI(TAG, "ON, lightness=%u, temperature=%uK: %s / %s / %s", lightness,
           temperature, esp_err_to_name(onoff_err), esp_err_to_name(lightness_err),
           esp_err_to_name(ctl_err));
}

}  // namespace esphome::ble_mesh_light
