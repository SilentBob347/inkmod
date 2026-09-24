#pragma once

#include <HalGPIO.h>

#include <array>

class GfxRenderer;
namespace freeink { namespace ui { enum class ScreenEdge : uint8_t; } }

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  enum class SwipeDir { None, Left, Right, Up, Down };
  static constexpr size_t BUTTON_COUNT = static_cast<size_t>(Button::PageForward) + 1;

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio, const GfxRenderer* renderer = nullptr) : gpio(gpio), renderer(renderer) {}

  // Enable/disable reader-specific front button mapping.
  // Call with true in reader activity onEnter(), false in onExit().
  //
  // Reader quick actions can replace the reader while the triggering physical
  // button is still held (for example long-press Join Network). Without
  // suppressing that outstanding release, the newly-entered activity may
  // consume it as its own Back/Confirm event and immediately cancel, wedge a
  // modal flow, or leave the screen looking unresponsive. Capture only buttons
  // that are actually down at the reader -> non-reader transition so ordinary
  // navigation remains unaffected.
  void setReaderMode(bool enabled) {
    if (readerMode && !enabled) {
      if (mapButton(Button::Back, &HalGPIO::isPressed)) {
        suppressBackRelease = true;
      }
      if (mapButton(Button::Confirm, &HalGPIO::isPressed)) {
        suppressConfirmRelease = true;
      }
      if (gpio.isPressed(HalGPIO::BTN_POWER)) {
        suppressPowerConfirmRelease = true;
      }
    }
    readerMode = enabled;
  }
  void setPowerAsConfirmInReaderMode(bool enabled) { powerAsConfirmInReaderMode = enabled; }

  void update() const { gpio.update(); }
  void suppressNextBackRelease() { suppressBackRelease = true; }
  void suppressNextConfirmRelease() { suppressConfirmRelease = true; }
  void suppressNextPowerConfirmRelease() { suppressPowerConfirmRelease = true; }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;
  // Returns the raw front button index that was released this frame (or -1 if none).
  int getReleasedFrontButton() const;
  bool isFrontButtonPressed(uint8_t buttonIndex) const;

  // X4 Pro touch bridge. Button-only X3/X4 paths are unchanged.
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  // Raw touch press edge, mapped to logical screen coordinates. Unlike
  // wasScreenTouchDown(), this fires immediately and does not require the
  // contact to remain inside tap slop. Intended for activity-owned drag
  // gestures such as the Home carousel.
  bool wasScreenTouchPressed(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  bool wasScreenTouchReleased() const;
  // Consume the current capacitive-touch contact/release so a tap used to open
  // a new activity is not seen again by that activity on its first frame.
  void suppressTouchContact() { gpio.suppressTouchContact(); }
  bool wasTapInRect(int x, int y, int width, int height) const;

  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Returns a completed swipe only when the gesture started inside the given
  // logical screen rectangle. Left-edge Back gestures stay reserved for Back.
  SwipeDir wasSwipeStartedInRect(int x, int y, int width, int height) const;
  // Returns a completed swipe that originated at the requested screen edge.
  // Exposed for reader-specific gestures such as bottom-edge menu invocation.
  bool wasEdgeSwipe(freeink::ui::ScreenEdge edge) const;
  bool wasBackGesture() const;
  bool hasHomeKey() const;
  bool wasHomeGesture() const;
  bool wasHomeKeyHold() const;
  bool wasLightPanelGesture() const;
  int getRendererWidth() const;
  int getRendererHeight() const;

#ifdef SIMULATOR
  void simulatorInjectPress(Button button);
  void simulatorInjectRelease(Button button);
  void simulatorClearInputFrame();
#endif

 private:
  HalGPIO& gpio;
  const GfxRenderer* renderer = nullptr;
  bool readerMode = false;
  bool powerAsConfirmInReaderMode = false;
  mutable bool suppressBackRelease = false;
  mutable bool suppressConfirmRelease = false;
  mutable bool suppressPowerConfirmRelease = false;
#ifdef SIMULATOR
  std::array<bool, BUTTON_COUNT> simulatorPressed{};
  std::array<bool, BUTTON_COUNT> simulatorReleased{};
  std::array<bool, BUTTON_COUNT> simulatorHeld{};
  std::array<unsigned long, BUTTON_COUNT> simulatorPressStart{};
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool softFrontButtonTapMatches(Button button) const;
  uint8_t mappedFrontHardwareButton(Button button) const;
  bool shouldUsePowerAsConfirmFallback() const;
  bool shouldMirrorPowerAsConfirmHold() const;
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
};
