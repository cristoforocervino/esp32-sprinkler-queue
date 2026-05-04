#include "sprinkler_queue.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace sprinkler_queue {

static const char *const TAG = "sprinkler_queue";

// ─────────────────────────────────────────────────────────────
// ZoneValve
// ─────────────────────────────────────────────────────────────
valve::ValveTraits ZoneValve::get_traits() {
  auto traits = valve::ValveTraits();
  traits.set_is_assumed_state(false);   // we know the relay state precisely
  traits.set_supports_position(false);  // binary on/off, no proportional position
  traits.set_supports_stop(false);
  traits.set_supports_toggle(true);     // allow toggle commands from HA UI
  return traits;
}

void ZoneValve::control(const valve::ValveCall &call) {
  if (controller_ == nullptr)
    return;

  // Stop is interpreted as "close" for our binary valves
  if (call.get_stop()) {
    controller_->on_valve_command(zone_idx_, false);
    return;
  }

  // Position-based command (0.0 close, 1.0 open)
  auto pos = call.get_position();
  if (pos.has_value()) {
    bool open = (*pos == valve::VALVE_OPEN);
    controller_->on_valve_command(zone_idx_, open);
    return;
  }

  // Toggle: invert current state
  if (call.get_toggle().has_value()) {
    bool currently_closed = this->is_fully_closed();
    controller_->on_valve_command(zone_idx_, currently_closed);
  }
}

// ─────────────────────────────────────────────────────────────
// ZoneNumber
// ─────────────────────────────────────────────────────────────
void ZoneNumber::setup() {
  float value = initial_value_;
  if (restore_value_) {
    pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    if (!pref_.load(&value))
      value = initial_value_;
  }
  this->publish_state(value);
}

void ZoneNumber::control(float value) {
  this->publish_state(value);
  if (restore_value_)
    pref_.save(&value);
}

void ZoneNumber::dump_config() {
  LOG_NUMBER("", "Zone Duration Number", this);
}

// ─────────────────────────────────────────────────────────────
// PauseNumber
// ─────────────────────────────────────────────────────────────
void PauseNumber::setup() {
  float value = initial_value_;
  if (restore_value_) {
    pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    if (!pref_.load(&value))
      value = initial_value_;
  }
  this->publish_state(value);
}

void PauseNumber::control(float value) {
  this->publish_state(value);
  if (restore_value_)
    pref_.save(&value);
}

void PauseNumber::dump_config() {
  LOG_NUMBER("", "Pause Between Valves Number", this);
}

// ─────────────────────────────────────────────────────────────
// SprinklerQueueController — add_zone
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::add_zone(ZoneValve *valve, ZoneNumber *num, GPIOPin *pin) {
  zones_.push_back({valve, num, pin, ZoneState::CLOSED});
}

// ─────────────────────────────────────────────────────────────
// SprinklerQueueController — setup
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::setup() {
  // Initialise master valve pin (if configured) and force closed
  if (master_pin_ != nullptr) {
    master_pin_->setup();
    master_pin_->digital_write(false);
  }

  // Initialise each zone pin and force closed
  for (auto &z : zones_) {
    if (z.pin != nullptr) {
      z.pin->setup();
      z.pin->digital_write(false);
    }
  }

  // Wire each ZoneValve back to us and publish initial CLOSED state
  for (uint8_t i = 0; i < zones_.size(); i++) {
    zones_[i].valve->set_controller(this, i);
    set_zone_state(i, ZoneState::CLOSED);
  }

  if (master_bs_ != nullptr)
    master_bs_->publish_state(false);
}

