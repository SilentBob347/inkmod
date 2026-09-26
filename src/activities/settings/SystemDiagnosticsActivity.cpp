#include "SystemDiagnosticsActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <esp_system.h>
#include <esp_ota_ops.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"

namespace {
StrId resetReasonId(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return StrId::STR_DIAG_RESET_POWERON;
    case ESP_RST_EXT: return StrId::STR_DIAG_RESET_EXTERNAL;
    case ESP_RST_SW: return StrId::STR_DIAG_RESET_SOFTWARE;
    case ESP_RST_PANIC: return StrId::STR_DIAG_RESET_PANIC;
    case ESP_RST_INT_WDT: return StrId::STR_DIAG_RESET_CPU_WDT;
    case ESP_RST_TASK_WDT: return StrId::STR_DIAG_RESET_TASK_WDT;
    case ESP_RST_WDT: return StrId::STR_DIAG_RESET_WDT;
    case ESP_RST_DEEPSLEEP: return StrId::STR_DIAG_RESET_DEEP_SLEEP;
    case ESP_RST_BROWNOUT: return StrId::STR_DIAG_RESET_BROWNOUT;
    case ESP_RST_SDIO: return StrId::STR_DIAG_RESET_SDIO;
    case ESP_RST_USB: return StrId::STR_DIAG_RESET_USB;
    case ESP_RST_JTAG: return StrId::STR_DIAG_RESET_JTAG;
    case ESP_RST_EFUSE: return StrId::STR_DIAG_RESET_EFUSE;
    default: return StrId::STR_NONE_OPT;
  }
}
std::string resetReasonDisplay() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const StrId reasonId = resetReasonId(reason);
  if (reasonId != StrId::STR_NONE_OPT) return std::string(I18N.get(reasonId));
  char buf[40];
  snprintf(buf, sizeof(buf), tr(STR_DIAG_RESET_UNKNOWN), static_cast<int>(reason));
  return std::string(buf);
}
std::string sinceLastChargeDisplay() {
  const uint64_t last = powerManager.getLastChargeEpochSeconds();
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  if (last == 0 || now < last) return "-";
  const uint64_t elapsed = now - last;
  const uint32_t days = static_cast<uint32_t>(elapsed / 86400ULL);
  const uint32_t hours = static_cast<uint32_t>((elapsed % 86400ULL) / 3600ULL);
  const uint32_t minutes = static_cast<uint32_t>((elapsed % 3600ULL) / 60ULL);
  char buf[40];
  if (days) snprintf(buf, sizeof(buf), "%u %s %u %s", days, tr(STR_DIAG_DAY_SHORT), hours, tr(STR_UNIT_HOUR_SHORT));
  else if (hours) snprintf(buf, sizeof(buf), "%u %s %u %s", hours, tr(STR_UNIT_HOUR_SHORT), minutes,
                           tr(STR_UNIT_MIN_SHORT));
  else snprintf(buf, sizeof(buf), "%u %s", minutes, tr(STR_UNIT_MIN_SHORT));
  return std::string(buf);
}
std::string displayControllerDisplay() {
  using DC = BoardConfig::DisplayController;
  const auto controller = BoardConfig::ACTIVE.displayController;
  const uint8_t variant = BoardConfig::ACTIVE.displayControllerVariant;
  char buf[40];
  switch (controller) {
    case DC::SSD1677:
      return "SSD1677";
    case DC::UC8179:
      return "UC8179";
    case DC::UC8279:
      if ((BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) && variant != 0) {
        snprintf(buf, sizeof(buf), "UC8279 / LUT %02X", static_cast<unsigned>(variant));
        return std::string(buf);
      }
      return "UC8279";
    case DC::UC8253:
      return "UC8253";
    default:
      return "Other";
  }
}

std::string bytesHuman(uint64_t bytes) {
  char buf[32];
  if (bytes >= 1073741824ULL) snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1073741824.0);
  else if (bytes >= 1048576ULL) snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  else snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>(bytes / 1024ULL));
  return buf;
}

using DiagnosticRows = std::vector<std::pair<std::string, std::string>>;

DiagnosticRows buildDiagnosticRows() {
  const auto heap = MemoryBudget::snapshot();
  const bool crashReport = Storage.exists("/crash_report.txt");
  DiagnosticRows rows;
  rows.reserve(12);
  rows.push_back({tr(STR_DIAG_FIRMWARE), std::string(INKMOD_VERSION) + " (" + INKMOD_FIRMWARE_VARIANT + ")"});
  const char* deviceName = BoardConfig::isX4Pro() ? "X4 Pro"
                           : BoardConfig::isX4Classic() ? "X4 Classic"
                           : gpio.deviceIsX3() ? "X3"
                                              : "X4";
  rows.push_back({tr(STR_DIAG_DEVICE), deviceName});
  if (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) {
    rows.push_back({tr(STR_CAT_DISPLAY), displayControllerDisplay()});
  }
  rows.push_back({tr(STR_DIAG_FREE_HEAP), bytesHuman(heap.freeHeap)});
  rows.push_back({tr(STR_DIAG_MAX_ALLOC), bytesHuman(heap.maxAllocHeap)});
  rows.push_back({tr(STR_INTERNAL_STORAGE), StorageUsageCalc::display()});
  rows.push_back({tr(STR_BATTERY), std::to_string(powerManager.getBatteryPercentage()) + "%"});
  if (!SETTINGS.clockDisabled) rows.push_back({tr(STR_SINCE_LAST_CHARGE), sinceLastChargeDisplay()});
  rows.push_back({tr(STR_DIAG_RESET_REASON), resetReasonDisplay()});
  rows.push_back({tr(STR_DIAG_CRASH_REPORT), crashReport ? tr(STR_YES) : tr(STR_NO)});
  return rows;
}

