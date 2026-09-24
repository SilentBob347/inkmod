#include "MappedInputManager.h"

#include <algorithm>
#include <BoardConfig.h>
#include <FreeInkUICore.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <utility>

#include "InkMODSettings.h"
#include "GlobalActions.h"

namespace {
using ButtonIndex = uint8_t;
constexpr ButtonIndex kNoButton = UINT8_MAX;

struct SideLayoutMap {
  ButtonIndex pageBackPrimary;
  ButtonIndex pageBackSecondary;
  ButtonIndex pageForwardPrimary;
  ButtonIndex pageForwardSecondary;
};

// Order matches InkMODSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, kNoButton, HalGPIO::BTN_DOWN, kNoButton},
    {HalGPIO::BTN_DOWN, kNoButton, HalGPIO::BTN_UP, kNoButton},
    {kNoButton, kNoButton, kNoButton, kNoButton},
    {kNoButton, kNoButton, HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
};

bool shouldSwapReaderSideButtons(const bool readerMode) {
  return readerMode && SETTINGS.sideButtonOrientationAware && SETTINGS.orientation != InkMODSettings::PORTRAIT;
}

bool shouldSwapReaderFrontNavButtons(const InkMODSettings::FRONT_BUTTON_ORIENTATION_AWARE orientationMode) {
  if (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_OFF) {
    return false;
  }
  return SETTINGS.orientation == InkMODSettings::LANDSCAPE_CW ||
         SETTINGS.orientation == InkMODSettings::LANDSCAPE_CCW ||
         (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_NAV_BUTTONS &&
          SETTINGS.orientation == InkMODSettings::INVERTED);
}

ButtonIndex invertFrontButtonPosition(const ButtonIndex button) {
  switch (button) {
    case HalGPIO::BTN_BACK:
      return HalGPIO::BTN_RIGHT;
    case HalGPIO::BTN_CONFIRM:
      return HalGPIO::BTN_LEFT;
    case HalGPIO::BTN_LEFT:
      return HalGPIO::BTN_CONFIRM;
    case HalGPIO::BTN_RIGHT:
      return HalGPIO::BTN_BACK;
    default:
      return button;
  }
}

ButtonIndex mapFrontButtonForReaderOrientation(const ButtonIndex button, const ButtonIndex leftButton,
                                               const ButtonIndex rightButton, const bool readerMode) {
  if (!readerMode) {
    return button;
  }

  const auto orientationMode =
      static_cast<InkMODSettings::FRONT_BUTTON_ORIENTATION_AWARE>(SETTINGS.frontButtonOrientationAware);

  if (orientationMode == InkMODSettings::FRONT_ORIENTATION_AWARE_ALL_BUTTONS &&
      SETTINGS.orientation == InkMODSettings::INVERTED) {
    return invertFrontButtonPosition(button);
  }

  if (shouldSwapReaderFrontNavButtons(orientationMode)) {
    if (button == leftButton) {
      return rightButton;
    }
    if (button == rightButton) {
      return leftButton;
    }
  }

  return button;
}

SideLayoutMap mapSideLayoutForReaderOrientation(SideLayoutMap side, const bool readerMode) {
  if (shouldSwapReaderSideButtons(readerMode)) {
    const bool hasPageBack = side.pageBackPrimary != kNoButton || side.pageBackSecondary != kNoButton;
    const bool hasPageForward = side.pageForwardPrimary != kNoButton || side.pageForwardSecondary != kNoButton;
    if (hasPageBack && hasPageForward) {
      std::swap(side.pageBackPrimary, side.pageForwardPrimary);
      std::swap(side.pageBackSecondary, side.pageForwardSecondary);
    }
  }
  return side;
}

ButtonIndex mapSideButtonForReaderOrientation(const ButtonIndex button, const bool readerMode) {
  if (!shouldSwapReaderSideButtons(readerMode)) {
    return button;
  }
  if (button == HalGPIO::BTN_UP) {
    return HalGPIO::BTN_DOWN;
  }
  if (button == HalGPIO::BTN_DOWN) {
    return HalGPIO::BTN_UP;
  }
  return button;
}

bool readMappedSideButtons(const HalGPIO& gpio, bool (HalGPIO::*fn)(uint8_t) const, const ButtonIndex primary,
                           const ButtonIndex secondary) {
  return (primary != kNoButton && (gpio.*fn)(primary)) || (secondary != kNoButton && (gpio.*fn)(secondary));
}

#ifdef SIMULATOR
size_t buttonIndex(MappedInputManager::Button button) { return static_cast<size_t>(button); }
#endif

}  // namespace

