#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ctime>

#include "HalGPIO.h"

namespace {
RTC_DATA_ATTR uint64_t lastChargeEpochSeconds = 0;
}  // namespace

HalPowerManager powerManager;  // Singleton instance

namespace {
void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(true);
  }
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}
}  // namespace

void HalPowerManager::begin() {
#if FREEINK_MCU_C3
  // The X3 hardware fingerprint temporarily opens and then closes Wire.
  // X3's BQ27220, DS3231 and QMI8658 all share this I2C bus, and HalClock /
  // HalTiltSensor expect it to remain initialized after powerManager.begin().
  // v1.1.9 accidentally dropped this re-initialization, which made the RTC
  // probe fail and hid the clock and all clock settings on X3.
  if (gpio.deviceIsX3()) {
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
  } else if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
#else
  // S3 boards use the SDK-owned I2C backends.
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
#endif
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;
  }


  // This function is called on nearly every main-loop pass. Most calls ask to
  // keep normal speed while we are already at normal speed; avoid touching the
  // WiFi driver or CPU-frequency API in that overwhelmingly common no-op case.
  const LockMode mode = currentLockMode;
  if (!enabled && !isLowPower) {
    return;
  }
  if (enabled && mode != None && !isLowPower) {
    return;
  }

  if (enabled) {
    const wifi_mode_t wifiMode = WiFi.getMode();
    if (wifiMode != WIFI_MODE_NULL) {
      enabled = false;
    }
  }

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  disableWiFiBeforeDeepSleep();

#ifdef ENABLE_SERIAL_LOG
  logSerial.end();
#endif

#if FREEINK_MCU_C3
  // Preserve inkMOD's proven X3/X4 C3 shutdown path unchanged.
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
#else
  // X4 Pro: keep every configured board power latch asserted through deep sleep.
  // CrossPoint 1.6.0 does this because GPIO isolation otherwise lets the X4 Pro
  // master rail float/drop after external USB power is removed.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // X4 Pro: use the SDK's S3-correct rail shutdown + wake-source implementation.
  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
#endif
}

void HalPowerManager::trackChargingState() const {
  const unsigned long now = millis();
  if (_chargeCheckLastPollMs != 0 && (now - _chargeCheckLastPollMs) < BATTERY_POLL_MS) return;
  _chargeCheckLastPollMs = now;

  if (gpio.isUsbConnectedCached()) {
    markChargingNow();
  }
}

bool HalPowerManager::markChargingNow() const {
  const time_t now = time(nullptr);
  // Do not poison the persisted value with the Unix-epoch-ish clock seen before
  // BM8563/NTP has initialised libc time. 2020-01-01 UTC is a deliberately
  // conservative lower bound for every real inkMOD installation.
  constexpr time_t kMinSaneEpoch = 1577836800;
  if (now < kMinSaneEpoch) return false;
  lastChargeEpochSeconds = static_cast<uint64_t>(now);
  return true;
}

uint64_t HalPowerManager::getLastChargeEpochSeconds() const { return lastChargeEpochSeconds; }

void HalPowerManager::seedLastChargeEpochSeconds(const uint64_t persistedValue) {
  if (lastChargeEpochSeconds == 0) {
    lastChargeEpochSeconds = persistedValue;
  }
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  trackChargingState();
  const unsigned long now = millis();

  // Gauge-backed boards (X3 BQ27220, X4 Pro CW2017): keep the last good value
  // across transient I2C failures instead of falling through to a nonexistent
  // ADC. X4 Pro's BoardConfig points BatteryMonitor at the real CW2017 SOC
  // register and OEM BATINFO profile.
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return static_cast<uint16_t>(std::clamp(_batteryCachedPercent, 0, 100));
    }
    static const BatteryMonitor gaugeBattery;
    uint16_t percent = 0;
    if (gaugeBattery.readPercentageChecked(percent)) {
      _batteryCachedPercent = std::min<uint16_t>(100, percent);
    }
    _batteryLastPollMs = now;
    return static_cast<uint16_t>(std::clamp(_batteryCachedPercent, 0, 100));
  }

  // ADC-backed plain X4: retain the existing 0.1%-resolution smoothing.
  if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return static_cast<uint16_t>(_batteryCachedPercent / 10);
  }

  static const BatteryMonitor battery;
  const uint16_t millivolts = battery.readMillivolts();
  const uint16_t rawPercent = BatteryMonitor::percentageFromMillivolts(millivolts);
  LOG_DBG("PWR", "X4 battery: %umV raw=%u%% cached=%d.%d%% usb=%d", millivolts, rawPercent,
          _batteryCachedPercent / 10, std::abs(_batteryCachedPercent % 10), gpio.isUsbConnectedCached() ? 1 : 0);

  constexpr uint16_t X4_USB_FULL_PERCENT = 95;
  constexpr uint16_t X4_USB_FULL_MV = 4080;
  if (gpio.isUsbConnectedCached() && (rawPercent >= X4_USB_FULL_PERCENT || millivolts >= X4_USB_FULL_MV)) {
    _batteryCachedPercent = 1000;
    _batteryLastPollMs = now;
    return 100;
  }

  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * rawPercent;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + rawPercent * 10) / 10;
  }
  _batteryLastPollMs = now;
  return static_cast<uint16_t>(_batteryCachedPercent / 10);
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}