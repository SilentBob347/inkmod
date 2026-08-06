#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  // I2C fuel gauge configuration for X3 battery monitoring
  bool _batteryUseI2C = false;            // True if using I2C fuel gauge (X3), false for ADC (X4)
  mutable int _batteryCachedPercent = 0;  // Last read battery percentage * 10 (0-1000); callers divide by 10 (ADC/X4
                                          // path only — I2C/X3 path stores 0-100 directly)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
  mutable unsigned long _chargeCheckLastPollMs = 0;  // Debounce timestamp for charge-state tracking

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
  static constexpr int LOW_POWER_FREQ = 10;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Updates the internal "last charging observed" timestamp whenever USB
  // charging is currently detected. Self-debounced; cheap to call from
  // getBatteryPercentage() on every poll.
  void trackChargingState() const;

  // esp_timer_get_time() value (microseconds since boot) as of the last
  // time trackChargingState() observed USB charging; 0 if never observed
  // this boot/deep-sleep-retained cycle. Kept in RTC memory (survives deep
  // sleep, resets together with esp_timer_get_time() on a hard reset - see
  // the .cpp), not persisted to SD. For Settings -> System -> Device's
  // "time since last charge" line: callers should treat a value greater
  // than the current esp_timer_get_time() as invalid (a hard reset
  // happened since it was set) rather than display it.
  uint64_t getLastChargeMonotonicUs() const;

  // Called once at startup with the value persisted from the previous
  // session (InkMODState::lastChargeMonotonicUs), so "since last charge"
  // has something to show before this boot's own trackChargingState()
  // has observed anything - RTC memory survives deep sleep on its own but
  // resets on a fresh flash/reset, unlike the persisted copy this seeds
  // from. Only takes effect if nothing has been observed yet this boot
  // (i.e. the RTC copy is still its power-on-reset default of 0) - a real
  // charging session already tracked this boot should never be overwritten
  // by an older, persisted value.
  void seedLastChargeMonotonicUs(uint64_t persistedValue);

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