namespace fui = freeink::ui;

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!renderer) return false;
  float nx=0, ny=0;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchPressed(int& x, int& y) const {
  if (!renderer) return false;
  float nx = 0, ny = 0;
  if (!gpio.wasTouchDown(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!renderer) return false;
  float nx=0, ny=0; unsigned long held=0;
  if (!gpio.isTouchTapCandidate(nx, ny, held) || held < 90) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  if (!renderer) return false;
  float nx=0, ny=0; if (!gpio.wasTouchLongPress(nx, ny)) return false;
  gpio.suppressTouchContact(); renderer->tapToLogical(nx, ny, x, y); return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!renderer) return false;
  float nx=0, ny=0; if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y); return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

bool MappedInputManager::wasTapInRect(int x, int y, int width, int height) const {
  int tx=0, ty=0; return wasScreenTapped(tx,ty) && tx>=x && tx<x+width && ty>=y && ty<y+height;
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r < 0 || r >= rowCount) return false;
    if (!gpio.hasTouch() && rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0, y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c < 0 || c >= colCount) return false;
    if (!gpio.hasTouch() && colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0, y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx,int& sy,int& ex,int& ey) const {
  if (!renderer) return false; float nxs=0,nys=0,nxe=0,nye=0;
  if (!gpio.wasSwipe(nxs,nys,nxe,nye)) return false;
  renderer->tapToLogical(nxs,nys,sx,sy); renderer->tapToLogical(nxe,nye,ex,ey); return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;

  // The left-edge swipe is the global Back gesture on X4 Pro.  Do not also
  // expose the same contact as an ordinary horizontal navigation swipe: many
  // activities process Left/Right before Back and would otherwise navigate and
  // return early, making Back appear broken.
  if (fui::edgeSwipe(fui::ScreenEdge::Left, sx, sy, ex, ey,
                     renderer->getScreenWidth(), renderer->getScreenHeight(), 0.05f)) {
    return SwipeDir::None;
  }

  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left: return SwipeDir::Left;
    case fui::SwipeDir::Right: return SwipeDir::Right;
    case fui::SwipeDir::Up: return SwipeDir::Up;
    case fui::SwipeDir::Down: return SwipeDir::Down;
    default: return SwipeDir::None;
  }
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipeStartedInRect(const int x, const int y, const int width,
                                                                       const int height) const {
  if (!renderer || width <= 0 || height <= 0) return SwipeDir::None;

  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;

  // Keep the global Back edge gesture exclusive even when its start point also
  // lies inside the caller's rectangle (e.g. a full-width Home carousel).
  if (fui::edgeSwipe(fui::ScreenEdge::Left, sx, sy, ex, ey,
                     renderer->getScreenWidth(), renderer->getScreenHeight(), 0.05f)) {
    return SwipeDir::None;
  }

  if (sx < x || sx >= x + width || sy < y || sy >= y + height) return SwipeDir::None;

  switch (fui::swipeDirection(sx, sy, ex, ey)) {
    case fui::SwipeDir::Left: return SwipeDir::Left;
    case fui::SwipeDir::Right: return SwipeDir::Right;
    case fui::SwipeDir::Up: return SwipeDir::Up;
    case fui::SwipeDir::Down: return SwipeDir::Down;
    default: return SwipeDir::None;
  }
}

