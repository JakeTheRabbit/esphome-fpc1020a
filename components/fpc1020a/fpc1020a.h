#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace fpc1020a {

// Driver for the M5Stack Finger unit (FPC1020A + STM32 running the
// Waveshare-style 8-byte UART protocol, 19200 8N1).
//
// Frame: 0xF5 CMD P1 P2 P3 0x00 CHK 0xF5, CHK = XOR(CMD..0x00).
// The module only scans while a command window is open, so this component
// continuously issues 1:N match commands and handles enroll/delete requests
// between windows.
class FPC1020A : public Component, public uart::UARTDevice, public api::CustomAPIDevice {
 public:
  static const int MAX_SLOT = 10;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Entity pointers are injected from YAML (on_boot lambda).
  void set_match_text(text_sensor::TextSensor *t) { this->match_text_ = t; }
  void set_status_text(text_sensor::TextSensor *t) { this->status_text_ = t; }
  void set_count_sensor(sensor::Sensor *s) { this->count_sensor_ = s; }
  void set_matched_bin(binary_sensor::BinarySensor *b) { this->matched_bin_ = b; }

  void set_slot_name(int slot, const std::string &name);
  void start_enroll(int slot);
  void delete_slot(int slot);
  void delete_all();
  void query_count();

  // 0 = scanning/idle, 1 = enrolling (finger prompts), 2 = recent accept, 3 = recent reject/fail
  int ui_state();

 protected:
  // HA service args must be int32_t to match ESPHome's arg-type specializations
  void enroll_service_(int32_t slot) { this->start_enroll((int) slot); }
  void delete_slot_service_(int32_t slot) { this->delete_slot((int) slot); }

  static const uint8_t CMD_ADD1 = 0x01, CMD_ADD2 = 0x02, CMD_ADD3 = 0x03;
  static const uint8_t CMD_DELETE = 0x04, CMD_DELETE_ALL = 0x05, CMD_COUNT = 0x09;
  static const uint8_t CMD_MATCH = 0x0C, CMD_CAPTURE_TIMEOUT = 0x2E;
  static const uint8_t ACK_SUCCESS = 0x00, ACK_FAIL = 0x01, ACK_FULL = 0x04,
                       ACK_NOUSER = 0x05, ACK_USER_EXISTS = 0x06, ACK_FINGER_EXISTS = 0x07,
                       ACK_TIMEOUT = 0x08;

  enum State : uint8_t {
    STATE_BOOT,
    STATE_IDLE,
    STATE_WAIT_MATCH,
    STATE_WAIT_TO_ENROLL,  // capture-timeout write before enroll
    STATE_WAIT_ADD1,
    STATE_WAIT_ADD2,
    STATE_WAIT_ADD3,
    STATE_WAIT_TO_SCAN,  // capture-timeout write back to scan value
    STATE_WAIT_DELETE,
    STATE_WAIT_DELETE_ALL,
    STATE_WAIT_COUNT,
  };

  void send_cmd_(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint32_t timeout_ms);
  void handle_frame_(const uint8_t *f);
  void set_status_(const std::string &s);
  void enroll_fail_(uint8_t code);
  void finish_enroll_();
  std::string slot_name_(int slot);
  std::string ack_str_(uint8_t code);

  text_sensor::TextSensor *match_text_{nullptr};
  text_sensor::TextSensor *status_text_{nullptr};
  sensor::Sensor *count_sensor_{nullptr};
  binary_sensor::BinarySensor *matched_bin_{nullptr};

  std::string names_[MAX_SLOT + 1];
  State state_{STATE_BOOT};
  uint8_t rx_[8];
  uint8_t rx_len_{0};
  uint32_t deadline_{0};
  uint32_t next_poll_{0};
  int enroll_slot_{0};
  int pending_enroll_{0};
  int pending_delete_{0};
  bool pending_delete_all_{false};
  bool pending_count_{false};
  uint32_t last_ok_ms_{0};
  uint32_t last_bad_ms_{0};
};

}  // namespace fpc1020a
}  // namespace esphome
