#include "fpc1020a.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fpc1020a {

static const char *const TAG = "fpc1020a";

// Module capture-timeout register units are roughly 0.2-0.3 s each.
static const uint8_t SCAN_TIMEOUT_UNITS = 5;     // ~1-1.5 s identify window while polling
static const uint8_t ENROLL_TIMEOUT_UNITS = 30;  // ~6-9 s per finger placement while enrolling
static const uint32_t SCAN_DEADLINE_MS = 5000;
static const uint32_t ENROLL_DEADLINE_MS = 15000;

void FPC1020A::setup() {
  for (int i = 1; i <= MAX_SLOT; i++) {
    if (this->names_[i].empty())
      this->names_[i] = "Slot " + to_string(i);
  }
  this->register_service(&FPC1020A::enroll_service_, "enroll", {"slot"});
  this->register_service(&FPC1020A::delete_slot_service_, "delete_slot", {"slot"});
  this->register_service(&FPC1020A::delete_all, "delete_all");
  this->state_ = STATE_BOOT;
  this->next_poll_ = millis() + 2000;  // let the module finish booting
  this->pending_count_ = true;
}

void FPC1020A::dump_config() {
  ESP_LOGCONFIG(TAG, "FPC1020A fingerprint reader (M5Stack Finger unit)");
  ESP_LOGCONFIG(TAG, "  Named slots: %d", MAX_SLOT);
}

void FPC1020A::set_slot_name(int slot, const std::string &name) {
  if (slot < 1 || slot > MAX_SLOT)
    return;
  this->names_[slot] = name.empty() ? ("Slot " + to_string(slot)) : name;
}

std::string FPC1020A::slot_name_(int slot) {
  if (slot >= 1 && slot <= MAX_SLOT)
    return this->names_[slot];
  return "User " + to_string(slot);
}

void FPC1020A::start_enroll(int slot) {
  if (slot < 1 || slot > MAX_SLOT) {
    this->set_status_("Slot must be 1-" + to_string(MAX_SLOT));
    return;
  }
  if (this->pending_enroll_ != 0 ||
      (this->state_ >= STATE_WAIT_TO_ENROLL && this->state_ <= STATE_WAIT_ADD3)) {
    this->set_status_("Already enrolling - finish that first");
    return;
  }
  this->pending_enroll_ = slot;
  this->set_status_("Starting enroll for slot " + to_string(slot) + " (" + this->slot_name_(slot) + ")...");
}

void FPC1020A::delete_slot(int slot) {
  if (slot < 1 || slot > MAX_SLOT) {
    this->set_status_("Slot must be 1-" + to_string(MAX_SLOT));
    return;
  }
  this->pending_delete_ = slot;
}

void FPC1020A::delete_all() { this->pending_delete_all_ = true; }

void FPC1020A::query_count() { this->pending_count_ = true; }

int FPC1020A::ui_state() {
  const uint32_t now = millis();
  if (this->pending_enroll_ != 0 ||
      (this->state_ >= STATE_WAIT_TO_ENROLL && this->state_ <= STATE_WAIT_ADD3))
    return 1;
  if (this->last_ok_ms_ != 0 && now - this->last_ok_ms_ < 2500)
    return 2;
  if (this->last_bad_ms_ != 0 && now - this->last_bad_ms_ < 2500)
    return 3;
  return 0;
}

void FPC1020A::send_cmd_(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint32_t timeout_ms) {
  while (this->available()) {
    uint8_t d;
    this->read_byte(&d);
  }
  this->rx_len_ = 0;
  const uint8_t chk = cmd ^ p1 ^ p2 ^ p3;
  const uint8_t frame[8] = {0xF5, cmd, p1, p2, p3, 0x00, chk, 0xF5};
  this->write_array(frame, 8);
  this->deadline_ = millis() + timeout_ms;
}

void FPC1020A::set_status_(const std::string &s) {
  ESP_LOGI(TAG, "%s", s.c_str());
  if (this->status_text_ != nullptr)
    this->status_text_->publish_state(s);
}

std::string FPC1020A::ack_str_(uint8_t code) {
  switch (code) {
    case ACK_FAIL:
      return "sensor error";
    case ACK_FULL:
      return "storage full";
    case ACK_NOUSER:
      return "no such user";
    case ACK_USER_EXISTS:
      return "slot in use - delete it first";
    case ACK_FINGER_EXISTS:
      return "finger already enrolled";
    case ACK_TIMEOUT:
      return "timed out waiting for finger";
    default:
      return "error code " + to_string((int) code);
  }
}

void FPC1020A::enroll_fail_(uint8_t code) {
  this->set_status_("Enroll failed: " + this->ack_str_(code));
  this->last_bad_ms_ = millis();
  this->finish_enroll_();
}

