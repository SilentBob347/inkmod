#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
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

  // Last good battery value. Gauge-backed boards store 0..100; the ADC X4
  // path stores tenths of a percent (0..1000) for smoothing. Runtime board
  // selection is fixed for the lifetime of the process, so the representations
  // never mix.
  mutable int _batteryCachedPercent = 0;
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
  mutable unsigned long _chargeCheckLastPollMs = 0;  // Debounce timestamp for charge-state tracking

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
  // ESP32-C3 is stable at 10 MHz. X4 Pro's ESP32-S3 uses PSRAM; keep APB/PSRAM
  // timing in the supported range while still cutting idle CPU power sharply.
  static constexpr int LOW_POWER_FREQ = FREEINK_MCU_S3 ? 80 : 10;  // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 500;   // downclock after 0.5 s idle
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Updates the internal "last charging observed" timestamp from HalGPIO's
  // cached USB state. Self-debounced and free of hardware I/O, so it is cheap
  // to call from the main loop.
  void trackChargingState() const;

  // time(nullptr) value (wall-clock epoch seconds) as of the last time
  // trackChargingState() observed USB charging; 0 if never observed this
  // boot/deep-sleep-retained cycle. Kept in RTC memory, not persisted to
  // SD directly (the app layer separately persists this to
  // InkMODState::lastChargeEpochSeconds on real transitions - see
  // main.cpp's loop() - since RTC memory alone doesn't survive this
  // device's battery-only deep sleep, which fully powers the MCU off).
  // Wall-clock time specifically (not esp_timer_get_time(), which resets
  // to ~0 on every wake from that same battery-only sleep) is what makes
  // a value from a previous session still comparable to "now" after a
  // sleep cycle - see trackChargingState()'s own comment for why that
  // distinction mattered here.
  uint64_t getLastChargeEpochSeconds() const;

  // Explicitly record a known charging/USB session using the current wall clock.
  // Used on X4 Pro where the board has no proven dedicated VBUS-detect GPIO.
  // Returns false until RTC/NTP has supplied a sane wall-clock value.
  bool markChargingNow() const;

  // Called once at startup with the value persisted from the previous
  // session (InkMODState::lastChargeEpochSeconds), so "since last charge"
  // has something to show before this boot's own trackChargingState()
  // has observed anything - RTC memory survives deep sleep on its own but
  // resets on a fresh flash/reset, unlike the persisted copy this seeds
  // from. Only takes effect if nothing has been observed yet this boot
  // (i.e. the RTC copy is still its power-on-reset default of 0) - a real
  // charging session already tracked this boot should never be overwritten
  // by an older, persisted value.
  void seedLastChargeEpochSeconds(uint64_t persistedValue);

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
