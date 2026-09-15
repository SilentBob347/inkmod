#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::loop() {
  if (mappedInput.hasTouch()) {
    auto& theme = UITheme::getInstance();
    const auto metrics = theme.getMetrics();
    const Rect screen = theme.getScreenSafeArea(renderer, true, false);
    const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
    constexpr int barWidth = 360;
    const int barX = screen.x + (screen.width - barWidth) / 2;
    const int barY = contentTop + metrics.verticalSpacing * 2;

    int tx = 0, ty = 0;
    if (mappedInput.isScreenTouchHeld(tx, ty) && ty >= barY - 20 && ty <= barY + 36) {
      const int clampedX = std::clamp(tx, barX + 2, barX + barWidth - 2);
      const int candidate = ((clampedX - (barX + 2)) * 100 + (barWidth - 4) / 2) / (barWidth - 4);
      if (candidate != percent) {
        percent = std::clamp(candidate, 0, 100);
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasScreenTapped(tx, ty) && ty >= renderer.getScreenHeight() - 86) {
      if (tx < renderer.getScreenWidth() / 2) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
      } else {
        setResult(PercentResult{percent});
      }
      finish();
      return;
    }
  }

  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PercentResult{percent});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustPercent(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustPercent(-kLargeStep); });
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_GO_TO_PERCENT), nullptr, true);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;

  const std::string percentText = std::to_string(percent) + "%";
  UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, contentTop, percentText.c_str(), true,
                            EpdFontFamily::BOLD);

  // Draw slider track.
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = screen.x + (screen.width - barWidth) / 2;
  const int barY = contentTop + metrics.verticalSpacing * 2;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on percent.
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current percent.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Hint text for step sizes.
  UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, barY + 30, tr(STR_PERCENT_STEP_HINT), true);

  if (mappedInput.hasTouch()) {
    const int screenWidth = renderer.getScreenWidth();
    const int screenHeight = renderer.getScreenHeight();
    const int margin = 14;
    const int gap = 12;
    const int buttonY = screenHeight - 72;
    const int buttonH = 54;
    const int buttonW = (screenWidth - margin * 2 - gap) / 2;
    const int leftX = margin;
    const int rightX = leftX + buttonW + gap;
    renderer.drawRect(leftX, buttonY, buttonW, buttonH);
    renderer.drawRect(rightX, buttonY, buttonW, buttonH);
    const int cancelW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CANCEL));
    renderer.drawText(UI_10_FONT_ID, leftX + std::max(0, (buttonW - cancelW) / 2), buttonY + 15, tr(STR_CANCEL));
    const int goW = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_GO_TO_PERCENT));
    renderer.drawText(UI_10_FONT_ID, rightX + std::max(0, (buttonW - goW) / 2), buttonY + 15, tr(STR_GO_TO_PERCENT));
  } else {
    // Button hints follow the current front button layout.
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }

  renderer.displayBuffer();
}
