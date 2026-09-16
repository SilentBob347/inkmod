#include "LegacyRenderPromptActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void LegacyRenderPromptActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialConfirmRelease_ = true;
  requestUpdate(true);
}

void LegacyRenderPromptActivity::loop() {
  // X4 Pro: the prompt has its own in-card touch buttons. Handle the fresh
  // touch-down before the stale Confirm-release guard; the contact that opened
  // this activity can only contribute a release here, not a new touch-down.
  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int screenWidth = renderer.getScreenWidth();
      const int screenHeight = renderer.getScreenHeight();
      const int cardWidth = std::min(screenWidth - 36, 520);
      const int cardHeight = 230;
      const int cardX = (screenWidth - cardWidth) / 2;
      const int cardY = std::max(28, (screenHeight - cardHeight) / 2);
      constexpr int sideInset = 18;
      constexpr int gap = 12;
      constexpr int buttonH = 50;
      const int buttonY = cardY + cardHeight - buttonH - 16;
      const int buttonW = (cardWidth - sideInset * 2 - gap) / 2;
      const int noX = cardX + sideInset;
      const int yesX = noX + buttonW + gap;

      if (ty >= buttonY && ty < buttonY + buttonH) {
        if (tx >= noX && tx < noX + buttonW) {
          mappedInput.suppressTouchContact();
          ActivityResult result;
          result.isCancelled = true;
          setResult(std::move(result));
          finish();
          return;
        }
        if (tx >= yesX && tx < yesX + buttonW) {
          mappedInput.suppressTouchContact();
          ActivityResult result;
          result.isCancelled = false;
          setResult(std::move(result));
          finish();
          return;
        }
      }
    }
  }

  if (ignoreInitialConfirmRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
      ignoreInitialConfirmRelease_ = false;
      return;
    }

    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.isPressed(MappedInputManager::Button::Power)) {
      ignoreInitialConfirmRelease_ = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;  // Нет
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    ActivityResult result;
    result.isCancelled = false;  // Да
    setResult(std::move(result));
    finish();
  }
}

void LegacyRenderPromptActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const int cardWidth = std::min(screenWidth - 36, 520);
  const int cardHeight = 230;
  const int cardX = (screenWidth - cardWidth) / 2;
  // On touch boards the choices live inside the card, so center the card in
  // the whole screen instead of reserving the hidden hardware-hint strip.
  const int usableBottom = mappedInput.hasTouch() ? screenHeight : screenHeight - metrics.buttonHintsHeight - 12;
  const int cardY = std::max(28, (usableBottom - cardHeight) / 2);

  renderer.fillRect(cardX, cardY, cardWidth, cardHeight, false);
  renderer.drawRect(cardX, cardY, cardWidth, cardHeight, 2, true);

  renderer.drawCenteredText(UI_10_FONT_ID, cardY + 18, "INKMOD // AFTER DARK", true, EpdFontFamily::BOLD);

  const auto question =
      renderer.wrappedText(UI_12_FONT_ID, "Ты устал в этом бренном мире?", cardWidth - 42, 3, EpdFontFamily::BOLD);

  int y = cardY + 66;
  for (const auto& line : question) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 3;
  }


  if (mappedInput.hasTouch()) {
    constexpr int sideInset = 18;
    constexpr int gap = 12;
    constexpr int buttonH = 50;
    const int buttonY = cardY + cardHeight - buttonH - 16;
    const int buttonW = (cardWidth - sideInset * 2 - gap) / 2;
    const int noX = cardX + sideInset;
    const int yesX = noX + buttonW + gap;

    renderer.drawRect(noX, buttonY, buttonW, buttonH, 2, true);
    renderer.drawRect(yesX, buttonY, buttonW, buttonH, 2, true);
    const int noW = renderer.getTextWidth(UI_10_FONT_ID, "Нет");
    const int yesW = renderer.getTextWidth(UI_10_FONT_ID, "Да");
    const int textY = buttonY + std::max(8, (buttonH - renderer.getLineHeight(UI_10_FONT_ID)) / 2);
    renderer.drawText(UI_10_FONT_ID, noX + (buttonW - noW) / 2, textY, "Нет", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, yesX + (buttonW - yesW) / 2, textY, "Да", true, EpdFontFamily::BOLD);
  } else {
    const auto labels = mappedInput.mapLabels("Нет", "Да", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }

  // Quiet open: no forced FULL_REFRESH. This removes the visible flash/disco
  // while keeping the question screen readable and interactive.
  renderer.displayBuffer();
}