void FPC1020A::finish_enroll_() {
  this->enroll_slot_ = 0;
  // restore the short identify window for scanning
  this->send_cmd_(CMD_CAPTURE_TIMEOUT, 0, SCAN_TIMEOUT_UNITS, 0, 2000);
  this->state_ = STATE_WAIT_TO_SCAN;
}

void FPC1020A::loop() {
  const uint32_t now = millis();

  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b))
      break;
    if (this->rx_len_ == 0 && b != 0xF5)
      continue;  // resync to frame start
    this->rx_[this->rx_len_++] = b;
    if (this->rx_len_ == 8) {
      this->rx_len_ = 0;
      const uint8_t chk = this->rx_[1] ^ this->rx_[2] ^ this->rx_[3] ^ this->rx_[4] ^ this->rx_[5];
      if (this->rx_[7] == 0xF5 && chk == this->rx_[6]) {
        this->handle_frame_(this->rx_);
      } else {
        ESP_LOGW(TAG, "Bad frame (checksum %02X, expected %02X)", this->rx_[6], chk);
      }
    }
  }

  // no-response watchdog
  if (this->state_ != STATE_IDLE && this->state_ != STATE_BOOT && now > this->deadline_) {
    if (this->state_ == STATE_WAIT_TO_ENROLL) {
      // module ignored the capture-timeout write; enroll with its default anyway
      ESP_LOGW(TAG, "No ack for capture timeout - continuing enroll");
      this->set_status_("Place finger on the sensor (1/3)");
      this->send_cmd_(CMD_ADD1, (uint8_t)(this->enroll_slot_ >> 8), (uint8_t)(this->enroll_slot_ & 0xFF), 1,
                      ENROLL_DEADLINE_MS);
      this->state_ = STATE_WAIT_ADD1;
      return;
    }
    if (this->state_ >= STATE_WAIT_ADD1 && this->state_ <= STATE_WAIT_ADD3) {
      this->set_status_("Enroll timed out - no response from reader");
      this->last_bad_ms_ = now;
      this->finish_enroll_();
      return;
    }
    if (this->state_ == STATE_WAIT_MATCH) {
      ESP_LOGW(TAG, "No response to identify command");
    } else {
      ESP_LOGW(TAG, "Response timeout in state %d", (int) this->state_);
    }
    this->state_ = STATE_IDLE;
    this->next_poll_ = now + 1000;
  }

  if (this->state_ == STATE_BOOT && now >= this->next_poll_) {
    this->send_cmd_(CMD_CAPTURE_TIMEOUT, 0, SCAN_TIMEOUT_UNITS, 0, 2000);
    this->state_ = STATE_WAIT_TO_SCAN;
    return;
  }

  if (this->state_ != STATE_IDLE || now < this->next_poll_)
    return;

  if (this->pending_enroll_ != 0) {
    this->enroll_slot_ = this->pending_enroll_;
    this->pending_enroll_ = 0;
    this->send_cmd_(CMD_CAPTURE_TIMEOUT, 0, ENROLL_TIMEOUT_UNITS, 0, 2000);
    this->state_ = STATE_WAIT_TO_ENROLL;
    return;
  }
  if (this->pending_delete_ != 0) {
    const int s = this->pending_delete_;
    this->pending_delete_ = 0;
    this->enroll_slot_ = s;  // remembered for the status message
    this->send_cmd_(CMD_DELETE, (uint8_t)(s >> 8), (uint8_t)(s & 0xFF), 0, 3000);
    this->state_ = STATE_WAIT_DELETE;
    return;
  }
  if (this->pending_delete_all_) {
    this->pending_delete_all_ = false;
    this->send_cmd_(CMD_DELETE_ALL, 0, 0, 0, 5000);
    this->state_ = STATE_WAIT_DELETE_ALL;
    return;
  }
  if (this->pending_count_) {
    this->pending_count_ = false;
    this->send_cmd_(CMD_COUNT, 0, 0, 0, 2000);
    this->state_ = STATE_WAIT_COUNT;
    return;
  }

  // keep an identify window open so a finger press is picked up
  this->send_cmd_(CMD_MATCH, 0, 0, 0, SCAN_DEADLINE_MS);
  this->state_ = STATE_WAIT_MATCH;
}

