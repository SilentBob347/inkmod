#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNavigation.h"

namespace {
constexpr int MENU_ITEM_COUNT = 4 + (FREEINK_CAP_USB_MSC ? 1 : 0);
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  bool touchActivate = false;
  if (mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing * 2;
    const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int subtitleLineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int touchRowHeight = std::max(metrics.listWithSubtitleRowHeight, titleLineH + 4 + subtitleLineH + 6);
    auto touch = TouchListNavigation::handle(mappedInput, selectedIndex, MENU_ITEM_COUNT,
                                             Rect{safeArea.x, contentTop, safeArea.width, contentHeight},
                                             touchRowHeight);
    if (touch.handled && !touch.activate) { requestUpdate(); return; }
    touchActivate = touch.activate;
  }

  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (touchActivate || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    NetworkMode mode = NetworkMode::JOIN_NETWORK;
    if (selectedIndex == 1) {
      mode = NetworkMode::CONNECT_CALIBRE;
    } else if (selectedIndex == 2) {
      mode = NetworkMode::CREATE_HOTSPOT;
    } else if (selectedIndex == 3) {
      mode = NetworkMode::NEARBY_STATS_SYNC;
#if FREEINK_CAP_USB_MSC
    } else if (selectedIndex == 4) {
      mode = NetworkMode::USB_DRIVE;
#endif
    }
    onModeSelected(mode);
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safeArea = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true, /*hasSideButtonHints=*/false);

  GUI.drawHeader(renderer, Rect{safeArea.x, metrics.topPadding, safeArea.width, metrics.headerHeight}, tr(STR_FILE_TRANSFER));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = safeArea.y + safeArea.height - contentTop - metrics.verticalSpacing * 2;
  // Menu items and descriptions
  GUI.drawList(
      renderer, Rect{safeArea.x, contentTop, safeArea.width, contentHeight}, static_cast<int>(MENU_ITEM_COUNT),
      selectedIndex, [](int index) {
        switch (index) {
          case 0: return std::string(I18N.get(StrId::STR_JOIN_NETWORK));
          case 1: return std::string(I18N.get(StrId::STR_CALIBRE_WIRELESS));
          case 2: return std::string(I18N.get(StrId::STR_CREATE_HOTSPOT));
          case 3: return std::string(I18N.get(StrId::STR_NEARBY_STATS_SYNC));
#if FREEINK_CAP_USB_MSC
          case 4: return std::string("USB-накопитель");
#endif
          default: return std::string();
        }
      },
      [](int index) {
        switch (index) {
          case 0: return std::string(I18N.get(StrId::STR_JOIN_DESC));
          case 1: return std::string(I18N.get(StrId::STR_CALIBRE_DESC));
          case 2: return std::string(I18N.get(StrId::STR_HOTSPOT_DESC));
          case 3: return std::string(I18N.get(StrId::STR_NEARBY_STATS_SYNC_DESC));
#if FREEINK_CAP_USB_MSC
          case 4: return std::string("Показать SD-карту как USB-диск на компьютере");
#endif
          default: return std::string();
        }
      },
      [](int index) {
#if FREEINK_CAP_USB_MSC
        if (index == 4) return UIIcon::Transfer;
#endif
        static constexpr UIIcon icons[4] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot, UIIcon::Transfer};
        return icons[index < 4 ? index : 0];
      });

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