bool diagnosticRowNeedsTwoLines(const GfxRenderer& renderer, const std::pair<std::string, std::string>& row,
                                const int availableWidth) {
  constexpr int kLabelValueGap = 10;
  return renderer.getTextWidth(UI_10_FONT_ID, row.first.c_str()) + kLabelValueGap +
             renderer.getTextWidth(UI_10_FONT_ID, row.second.c_str()) >
         availableWidth;
}
}

void SystemDiagnosticsActivity::onEnter() {
  Activity::onEnter();
#ifdef ENABLE_SERIAL_LOG
  const size_t renderStackFree = activityManager.getRenderTaskStackHighWaterMark();
  LOG_INF("DIAG", "Render task stack minimum free=%u B of 16384 B", static_cast<unsigned>(renderStackFree));
#endif
  if (!StorageUsageCalc::ready()) StorageUsageCalc::start();
  storageReadySeen = StorageUsageCalc::ready();
  requestUpdate(true);
}

void SystemDiagnosticsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }

  buttonNavigator.onPreviousRelease([this] { selectedAction = ButtonNavigator::previousIndex(selectedAction, ACTION_COUNT); requestUpdate(); });
  buttonNavigator.onNextRelease([this] { selectedAction = ButtonNavigator::nextIndex(selectedAction, ACTION_COUNT); requestUpdate(); });
  buttonNavigator.onPreviousContinuous([this] { selectedAction = ButtonNavigator::previousIndex(selectedAction, ACTION_COUNT); requestUpdate(); });
  buttonNavigator.onNextContinuous([this] { selectedAction = ButtonNavigator::nextIndex(selectedAction, ACTION_COUNT); requestUpdate(); });

  bool touchActivate = false;
  if (mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int left = metrics.contentSidePadding;
      const int right = pageWidth - metrics.contentSidePadding;
      int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const auto rows = buildDiagnosticRows();
      for (const auto& row : rows) {
        const bool twoLines = diagnosticRowNeedsTwoLines(renderer, row, right - left);
        const int neededH = twoLines ? (lineH * 2 + metrics.verticalSpacing) : lineH;
        if (y + neededH >= pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing) break;
        y += lineH + metrics.verticalSpacing;
        if (twoLines) y += lineH + metrics.verticalSpacing;
      }
      y += metrics.verticalSpacing;
      if (ty >= y) {
        const int row = (ty - y) / (lineH + metrics.verticalSpacing);
        if (row >= 0 && row < ACTION_COUNT) {
          selectedAction = row;
          touchActivate = true;
          requestUpdate();
        }
      }
    }
  }

  if (touchActivate || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedAction == 0) {
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
      requestUpdate(true);
    } else if (selectedAction == 1) {
      startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(true); });
    } else {
      const esp_partition_t* running = esp_ota_get_running_partition();
      const esp_partition_t* alternate = running ? esp_ota_get_next_update_partition(running) : nullptr;
      size_t imageSize = 0;
      const auto validation = firmware_flash::validateImagePartition(alternate, &imageSize);
      if (!alternate || validation != firmware_flash::Result::OK) {
        GUI.drawPopup(renderer, tr(STR_OTHER_SLOT_INVALID)); renderer.displayBuffer(); delay(900); requestUpdate(true);
      } else {
        char body[112];
        snprintf(body, sizeof(body), tr(STR_BOOT_OTHER_SLOT_CONFIRM), alternate->label,
                 static_cast<unsigned long>(imageSize / 1024u));
        startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOT_OTHER_SLOT), body),
          [this, alternate](const ActivityResult& result) {
            if (result.isCancelled) { requestUpdate(true); return; }
            if (!ota_boot::switchTo(alternate)) {
              GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE)); renderer.displayBuffer(); delay(900); requestUpdate(true); return;
            }
            GUI.drawPopup(renderer, tr(STR_RESTARTING_HINT)); renderer.displayBuffer(); delay(400); ESP.restart();
          });
      }
    }
  }

  const bool readyNow = StorageUsageCalc::ready();
  if (readyNow != storageReadySeen) { storageReadySeen = readyNow; requestUpdate(true); }
}

void SystemDiagnosticsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth(), pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DIAGNOSTICS));
  const auto rows = buildDiagnosticRows();

  const int left = metrics.contentSidePadding, right = pageWidth - metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& row : rows) {
    const bool twoLines = diagnosticRowNeedsTwoLines(renderer, row, right - left);
    const int neededH = twoLines ? (lineH * 2 + metrics.verticalSpacing) : lineH;
    if (y + neededH >= pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing) break;

    renderer.drawText(UI_10_FONT_ID, left, y, row.first.c_str());
    const int valueW = renderer.getTextWidth(UI_10_FONT_ID, row.second.c_str());
    if (twoLines) {
      y += lineH + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, std::max(left, right - valueW), y, row.second.c_str());
    } else {
      renderer.drawText(UI_10_FONT_ID, std::max(left, right - valueW), y, row.second.c_str());
    }
    y += lineH + metrics.verticalSpacing;
  }
  y += metrics.verticalSpacing;
  const char* actions[ACTION_COUNT] = {tr(STR_DIAG_FORCE_SCREEN_CLEAR), tr(STR_SD_FIRMWARE_UPDATE),
                                       tr(STR_BOOT_OTHER_SLOT)};
  for (int i = 0; i < ACTION_COUNT && y + lineH < pageHeight - metrics.buttonHintsHeight; ++i) {
    if (i == selectedAction) renderer.fillRect(left - 4, y - 2, right - left + 8, lineH + 4, true);
    renderer.drawText(UI_10_FONT_ID, left, y, actions[i], i != selectedAction);
    y += lineH + metrics.verticalSpacing;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
