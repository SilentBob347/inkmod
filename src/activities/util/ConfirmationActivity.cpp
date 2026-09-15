#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           bool ignoreInitialConfirmRelease)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      ignoreConfirmRelease(ignoreInitialConfirmRelease) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);
  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = safeArea.width - (margin * 2);
  const int contentTop = safeArea.y + margin;
  // Touch devices need real action buttons in confirmation dialogs. They are
  // not passive hardware-key hints, so reserve room for them even though the
  // global X4 Pro theme hides normal on-screen button hints.
  const int touchActionReserve = mappedInput.hasTouch() ? 66 : 0;
  const int contentBottom = safeArea.y + safeArea.height - margin - touchActionReserve;
  const int contentHeight = contentBottom - contentTop;
  const int maxTotalLines = std::max(1, contentHeight / lineHeight);

  if (!heading.empty()) {
    const int headingLineCap = body.empty() ? maxTotalLines : std::min(2, std::max(1, maxTotalLines - 1));
    headingLines = renderer.wrappedText(fontId, heading.c_str(), maxWidth, headingLineCap, EpdFontFamily::BOLD);
  }
  int totalHeight = 0;
  if (!headingLines.empty()) totalHeight += static_cast<int>(headingLines.size()) * lineHeight;
  if (!body.empty()) {
    const int bodyGap = headingLines.empty() ? 0 : spacing;
    const int bodyHeight = std::max(lineHeight, contentHeight - totalHeight - bodyGap);
    const int bodyLineCap = std::max(1, bodyHeight / lineHeight);
    bodyLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, bodyLineCap, EpdFontFamily::REGULAR);
    if (!bodyLines.empty()) {
      totalHeight += bodyGap + static_cast<int>(bodyLines.size()) * lineHeight;
    }
  }

  // Center the dialog text inside the space above the bottom button hints.
  startY = contentTop + std::max(0, (contentHeight - totalHeight) / 2);

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  for (const auto& line : headingLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight;
  }

  if (!headingLines.empty() && !bodyLines.empty()) {
    currentY += spacing;
  }

  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  // Confirmation buttons are real actions on touch devices, not hardware-key
  // hints. Keep them visible on X4 Pro while the rest of the UI stays clean.
  if (mappedInput.hasTouch()) {
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    constexpr int side = 20;
    constexpr int gap = 12;
    constexpr int buttonH = 48;
    const int buttonW = (pageWidth - side * 2 - gap) / 2;
    const int buttonY = pageHeight - buttonH - 12;
    const bool deleteDialog = heading.find(I18N.get(StrId::STR_DELETE)) != std::string::npos;
    const char* leftLabel = deleteDialog ? I18N.get(StrId::STR_NO) : I18N.get(StrId::STR_CANCEL);
    const char* rightLabel = deleteDialog ? I18N.get(StrId::STR_DELETE) : I18N.get(StrId::STR_CONFIRM);
    renderer.drawRect(side, buttonY, buttonW, buttonH);
    renderer.drawRect(side + buttonW + gap, buttonY, buttonW, buttonH);
    const int leftTextW = renderer.getTextWidth(UI_10_FONT_ID, leftLabel);
    const int rightTextW = renderer.getTextWidth(UI_10_FONT_ID, rightLabel);
    renderer.drawText(UI_10_FONT_ID, side + (buttonW - leftTextW) / 2, buttonY + 10, leftLabel);
    renderer.drawText(UI_10_FONT_ID, side + buttonW + gap + (buttonW - rightTextW) / 2, buttonY + 10, rightLabel);
  } else {
    const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;

    // React as soon as the contact has been stable for the touch backend's
    // short tap-candidate delay (~90 ms).  Waiting exclusively for release
    // made confirmation dialogs feel as if the first tap was ignored,
    // especially on e-ink where the action itself can take noticeable time.
    // Keep the completed-tap path as a fallback for very quick contacts.
    const bool touchAction = mappedInput.wasScreenTouchDown(tx, ty) || mappedInput.wasScreenTapped(tx, ty);
    if (touchAction) {
      const int pageWidth = renderer.getScreenWidth();
      const int pageHeight = renderer.getScreenHeight();
      constexpr int side = 20;
      constexpr int gap = 12;
      constexpr int buttonH = 48;
      const int buttonW = (pageWidth - side * 2 - gap) / 2;
      const int buttonY = pageHeight - buttonH - 12;
      // Keep the drawn buttons compact, but make the finger target taller.
      // The extra area is split only horizontally, so adjacent actions never
      // overlap. This makes confirmation reliable without changing the theme.
      constexpr int touchPadY = 14;
      if (ty >= buttonY - touchPadY && ty < buttonY + buttonH + touchPadY) {
        ActivityResult res;
        if (tx >= 0 && tx < side + buttonW + gap / 2) {
          res.isCancelled = true;
          mappedInput.suppressTouchContact();
          setResult(std::move(res));
          finish();
          return;
        }
        const int rightX = side + buttonW + gap / 2;
        if (tx >= rightX && tx < pageWidth) {
          res.isCancelled = false;
          mappedInput.suppressTouchContact();
          setResult(std::move(res));
          finish();
          return;
        }
      }
    }
  }

  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