void FPC1020A::handle_frame_(const uint8_t *f) {
  const uint8_t cmd = f[1], q1 = f[2], q2 = f[3], q3 = f[4];
  const uint32_t now = millis();

  switch (cmd) {
    case CMD_MATCH: {
      this->state_ = STATE_IDLE;
      if (q3 >= 1 && q3 <= 3) {  // matched; q3 = stored privilege level
        const int uid = (q1 << 8) | q2;
        const std::string name = this->slot_name_(uid);
        ESP_LOGI(TAG, "Fingerprint match: slot %d (%s)", uid, name.c_str());
        if (this->match_text_ != nullptr)
          this->match_text_->publish_state(name);
        if (this->matched_bin_ != nullptr) {
          this->matched_bin_->publish_state(true);
          this->set_timeout("match_off", 1500, [this]() { this->matched_bin_->publish_state(false); });
        }
        this->fire_homeassistant_event(
            "esphome.fingerprint_matched",
            {{"node", App.get_name()}, {"user_id", to_string(uid)}, {"user_name", name}});
        this->last_ok_ms_ = now;
        this->next_poll_ = now + 3000;  // debounce while the finger is still resting
      } else if (q3 == ACK_NOUSER) {
        ESP_LOGW(TAG, "Unknown fingerprint rejected");
        if (this->match_text_ != nullptr)
          this->match_text_->publish_state("Unknown finger");
        this->fire_homeassistant_event("esphome.fingerprint_unmatched", {{"node", App.get_name()}});
        this->last_bad_ms_ = now;
        this->next_poll_ = now + 2000;
      } else {  // ACK_TIMEOUT: nobody touched it; anything else: shrug and rescan
        this->next_poll_ = now;
      }
      break;
    }

    case CMD_ADD1:
      if (q3 == ACK_SUCCESS) {
        this->set_status_("Good. Lift off, then place the same finger again (2/3)");
        this->send_cmd_(CMD_ADD2, (uint8_t)(this->enroll_slot_ >> 8), (uint8_t)(this->enroll_slot_ & 0xFF), 1,
                        ENROLL_DEADLINE_MS);
        this->state_ = STATE_WAIT_ADD2;
      } else {
        this->enroll_fail_(q3);
      }
      break;

    case CMD_ADD2:
      if (q3 == ACK_SUCCESS) {
        this->set_status_("Once more - place the same finger (3/3)");
        this->send_cmd_(CMD_ADD3, (uint8_t)(this->enroll_slot_ >> 8), (uint8_t)(this->enroll_slot_ & 0xFF), 1,
                        ENROLL_DEADLINE_MS);
        this->state_ = STATE_WAIT_ADD3;
      } else {
        this->enroll_fail_(q3);
      }
      break;

    case CMD_ADD3:
      if (q3 == ACK_SUCCESS) {
        const std::string name = this->slot_name_(this->enroll_slot_);
        this->set_status_("Enrolled slot " + to_string(this->enroll_slot_) + " (" + name + ")");
        this->fire_homeassistant_event(
            "esphome.fingerprint_enrolled",
            {{"node", App.get_name()}, {"user_id", to_string(this->enroll_slot_)}, {"user_name", name}});
        this->last_ok_ms_ = now;
        this->pending_count_ = true;
        this->finish_enroll_();
      } else {
        this->enroll_fail_(q3);
      }
      break;

    case CMD_DELETE:
      this->state_ = STATE_IDLE;
      this->next_poll_ = now;
      if (q3 == ACK_SUCCESS) {
        this->set_status_("Deleted slot " + to_string(this->enroll_slot_));
      } else {
        this->set_status_("Delete failed: " + this->ack_str_(q3));
      }
      this->enroll_slot_ = 0;
      this->pending_count_ = true;
      break;

    case CMD_DELETE_ALL:
      this->state_ = STATE_IDLE;
      this->next_poll_ = now;
      this->set_status_(q3 == ACK_SUCCESS ? "All fingerprints deleted" : ("Delete all failed: " + this->ack_str_(q3)));
      this->pending_count_ = true;
      break;

    case CMD_COUNT:
      this->state_ = STATE_IDLE;
      this->next_poll_ = now;
      if (q3 == ACK_SUCCESS) {
        const int n = (q1 << 8) | q2;
        ESP_LOGI(TAG, "Enrolled fingerprints: %d", n);
        if (this->count_sensor_ != nullptr)
          this->count_sensor_->publish_state(n);
      } else {
        ESP_LOGW(TAG, "Count query failed: %s", this->ack_str_(q3).c_str());
      }
      break;

    case CMD_CAPTURE_TIMEOUT:
      if (this->state_ == STATE_WAIT_TO_ENROLL) {
        this->set_status_("Place finger on the sensor (1/3)");
        this->send_cmd_(CMD_ADD1, (uint8_t)(this->enroll_slot_ >> 8), (uint8_t)(this->enroll_slot_ & 0xFF), 1,
                        ENROLL_DEADLINE_MS);
        this->state_ = STATE_WAIT_ADD1;
      } else {  // boot config or post-enroll restore
        this->state_ = STATE_IDLE;
        this->next_poll_ = now;
      }
      break;

    default:
      ESP_LOGD(TAG, "Unhandled response cmd 0x%02X (%02X %02X %02X)", cmd, q1, q2, q3);
      this->state_ = STATE_IDLE;
      this->next_poll_ = now + 200;
      break;
  }
}

}  // namespace fpc1020a
}  // namespace esphome