// ─────────────────────────────────────────────────────────────
// SprinklerQueueController — loop (FSM)
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::loop() {
  uint32_t now = millis();

  switch (fsm_) {
    case FSMState::IDLE:
      if (!queue_.empty())
        open_next_from_queue();
      break;

    case FSMState::VALVE_OPEN: {
      uint32_t elapsed = (uint32_t)(now - timer_start_ms_);
      if (elapsed >= active_duration_ms()) {
        ESP_LOGI(TAG, "Zone %d duration elapsed, closing", active_idx_ + 1);
        close_active_valve(false);
      }
      break;
    }

    case FSMState::PAUSING: {
      uint32_t elapsed = (uint32_t)(now - timer_start_ms_);
      if (elapsed >= pause_ms()) {
        ESP_LOGD(TAG, "Pause elapsed, resuming queue");
        fsm_ = FSMState::IDLE;
      }
      break;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// on_valve_command — called by ZoneValve::control
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::on_valve_command(uint8_t idx, bool open) {
  if (idx >= zones_.size())
    return;

  ZoneEntry &z = zones_[idx];

  if (open) {
    // Idempotent: already pending or open
    if (z.state == ZoneState::PENDING || z.state == ZoneState::OPEN) {
      // Re-publish to keep HA in sync (e.g. after a reconnection)
      set_zone_state(idx, z.state);
      return;
    }
    enqueue_zone(idx);
  } else {
    if (z.state == ZoneState::OPEN) {
      // Currently open — close immediately, skip pause
      close_active_valve(true);
    } else if (z.state == ZoneState::PENDING) {
      // In queue — remove it
      dequeue_zone(idx);
    }
    // Already CLOSED: nothing to do
  }
}

// ─────────────────────────────────────────────────────────────
// enqueue_zone
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::enqueue_zone(uint8_t idx) {
  queue_.push_back(idx);
  set_zone_state(idx, ZoneState::PENDING);
  ESP_LOGI(TAG, "Zone %d queued (queue depth=%d)", idx + 1, (int) queue_.size());
}

// ─────────────────────────────────────────────────────────────
// dequeue_zone — remove from queue without opening
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::dequeue_zone(uint8_t idx) {
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (*it == idx) {
      queue_.erase(it);
      break;
    }
  }
  set_zone_state(idx, ZoneState::CLOSED);
  ESP_LOGI(TAG, "Zone %d removed from queue", idx + 1);
}

// ─────────────────────────────────────────────────────────────
// open_next_from_queue
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::open_next_from_queue() {
  if (queue_.empty())
    return;

  uint8_t idx = queue_.front();
  queue_.pop_front();

  active_idx_ = (int8_t) idx;
  timer_start_ms_ = millis();
  fsm_ = FSMState::VALVE_OPEN;

  // Open master first, then the zone (avoids back-pressure spike on the zone)
  if (master_pin_ != nullptr)
    master_pin_->digital_write(true);

  if (zones_[idx].pin != nullptr)
    zones_[idx].pin->digital_write(true);

  set_zone_state(idx, ZoneState::OPEN);

  if (master_bs_ != nullptr)
    master_bs_->publish_state(true);

  ESP_LOGI(TAG, "Zone %d opened (duration=%.0fs)", idx + 1,
           active_duration_ms() / 1000.0f);
}

// ─────────────────────────────────────────────────────────────
// close_active_valve
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::close_active_valve(bool skip_pause) {
  if (active_idx_ < 0)
    return;

  uint8_t idx = (uint8_t) active_idx_;

  // Close zone first, then master
  if (zones_[idx].pin != nullptr)
    zones_[idx].pin->digital_write(false);

  if (master_pin_ != nullptr)
    master_pin_->digital_write(false);

  set_zone_state(idx, ZoneState::CLOSED);

  if (master_bs_ != nullptr)
    master_bs_->publish_state(false);

  active_idx_ = -1;

  uint32_t p = pause_ms();
  if (skip_pause || p == 0) {
    fsm_ = FSMState::IDLE;
    ESP_LOGI(TAG, "Zone %d closed (no pause)", idx + 1);
  } else {
    fsm_ = FSMState::PAUSING;
    timer_start_ms_ = millis();
    ESP_LOGI(TAG, "Zone %d closed, pausing %dms before next", idx + 1, p);
  }
}

// ─────────────────────────────────────────────────────────────
// set_zone_state — updates state and publishes the valve entity
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::set_zone_state(uint8_t idx, ZoneState s) {
  ZoneEntry &z = zones_[idx];
  z.state = s;

  switch (s) {
    case ZoneState::CLOSED:
      z.valve->position = valve::VALVE_CLOSED;
      z.valve->current_operation = valve::VALVE_OPERATION_IDLE;
      break;
    case ZoneState::PENDING:
      // Physically still closed, but HA should show "Opening..." (queued)
      z.valve->position = valve::VALVE_CLOSED;
      z.valve->current_operation = valve::VALVE_OPERATION_OPENING;
      break;
    case ZoneState::OPEN:
      z.valve->position = valve::VALVE_OPEN;
      z.valve->current_operation = valve::VALVE_OPERATION_IDLE;
      break;
  }
  z.valve->publish_state();
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────
uint32_t SprinklerQueueController::active_duration_ms() const {
  if (active_idx_ < 0 || (size_t) active_idx_ >= zones_.size())
    return 0;
  // num->state may be NaN if the number entity hasn't published yet;
  // fall back to a sane default of 300 seconds.
  float secs = zones_[(size_t) active_idx_].num->state;
  if (std::isnan(secs) || secs <= 0.0f)
    secs = 300.0f;
  return (uint32_t)(secs * 1000.0f);
}

uint32_t SprinklerQueueController::pause_ms() const {
  if (pause_num_ == nullptr)
    return 0;
  float secs = pause_num_->state;
  if (std::isnan(secs) || secs < 0.0f)
    return 0;
  return (uint32_t)(secs * 1000.0f);
}

// ─────────────────────────────────────────────────────────────
// dump_config
// ─────────────────────────────────────────────────────────────
void SprinklerQueueController::dump_config() {
  ESP_LOGCONFIG(TAG, "Sprinkler Queue Controller:");
  ESP_LOGCONFIG(TAG, "  Zones: %d", (int) zones_.size());
  ESP_LOGCONFIG(TAG, "  Master valve: %s", master_pin_ ? "configured" : "none");
}

}  // namespace sprinkler_queue
}  // namespace esphome