bool MappedInputManager::wasEdgeSwipe(const freeink::ui::ScreenEdge edge) const {
  if (!renderer) return false; int sx=0,sy=0,ex=0,ey=0; if(!decodeSwipe(sx,sy,ex,ey)) return false;
  // Back must begin at the physical edge. The SDK's generic 25% side-edge band
  // is intentionally generous for thumb gestures, but in the reader it steals
  // ordinary right-swipes used for the previous page. Keep only the left edge
  // strict; top/bottom gestures retain the SDK defaults.
  const float edgeFrac = edge == fui::ScreenEdge::Left ? 0.05f : -1.0f;
  return fui::edgeSwipe(edge,sx,sy,ex,ey,renderer->getScreenWidth(),renderer->getScreenHeight(),edgeFrac);
}

bool MappedInputManager::wasBackGesture() const { return wasEdgeSwipe(fui::ScreenEdge::Left); }
bool MappedInputManager::hasHomeKey() const { return gpio.hasHomeKey(); }
bool MappedInputManager::wasHomeGesture() const { return gpio.hasHomeKey() ? gpio.wasHomeKeyTapped() : wasEdgeSwipe(fui::ScreenEdge::Bottom); }
bool MappedInputManager::wasHomeKeyHold() const { return gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed(); }
bool MappedInputManager::wasLightPanelGesture() const { return Frontlight.present() && wasEdgeSwipe(fui::ScreenEdge::Top); }
int MappedInputManager::getRendererWidth() const { return renderer ? renderer->getScreenWidth() : 0; }
int MappedInputManager::getRendererHeight() const { return renderer ? renderer->getScreenHeight() : 0; }


uint8_t MappedInputManager::mappedFrontHardwareButton(const Button button) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;

  ButtonIndex physical = kNoButton;
  switch (button) {
    case Button::Back:
      physical = btnBack;
      break;
    case Button::Confirm:
      physical = btnConfirm;
      break;
    case Button::Left:
      physical = btnLeft;
      break;
    case Button::Right:
      physical = btnRight;
      break;
    default:
      return kNoButton;
  }
  return mapFrontButtonForReaderOrientation(physical, btnLeft, btnRight, readerMode);
}

bool MappedInputManager::softFrontButtonTapMatches(const Button button) const {
  // X4 Pro: the four button hints are drawn in portrait coordinates along the
  // bottom edge. Make those existing on-screen hints real touch buttons.
  // Use raw touch coordinates and convert to the portrait frame so the hit
  // zones keep working even when the reader itself is rotated.
  if (!hasTouch() || !renderer) return false;

  const uint8_t expected = mappedFrontHardwareButton(button);
  if (expected == kNoButton || expected > HalGPIO::BTN_RIGHT) return false;

  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;

  // X4/X4 Pro panel is physically 800x480; portrait logical coordinates are
  // 480x800. drawButtonHints() uses X4 positions {11,128,246,363}, 106x40.
  constexpr int kPanelWidth = 800;
  constexpr int kPanelHeight = 480;
  constexpr int kPortraitHeight = 800;
  constexpr int kButtonWidth = 106;
  constexpr int kButtonHeight = 40;
  constexpr int kButtonX[4] = {11, 128, 246, 363};

  int phyX = static_cast<int>(nx * kPanelWidth);
  int phyY = static_cast<int>(ny * kPanelHeight);
  phyX = std::max(0, std::min(kPanelWidth - 1, phyX));
  phyY = std::max(0, std::min(kPanelHeight - 1, phyY));
  const int x = kPanelHeight - 1 - phyY;
  const int y = phyX;

  if (y < kPortraitHeight - kButtonHeight) return false;

  int slot = -1;
  for (int i = 0; i < 4; ++i) {
    if (x >= kButtonX[i] && x < kButtonX[i] + kButtonWidth) {
      slot = i;
      break;
    }
  }
  if (slot < 0 || static_cast<uint8_t>(slot) != expected) return false;

  // The touch backend already suppresses this completed contact for the rest
  // of the current frame/contact. Do not keep a separate persistent latch: most
  // activities rely on the global GPIO update and do not call mappedInput.update().
  gpio.suppressTouchContact();
  return true;
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  // Avoid rebuilding both front- and side-button mappings for every query.
  // Activities may ask several button states each main-loop pass, so only
  // compute the mapping group required by the requested logical button.
  switch (button) {
    case Button::Power:
      return (gpio.*fn)(HalGPIO::BTN_POWER);

    case Button::Up:
      return (gpio.*fn)(mapSideButtonForReaderOrientation(HalGPIO::BTN_UP, readerMode));
    case Button::Down:
      return (gpio.*fn)(mapSideButtonForReaderOrientation(HalGPIO::BTN_DOWN, readerMode));

    case Button::PageBack:
    case Button::PageForward: {
      const auto sideLayout = static_cast<InkMODSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
      const auto side = mapSideLayoutForReaderOrientation(kSideLayouts[sideLayout], readerMode);
      if (button == Button::PageBack) {
        return readMappedSideButtons(gpio, fn, side.pageBackPrimary, side.pageBackSecondary);
      }
      return readMappedSideButtons(gpio, fn, side.pageForwardPrimary, side.pageForwardSecondary);
    }

    case Button::Back:
    case Button::Confirm:
    case Button::Left:
    case Button::Right: {
      const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
      const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
      const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
      const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
      const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;

      ButtonIndex physical = btnBack;
      switch (button) {
        case Button::Confirm:
          physical = btnConfirm;
          break;
        case Button::Left:
          physical = btnLeft;
          break;
        case Button::Right:
          physical = btnRight;
          break;
        case Button::Back:
        default:
          break;
      }
      return (gpio.*fn)(mapFrontButtonForReaderOrientation(physical, btnLeft, btnRight, readerMode));
    }
  }

  return false;
}

