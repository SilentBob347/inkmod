#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public Activity {
 public:
  // Slider-style percent selector for jumping within a book.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent)
      : Activity("EpubReaderPercentSelection", renderer, mappedInput), percent(initialPercent) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  // Current percent value (0-100) shown on the slider.
  int percent = 0;

  ButtonNavigator buttonNavigator;

  // X4 Pro has only the physical Left/Right navigation buttons. A short press
  // changes the target by 1%, while holding the same button changes it by 10%
  // exactly once when the long-press threshold is reached.
  bool x4ProLeftLongHandled = false;
  bool x4ProRightLongHandled = false;

  // Change the current percent by a delta and clamp within bounds.
  void adjustPercent(int delta);
};
