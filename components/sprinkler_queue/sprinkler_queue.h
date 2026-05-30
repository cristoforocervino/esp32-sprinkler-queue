#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/valve/valve.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace esphome {
namespace sprinkler_queue {

// ─────────────────────────────────────────────────────────────
// Internal queue/FSM state of a single zone valve.
// ─────────────────────────────────────────────────────────────
enum class ZoneState : uint8_t {
  CLOSED = 0,  // physically closed, not in queue
  PENDING,     // queued, waiting for its turn
  OPEN,        // physically open, timer running
};

// ─────────────────────────────────────────────────────────────
// Forward declaration
// ─────────────────────────────────────────────────────────────
class SprinklerQueueController;

// ─────────────────────────────────────────────────────────────
// ZoneValve — native HA valve entity. Maps internal ZoneState
// to (position, current_operation):
//   CLOSED  -> position=0, operation=IDLE
//   PENDING -> position=0, operation=OPENING (HA shows "Opening...")
//   OPEN    -> position=1, operation=IDLE
// ─────────────────────────────────────────────────────────────
class ZoneValve : public valve::Valve, public Component {
 public:
  void set_controller(SprinklerQueueController *ctrl, uint8_t idx) {
    controller_ = ctrl;
    zone_idx_ = idx;
  }

  valve::ValveTraits get_traits() override;

 protected:
  void control(const valve::ValveCall &call) override;

  SprinklerQueueController *controller_{nullptr};
  uint8_t zone_idx_{0};
};

// ─────────────────────────────────────────────────────────────
// ZoneNumber — per-valve max duration in seconds, persisted to flash.
// ─────────────────────────────────────────────────────────────
class ZoneNumber : public number::Number, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_initial_value(float v) { initial_value_ = v; }
  void set_restore_value(bool r) { restore_value_ = r; }

 protected:
  void control(float value) override;

  float initial_value_{300.0f};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

// ─────────────────────────────────────────────────────────────
// MasterBinarySensor — read-only state of the master valve relay.
// ON whenever the master pin is physically energised (either because
// a zone is OPEN, or because the manual override switch is ON).
// ─────────────────────────────────────────────────────────────
class MasterBinarySensor : public binary_sensor::BinarySensor, public Component {};

// ─────────────────────────────────────────────────────────────
// MasterValveManualSwitch — optional HA switch that lets the user
// open the master valve manually, independently of any zone.
// Useful for debugging plumbing, pressure testing, system flushing.
//
// When ON: master pin is energised regardless of queue state.
// When OFF: master pin follows the queue logic (open iff any zone OPEN).
//
// The switch boots OFF for safety (no `restore_value`) so a power cycle
// never leaves the master open unattended.
// ─────────────────────────────────────────────────────────────
class SprinklerQueueController;  // forward declaration for set_parent()

class MasterValveManualSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(SprinklerQueueController *p) { parent_ = p; }

 protected:
  void write_state(bool state) override;
  SprinklerQueueController *parent_{nullptr};
};

// ─────────────────────────────────────────────────────────────
// PauseNumber — global pause between valves in seconds, persisted.
// ─────────────────────────────────────────────────────────────
class PauseNumber : public number::Number, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_initial_value(float v) { initial_value_ = v; }
  void set_restore_value(bool r) { restore_value_ = r; }

 protected:
  void control(float value) override;

  float initial_value_{5.0f};
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

// ─────────────────────────────────────────────────────────────
// Per-zone data bundle owned by the controller.
// ─────────────────────────────────────────────────────────────
struct ZoneEntry {
  ZoneValve *valve{nullptr};
  ZoneNumber *num{nullptr};
  GPIOPin *pin{nullptr};        // hardware output: GPIO, sn74hc595, mcp23017, ...
  ZoneState state{ZoneState::CLOSED};
};

// ─────────────────────────────────────────────────────────────
// SprinklerQueueController — central coordinator.
//
// Guarantees that at most one zone valve is open at any time. Queues
// concurrent open requests and serializes them with a configurable
// inter-valve pause to let water pressure recover.
// ─────────────────────────────────────────────────────────────
class SprinklerQueueController : public Component {
 public:
  // ── ESPHome lifecycle ──────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Run after pin expanders (sn74hc595, mcp23017, ...) finish
  // their HARDWARE-priority setup, but before user-level components.
  float get_setup_priority() const override {
    return setup_priority::IO - 5.0f;
  }

  // ── Master valve setters (optional — may stay nullptr) ────
  void set_master_pin(GPIOPin *pin) { master_pin_ = pin; }
  void set_master_binary_sensor(MasterBinarySensor *bs) { master_bs_ = bs; }
  void set_master_manual_switch(MasterValveManualSwitch *sw) { master_manual_switch_ = sw; }

  // ── Pause number setter ──────────────────────────────────
  void set_pause_number(PauseNumber *num) { pause_num_ = num; }

  // ── Zone registration (called from codegen, once per valve) ─
  void add_zone(ZoneValve *valve, ZoneNumber *num, GPIOPin *pin);

  // ── Called by ZoneValve::control ──────────────────────────
  // open=true  -> request opening (enqueue if currently CLOSED)
  // open=false -> request closing (dequeue if PENDING, close if OPEN)
  void on_valve_command(uint8_t zone_idx, bool open);

  // ── Called by MasterValveManualSwitch::write_state ────────
  // Tracks the manual override state and refreshes the master pin.
  void on_master_manual_changed(bool state);

 protected:
  // ── Queue management ──────────────────────────────────────
  void enqueue_zone(uint8_t idx);
  void dequeue_zone(uint8_t idx);
  void open_next_from_queue();
  void close_active_valve(bool skip_pause);
  void set_zone_state(uint8_t idx, ZoneState s);

  // ── Helpers ──────────────────────────────────────────────
  uint32_t pause_ms() const;
  uint32_t active_duration_ms() const;
  bool any_zone_open_() const;
  // Recomputes the desired master pin state from (manual switch OR any zone open)
  // and writes it to master_pin_ + master_bs_. Cheap to call from any path.
  void update_master_pin_();

  // ── Master valve (optional) ───────────────────────────────
  GPIOPin *master_pin_{nullptr};         // nullptr = no master valve
  MasterBinarySensor *master_bs_{nullptr};
  MasterValveManualSwitch *master_manual_switch_{nullptr};  // nullptr = no manual override
  bool master_manual_state_{false};      // cached state of the manual switch

  // ── Entities ─────────────────────────────────────────────
  std::vector<ZoneEntry> zones_;
  PauseNumber *pause_num_{nullptr};

  // ── Queue & state machine ────────────────────────────────
  // Stores zone indices (0-based into zones_) waiting to open.
  std::deque<uint8_t> queue_;

  // Index of the currently open valve (-1 = none).
  int8_t active_idx_{-1};

  enum class FSMState : uint8_t {
    IDLE,
    VALVE_OPEN,  // a valve is currently open, duration timer running
    PAUSING,     // inter-valve pause, no valve open
  };

  FSMState fsm_{FSMState::IDLE};
  uint32_t timer_start_ms_{0};
};

}  // namespace sprinkler_queue
}  // namespace esphome