bool MappedInputManager::shouldUsePowerAsConfirmFallback() const { return !readerMode || powerAsConfirmInReaderMode; }

bool MappedInputManager::shouldMirrorPowerAsConfirmHold() const {
  return shouldUsePowerAsConfirmFallback() &&
         !isPowerButtonActionAvailableOutsideReader(static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
}

bool MappedInputManager::wasPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorPressed[buttonIndex(button)]) {
    return true;
  }
#endif

  if (softFrontButtonTapMatches(button)) {
    return true;
  }

  // CrossPoint global Back gesture: it must work in reader sub-activities too
  // (footnotes, TOC, bookmarks, dictionary, etc.), not only outside reader mode.
  if (button == Button::Back && hasTouch() && wasBackGesture()) {
    return true;
  }

  // Touch swipes are intentionally NOT translated into logical navigation
  // buttons here. On X4 Pro taps activate visible controls directly, while
  // activities with long lists/files consume wasSwipe() themselves for page
  // scrolling. Global swipe->Left/Right/Up/Down emulation made Home themes
  // move the selection instead of activating the item that was actually tapped.

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasPressed)) {
      return true;
    }

    return shouldUsePowerAsConfirmFallback() &&
           !isPowerButtonActionAvailableOutsideReader(
               static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) &&
           gpio.wasPressed(HalGPIO::BTN_POWER);
  }

  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
#ifdef SIMULATOR
  if (simulatorReleased[buttonIndex(button)]) {
    return true;
  }
