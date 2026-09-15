#include "BackupStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/reader/StatsBackup.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BackupStatsActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  backupFileName[0] = '\0';
  requestUpdate();
}

void BackupStatsActivity::onExit() { Activity::onExit(); }

void BackupStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BACKUP_NOW));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_CONFIRM), true);

    if (mappedInput.hasTouch()) {
      // These are real actions on X4 Pro, not passive hardware-key hints.
      // Draw two large touch buttons just like ConfirmationActivity.
      constexpr int side = 20;
      constexpr int gap = 12;
      constexpr int buttonH = 48;
      const int buttonW = (pageWidth - side * 2 - gap) / 2;
      const int buttonY = pageHeight - buttonH - 12;
      const char* leftLabel = tr(STR_CANCEL);
      const char* rightLabel = tr(STR_CONFIRM);
      renderer.drawRect(side, buttonY, buttonW, buttonH);
      renderer.drawRect(side + buttonW + gap, buttonY, buttonW, buttonH);
      const int leftW = renderer.getTextWidth(UI_10_FONT_ID, leftLabel);
      const int rightW = renderer.getTextWidth(UI_10_FONT_ID, rightLabel);
      renderer.drawText(UI_10_FONT_ID, side + (buttonW - leftW) / 2, buttonY + 10, leftLabel);
      renderer.drawText(UI_10_FONT_ID, side + buttonW + gap + (buttonW - rightW) / 2, buttonY + 10, rightLabel);
    } else {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_DONE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, backupFileName[0] != '\0' ? backupFileName : "-");

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BACKUP_STATS_FAILED), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BackupStatsActivity::runBackup() {
  LOG_DBG("BACKUP_STATS", "Creating reading-stats backup");
  state = backupGlobalStats(true, backupFileName, sizeof(backupFileName)) ? SUCCESS : FAILED;
  requestUpdate();
}

void BackupStatsActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.hasTouch()) {
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTouchDown(tx, ty) || mappedInput.wasScreenTapped(tx, ty)) {
        const int pageWidth = renderer.getScreenWidth();
        const int pageHeight = renderer.getScreenHeight();
        constexpr int side = 20;
        constexpr int gap = 12;
        constexpr int buttonH = 48;
        const int buttonW = (pageWidth - side * 2 - gap) / 2;
        const int buttonY = pageHeight - buttonH - 12;
        if (ty >= buttonY && ty < buttonY + buttonH) {
          if (tx >= side && tx < side + buttonW) {
            mappedInput.suppressTouchContact();
            goBack();
            return;
          }
          const int rightX = side + buttonW + gap;
          if (tx >= rightX && tx < rightX + buttonW) {
            mappedInput.suppressTouchContact();
            runBackup();
            return;
          }
        }
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      runBackup();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
  }
}
