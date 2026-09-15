#pragma once

#include "activities/Activity.h"
#include <HalStorage.h>

class UsbDriveActivity final : public Activity {
 public:
  explicit UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDrive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  UsbDriveState state = UsbDriveState::WaitingForHost;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool exitRequested = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long exitRequestedAt = 0;
  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 120000;
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 4000;
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1200;

  void restartHome();
};