#endif

  // Do not translate touch swipes into button releases either. Explicit
  // scrolling activities handle swipes themselves; ordinary menus are tap-first.

  if (softFrontButtonTapMatches(button)) {
    return true;
  }

  if (button == Button::Back) {
    // Left-edge swipe is Back on touch devices. Let each activity handle the
    // synthetic Back through its existing code path so cancel/results stay correct.
    if (hasTouch() && wasBackGesture()) {
      return true;
    }
    if (!mapButton(button, &HalGPIO::wasReleased)) {
      return false;
    }

    if (suppressBackRelease) {
      suppressBackRelease = false;
      return false;
    }

    return true;
  }

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::wasReleased)) {
      if (suppressConfirmRelease) {
        suppressConfirmRelease = false;
        return false;
      }
      return true;
    }

    if (!shouldUsePowerAsConfirmFallback() || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
      return false;
    }

    if (suppressConfirmRelease) {
      suppressConfirmRelease = false;
      suppressPowerConfirmRelease = false;
      return false;
    }

    if (suppressPowerConfirmRelease) {
      suppressPowerConfirmRelease = false;
      return false;
    }

    const bool longPress = gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    const auto action = longPress ? static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn)
                                  : static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn);
    return !isPowerButtonActionAvailableOutsideReader(action);
  }

  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const {
#ifdef SIMULATOR
  if (simulatorHeld[buttonIndex(button)]) {
    return true;
  }
#endif

  if (button == Button::Confirm) {
    if (mapButton(button, &HalGPIO::isPressed)) {
      return true;
    }

    if (!shouldMirrorPowerAsConfirmHold() || !gpio.isPressed(HalGPIO::BTN_POWER)) {
      return false;
    }

    return !isPowerButtonActionAvailableOutsideReader(
               static_cast<InkMODSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)) ||
           gpio.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  }

  return mapButton(button, &HalGPIO::isPressed);
}

bool MappedInputManager::wasAnyPressed() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorPressed.begin(), simulatorPressed.end(), [](bool pressed) { return pressed; })) {
    return true;
  }
#endif
  return gpio.wasAnyPressed();
}

bool MappedInputManager::wasAnyReleased() const {
#ifdef SIMULATOR
  if (std::any_of(simulatorReleased.begin(), simulatorReleased.end(), [](bool released) { return released; })) {
    return true;
  }
#endif
  return gpio.wasAnyReleased();
}

unsigned long MappedInputManager::getHeldTime() const {
  unsigned long heldTime = gpio.getHeldTime();
#ifdef SIMULATOR
  const unsigned long now = millis();
  for (size_t i = 0; i < BUTTON_COUNT; i++) {
    if (simulatorHeld[i] && simulatorPressStart[i] > 0) {
      heldTime = std::max(heldTime, now - simulatorPressStart[i]);
    }
  }
#endif
  return heldTime;
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  const bool useReaderMapping = readerMode && SETTINGS.readerFrontButtonsEnabled;
  const ButtonIndex btnBack = useReaderMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
  const ButtonIndex btnConfirm = useReaderMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
  const ButtonIndex btnLeft = useReaderMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
  const ButtonIndex btnRight = useReaderMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
  const ButtonIndex mappedBack = mapFrontButtonForReaderOrientation(btnBack, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedConfirm = mapFrontButtonForReaderOrientation(btnConfirm, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedLeft = mapFrontButtonForReaderOrientation(btnLeft, btnLeft, btnRight, readerMode);
  const ButtonIndex mappedRight = mapFrontButtonForReaderOrientation(btnRight, btnLeft, btnRight, readerMode);

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](ButtonIndex hw) -> const char* {
    if (hw == mappedBack) return back;
    if (hw == mappedConfirm) return confirm;
    if (hw == mappedLeft) return previous;
    if (hw == mappedRight) return next;
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

int MappedInputManager::getReleasedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping for screens whose labels are fixed to physical slots.
  if (gpio.wasReleased(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

bool MappedInputManager::isFrontButtonPressed(const uint8_t buttonIndex) const { return gpio.isPressed(buttonIndex); }

#ifdef SIMULATOR
void MappedInputManager::simulatorInjectPress(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorPressed[idx] = true;
  simulatorReleased[idx] = false;
  simulatorHeld[idx] = true;
  simulatorPressStart[idx] = millis();
}

void MappedInputManager::simulatorInjectRelease(Button button) {
  const size_t idx = buttonIndex(button);
  simulatorPressed[idx] = false;
  simulatorReleased[idx] = true;
  simulatorHeld[idx] = false;
}

void MappedInputManager::simulatorClearInputFrame() {
  simulatorPressed.fill(false);
  simulatorReleased.fill(false);
}
#endif
