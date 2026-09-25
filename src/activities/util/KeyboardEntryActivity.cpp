#include "KeyboardEntryActivity.h"

#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "UiTextSize.h"
#include "components/UITheme.h"
#include "fontIds.h"

const char* const KeyboardEntryActivity::shiftString[2] = {"shift", "SHIFT"};

namespace {

bool isUtf8Continuation(const char value) {
  return (static_cast<uint8_t>(value) & 0xC0U) == 0x80U;
}

// Keyboard cursor positions are byte indexes because std::string stores UTF-8.
// These helpers ensure editing actions still operate on complete characters.
size_t previousUtf8Boundary(const std::string& text, size_t position) {
  if (position == 0) return 0;
  --position;
  while (position > 0 && isUtf8Continuation(text[position])) --position;
  return position;
}

size_t nextUtf8Boundary(const std::string& text, size_t position) {
  if (position >= text.size()) return text.size();
  ++position;
  while (position < text.size() && isUtf8Continuation(text[position])) ++position;
  return position;
}

// These UTF-8 labels are static flash data. The keyboard never copies a
// layout to heap RAM; it inserts only the selected one- or two-byte glyph.
constexpr const char* kRussianLower[3][10] = {
    {"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з"},
    {"ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж"},
    {"я", "ч", "с", "м", "и", "т", "ь", "б", "ю", "х"},
};
constexpr const char* kRussianUpper[3][10] = {
    {"Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З"},
    {"Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж"},
    {"Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", "Х"},
};
constexpr const char* kUkrainianLower[3][10] = {
    {"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з"},
    {"ф", "і", "в", "а", "п", "р", "о", "л", "д", "ж"},
    {"я", "ч", "с", "м", "и", "т", "ь", "б", "ю", "х"},
};
constexpr const char* kUkrainianUpper[3][10] = {
    {"Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З"},
    {"Ф", "І", "В", "А", "П", "Р", "О", "Л", "Д", "Ж"},
    {"Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", "Х"},
};
// A 4x10 keyboard reserves its first row for digits, leaving 30 letter cells.
// Russian and Ukrainian each need 33 letters, so the three least frequently
// used letters live as long-press alternates on 1/2/3 (still visible/usable
// through the symbol layer for punctuation). This keeps every alphabet letter
// available without shrinking the X4 Pro touch targets.
constexpr const char* kRussianExtraLower[3] = {"ё", "ъ", "э"};
constexpr const char* kRussianExtraUpper[3] = {"Ё", "Ъ", "Э"};
constexpr const char* kUkrainianExtraLower[3] = {"є", "ї", "ґ"};
constexpr const char* kUkrainianExtraUpper[3] = {"Є", "Ї", "Ґ"};

}  // namespace

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  symMode = false;
  urlMode = false;
  cursorMode = false;
  togglePos = false;
  passwordVisible = false;
  shiftState = 0;
  switch (I18N.getLanguage()) {
    case ::Language::RU:
      language = Language::Russian;
      break;
    case ::Language::UK:
      language = Language::Ukrainian;
      break;
    default:
      language = Language::English;
      break;
  }
  selectedRow = 0;
  selectedCol = 0;
  delPressCount = 0;
  hintVisible = false;
  hintShowTime = 0;
  rightHeld = false;
  rightLongHandled = false;
  savedCursorPos = 0;
  rightStartCursorPos = 0;
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getContentRowCount() const {
  if (urlMode) return 3;
  return ABC_ROWS;
}

int KeyboardEntryActivity::getContentColCount() const {
  if (urlMode) return 3;
  return COLS;
}

int KeyboardEntryActivity::getTotalRowCount() const { return getContentRowCount() + 1; }

bool KeyboardEntryActivity::isBottomRow(const int row) const { return row == getContentRowCount(); }

const char* KeyboardEntryActivity::keyLabel(const int row, const int col, const bool secondary,
                                            char (&asciiBuf)[2]) const {
  if (!symMode && inputType != InputType::Url) {
    // Cyrillic layouts have 33 letters while the three alphabet rows provide
    // 30 cells. Keep the three extra letters on the digit row, but make them
    // visible and directly typeable: in normal mode they are shown as the
    // secondary label on 1/2/3 (and remain available by long press); with
    // SHIFT enabled they become the primary tap action.
    if (row == 0 && col >= 0 && col < 3 && secondary) {
      if (language == Language::Russian) return kRussianExtraUpper[col];
      if (language == Language::Ukrainian) return kUkrainianExtraUpper[col];
    }
    if (row >= 1 && row <= 3 && col >= 0 && col < COLS) {
      const int languageRow = row - 1;
      if (language == Language::Russian) return (secondary ? kRussianUpper : kRussianLower)[languageRow][col];
      if (language == Language::Ukrainian) return (secondary ? kUkrainianUpper : kUkrainianLower)[languageRow][col];
    }
  }

  const KeyDef& key = (symMode ? symLayout : (inputType == InputType::Url ? urlLayout : abcLayout))[row][col];
  asciiBuf[0] = secondary && key.secondary != '\0' ? key.secondary : key.primary;
  asciiBuf[1] = '\0';
  return asciiBuf;
}

const char* KeyboardEntryActivity::getSelectedText() {
  if (selectedRow < 0 || selectedRow >= getContentRowCount() || selectedCol < 0 || selectedCol >= COLS) return "";
  return keyLabel(selectedRow, selectedCol, !symMode && shiftState > 0, selectedAsciiKey);
}

const char* KeyboardEntryActivity::getAlternativeText() {
  if (symMode || urlMode || inputType == InputType::Url || selectedRow < 0 || selectedRow >= getContentRowCount() ||
      selectedCol < 0 || selectedCol >= COLS) {
    return "";
  }
  if (selectedRow == 0 && selectedCol >= 0 && selectedCol < 3) {
    const bool upper = shiftState > 0;
    if (language == Language::Russian) return (upper ? kRussianExtraUpper : kRussianExtraLower)[selectedCol];
    if (language == Language::Ukrainian) return (upper ? kUkrainianExtraUpper : kUkrainianExtraLower)[selectedCol];
  }
  return keyLabel(selectedRow, selectedCol, shiftState == 0, selectedAsciiKey);
}

const char* KeyboardEntryActivity::languageModeLabel() const {
  switch (language) {
    case Language::Russian:
      return "RU #";
    case Language::Ukrainian:
      return "UK #";
    default:
      return "EN #";
  }
}

void KeyboardEntryActivity::cycleLanguage() {
  language = language == Language::English ? Language::Russian
             : language == Language::Russian ? Language::Ukrainian
                                              : Language::English;
}

bool KeyboardEntryActivity::insertChar(char c) {
  if (c == '\0') return true;
  if (maxLength != 0 && text.length() >= maxLength) return true;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, 1, c);
  cursorPos++;
  return true;
}

void KeyboardEntryActivity::insertString(const std::string& str) {
  if (str.empty()) return;
  if (maxLength != 0 && text.length() + str.length() > maxLength) return;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, str);
  cursorPos += str.length();
}

bool KeyboardEntryActivity::handleKeyPress() {
  if (isBottomRow(selectedRow)) {
    switch (static_cast<SpecialKeyType>(selectedCol)) {
      case SpecialKeyType::Shift:
        delPressCount = 0;
        hintVisible = false;
        if (urlMode || inputType == InputType::Url) return true;
        if (symMode) return true;
        shiftState = (shiftState + 1) % 2;
        return true;
      case SpecialKeyType::Mode: {
        delPressCount = 0;
        hintVisible = false;
        if (urlMode) {
          urlMode = false;
          symMode = false;
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Mode);
          requestUpdate();
          return true;
        }
        symMode = !symMode;
        int maxRow = getTotalRowCount() - 1;
        if (selectedRow > maxRow) selectedRow = maxRow;
        if (isBottomRow(selectedRow)) {
          if (selectedCol >= BOTTOM_KEY_COUNT) selectedCol = BOTTOM_KEY_COUNT - 1;
        } else {
          if (selectedCol >= getContentColCount()) selectedCol = getContentColCount() - 1;
        }
        return true;
      }
      case SpecialKeyType::Space:
        delPressCount = 0;
        hintVisible = false;
        if (inputType == InputType::Url) {
          urlMode = !urlMode;
          if (urlMode) {
            symMode = false;
          }
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Space);
          requestUpdate();
        } else {
          return insertChar(' ');
        }
        return true;
      case SpecialKeyType::Del:
        delPressCount++;
        if (delPressCount >= 2) {
          hintVisible = true;
          hintShowTime = millis();
        }
        if (cursorPos > 0 && !text.empty()) {
          const size_t eraseStart = previousUtf8Boundary(text, cursorPos);
          text.erase(eraseStart, cursorPos - eraseStart);
          cursorPos = eraseStart;
        }
        return true;
      case SpecialKeyType::Ok:
        delPressCount = 0;
        hintVisible = false;
        if (text.length() < minLength) return true;
        onComplete(text);
        return false;
      default:
        return true;
    }
  }

  if (urlMode) {
    delPressCount = 0;
    hintVisible = false;
    const int idx = selectedCol + selectedRow * 3;
    if (idx < URL_SNIPPET_COUNT) {
      insertString(urlSnippets[idx]);
    }
    return true;
  }

  delPressCount = 0;
  hintVisible = false;

  insertString(getSelectedText());
  return true;
}

void KeyboardEntryActivity::mapColContentBottom(int& col, bool goingUp) const {
  if (urlMode) {
    col = goingUp ? col - 1 : col + 1;
    if (col < 0) col = 0;
    if (col >= 3) col = 2;
  } else {
    col = goingUp ? col * 2 : col / 2;
  }
}

bool KeyboardEntryActivity::findTouchKey(const int x, const int y, int& row, int& col) const {
  const uint8_t table = activeTouchTable.load(std::memory_order_acquire);
  const uint8_t count = touchKeyCounts[table];
  for (uint8_t i = 0; i < count; ++i) {
    const auto& hit = touchKeys[table][i];
    if (x >= hit.rect.x && y >= hit.rect.y && x < hit.rect.x + hit.rect.width && y < hit.rect.y + hit.rect.height) {
      row = hit.row;
      col = hit.col;
      return true;
    }
  }
  return false;
}

bool KeyboardEntryActivity::activateTouchKey(const int row, const int col, const bool longPress) {
  // row == -2 is the password visibility chip next to the text field.
  if (row == -2) {
    if (inputType == InputType::Password) {
      passwordVisible = !passwordVisible;
      return true;
    }
    return false;
  }

  if (row < 0 || row >= getTotalRowCount()) return false;
  const int maxCol = isBottomRow(row) ? BOTTOM_KEY_COUNT : getContentColCount();
  if (col < 0 || col >= maxCol) return false;

  selectedRow = row;
  selectedCol = col;
  cursorMode = false;
  togglePos = false;
  hintVisible = false;

  if (longPress) {
    if (isBottomRow(selectedRow) && selectedCol == static_cast<int>(SpecialKeyType::Del)) {
      text.clear();
      cursorPos = 0;
      return true;
    }
    if (isBottomRow(selectedRow) && selectedCol == static_cast<int>(SpecialKeyType::Mode) && !symMode && !urlMode &&
        inputType != InputType::Url) {
      cycleLanguage();
      return true;
    }
    const char* alt = getAlternativeText();
    if (alt[0] != '\0') {
      insertString(alt);
      return true;
    }
    // No alternate action: fall back to a normal activation, as the
    // CrossPoint touch keyboard does for keys without a long-press variant.
  }

  return handleKeyPress();
}

void KeyboardEntryActivity::loop() {
  const int totalRows = getTotalRowCount();

  // Touch keyboard. MappedInputManager already converts the X4 Pro panel
  // coordinates into the same logical coordinates used for rendering.
  int touchX = 0;
  int touchY = 0;
  int touchRow = -1;
  int touchCol = -1;
  if (mappedInput.wasScreenLongPress(touchX, touchY) && findTouchKey(touchX, touchY, touchRow, touchCol)) {
    // Keep the keyboard touch event local. In particular, the Backspace key
    // sits close to the on-screen navigation hints on X4 Pro; without consuming
    // the contact, the same release can be observed by the generic button
    // bridge and move the keyboard selection.
    const bool handled = activateTouchKey(touchRow, touchCol, true);
    mappedInput.suppressTouchContact();
    if (handled) requestUpdate();
    return;
  }
  if (mappedInput.wasScreenTapped(touchX, touchY) && findTouchKey(touchX, touchY, touchRow, touchCol)) {
    const bool handled = activateTouchKey(touchRow, touchCol, false);
    mappedInput.suppressTouchContact();
    if (handled) requestUpdate();
    return;
  }

  if (!cursorMode && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    upHeld = true;
    upLongHandled = false;
  }

  if (upHeld && !upLongHandled && mappedInput.isPressed(MappedInputManager::Button::Up) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    cursorMode = true;
    upLongHandled = true;
    hintVisible = true;
    hintShowTime = millis();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::previousIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downHeld = true;
    if (cursorMode) {
      togglePos = false;
      passwordVisible = false;
      cursorMode = false;
      hintVisible = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::nextIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorMode) {
      if (togglePos) {
        cursorPos = savedCursorPos;
        togglePos = false;
        requestUpdate();
      } else if (cursorPos > 0) {
        cursorPos = previousUtf8Boundary(text, cursorPos);
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      rightHeld = true;
      rightLongHandled = false;
      rightStartCursorPos = cursorPos;
    }
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (rightHeld && !rightLongHandled && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      savedCursorPos = rightStartCursorPos;
      togglePos = true;
      rightLongHandled = true;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password) {
      rightHeld = false;
      rightLongHandled = false;
    }
    if (cursorMode && !togglePos && cursorPos < text.length()) {
      cursorPos = nextUtf8Boundary(text, cursorPos);
      requestUpdate();
    }
    if (cursorMode) return;
    rightHeld = false;
    rightLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && isBottomRow(selectedRow) &&
      selectedCol == static_cast<int>(SpecialKeyType::Del)) {
    text.clear();
    cursorPos = 0;
    confirmLongHandled = true;
    requestUpdate();
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (isBottomRow(selectedRow) && selectedCol == static_cast<int>(SpecialKeyType::Mode) && !symMode &&
        !urlMode && inputType != InputType::Url) {
      cycleLanguage();
      confirmLongHandled = true;
      requestUpdate();
    } else {
      const char* alt = getAlternativeText();
      if (alt[0] == '\0') return;
      insertString(alt);
      requestUpdate();
      confirmLongHandled = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled && !cursorMode) {
      if (handleKeyPress()) {
        requestUpdate();
      }
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password && togglePos) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
  }

  if (hintVisible && !cursorMode && millis() - hintShowTime > 4000) {
    hintVisible = false;
    requestUpdate();
  }
}

void KeyboardEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int controlFontId = uiControlFontId();
  const int hintFontId = uiHintFontId();
  Rect passwordToggleTouchRect{};
  bool hasPasswordToggleTouch = false;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(controlFontId);
  const int inputStartY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                          metrics.verticalSpacing * 4 + metrics.keyboardVerticalOffset;
  int inputHeight = 0;

  std::string displayText;
  if (inputType == InputType::Password && !passwordVisible) {
    size_t revealPos;
    if (cursorMode) {
      revealPos = text.length();  // no reveal in displayText; block draws actual char directly
    } else {
      revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : std::string::npos;
    }
    displayText = text;
    for (size_t i = 0; i < displayText.length(); i++) {
      if (i != revealPos) {
        displayText[i] = '*';
      }
    }
  } else {
    displayText = text;
  }

  const bool isPassword = (inputType == InputType::Password);
  // The X4 puts both page buttons on the right edge; X3 has one on
  // each edge. Keep an extra gutter as the themed outlines themselves
  // reach the edge of the button column.
  constexpr int sideHintClearance = 8;
  const int sideHintReservation =
      gpio.deviceIsX3() ? 2 * (metrics.sideButtonHintsWidth + sideHintClearance)
                         : metrics.sideButtonHintsWidth + sideHintClearance;
  const int availableWidth = std::max(0, pageWidth - sideHintReservation);
  const int effectiveMargin = (pageWidth - availableWidth * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = isPassword ? 4 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(controlFontId, "[abc]"),
                                                  renderer.getTextWidth(controlFontId, "[***]")) +
                                             toggleGap
                                       : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;

  int cursorCharWidth = 6;
  if (cursorPos < text.length()) {
    int w = renderer.getTextWidth(controlFontId, text.substr(cursorPos, 1).c_str());
    if (w > cursorCharWidth) cursorCharWidth = w;
  }

  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  int cursorPixelX = effectiveMargin;
  int cursorLineY = inputStartY;
  bool cursorDrawn = false;

  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextAdvanceX(controlFontId, lineText.c_str(), EpdFontFamily::REGULAR);
    if (textWidth <= maxLineWidth) {
      const bool isLastLine = (lineEndIdx == static_cast<int>(displayText.length()));
      bool isCursorLine = false;
      if (!cursorDrawn && cursorPos >= lineStartIdx &&
          (isLastLine ? cursorPos <= lineEndIdx : cursorPos < lineEndIdx)) {
        std::string beforeCursor;
        if (isPassword && !passwordVisible && cursorMode) {
          beforeCursor = std::string(cursorPos - lineStartIdx, '*');
        } else {
          beforeCursor = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        }
        int beforeWidth = renderer.getTextAdvanceX(controlFontId, beforeCursor.c_str(), EpdFontFamily::REGULAR);
        int kernOffset = 0;
        if (cursorPos < displayText.length()) {
          std::string beforeAndCursor = beforeCursor + displayText.substr(cursorPos, 1);
          int beforeAndCursorWidth =
              renderer.getTextAdvanceX(controlFontId, beforeAndCursor.c_str(), EpdFontFamily::REGULAR);
          int charAdvance =
              renderer.getTextAdvanceX(controlFontId, displayText.substr(cursorPos, 1).c_str(), EpdFontFamily::REGULAR);
          kernOffset = beforeAndCursorWidth - beforeWidth - charAdvance;
        }
        if (centerText) {
          cursorPixelX = effectiveMargin + (maxLineWidth - textWidth) / 2 + beforeWidth + kernOffset;
        } else {
          cursorPixelX = effectiveMargin + beforeWidth + kernOffset;
        }
        cursorLineY = inputStartY + inputHeight;
        cursorDrawn = true;
        isCursorLine = true;
      }

      const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin;
      if (isCursorLine && cursorMode && isPassword && !passwordVisible && !togglePos) {
        // Draw text in 3 parts to avoid block cursor overflowing onto next char.
        // displayText uses '*' for all chars; actual char may be wider than '*'.
        // Part 1: chars before cursor position
        const std::string part1 = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        renderer.drawText(controlFontId, lineStartX, inputStartY + inputHeight, part1.c_str());
        // Part 2: skip cursor slot (block + actual char drawn later)
        // Part 3: chars after cursor position (skip char under cursor), starting at cursorPixelX + cursorCharWidth
        const int afterStart = static_cast<int>(cursorPos) + (cursorPos < text.length() ? 1 : 0);
        const int afterEnd = lineEndIdx;
        if (afterStart < afterEnd) {
          const std::string part3 = displayText.substr(afterStart, afterEnd - afterStart);
          renderer.drawText(controlFontId, cursorPixelX + cursorCharWidth, inputStartY + inputHeight, part3.c_str());
        }
      } else {
        renderer.drawText(controlFontId, lineStartX, inputStartY + inputHeight, lineText.c_str());
      }
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputHeight += lineHeight;
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }

  const int fieldWidth = (inputHeight > 0) ? maxLineWidth : textWidth;
  const int lineMargin = effectiveMargin;
  GUI.drawTextField(renderer, Rect{0, inputStartY, pageWidth, inputHeight}, fieldWidth, cursorMode, lineMargin,
                    pageWidth - 2 * lineMargin);

  if (cursorMode && !togglePos && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorPos < text.length()) {
      const char buf[2] = {text[cursorPos], '\0'};
      renderer.drawText(controlFontId, cursorPixelX, cursorLineY, buf, false);
    }
  } else if (cursorPos <= displayText.length()) {
    static constexpr int serifW = 3;
    const int cX = cursorPixelX;
    const int cY = cursorLineY;
    const int cBottom = cursorLineY + lineHeight - 1;
    renderer.fillRect(cX, cY, 2, lineHeight, true);
    renderer.drawLine(cX - serifW, cY, cX - 1, cY, 2, true);
    renderer.drawLine(cX + 1, cY, cX + serifW, cY, 2, true);
    renderer.drawLine(cX - serifW, cBottom, cX - 1, cBottom, 2, true);
    renderer.drawLine(cX + 1, cBottom, cX + serifW, cBottom, 2, true);
  }

  if (isPassword) {
    const char* toggleLabel = passwordVisible ? "[***]" : "[abc]";
    const int toggleWidth = renderer.getTextWidth(controlFontId, toggleLabel);
    const int toggleX = pageWidth - effectiveMargin - toggleWidth;
    const int toggleY = inputStartY + inputHeight;
    const bool toggleSelected = cursorMode && togglePos;
    passwordToggleTouchRect = Rect{toggleX - 4, toggleY - 4, toggleWidth + 8, lineHeight + 8};
    hasPasswordToggleTouch = true;

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(controlFontId, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(controlFontId, toggleX, toggleY, toggleLabel, true);
    }
  }

  if (hintVisible && !text.empty()) {
    const int hintLh = renderer.getLineHeight(hintFontId);
    const int underlineY = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;
    const int hintY = underlineY + 4;
    if (cursorMode) {
      int hintLineY = hintY;
      if (inputType == InputType::Password && togglePos) {
        renderer.drawCenteredText(
            hintFontId, hintLineY,
            passwordVisible ? tr(STR_KB_HINT_TOGGLE_HIDE_PASSWORD) : tr(STR_KB_HINT_TOGGLE_SHOW_PASSWORD), true);
        hintLineY += hintLh;
        renderer.drawCenteredText(hintFontId, hintLineY, tr(STR_KB_HINT_RETURN_CURSOR), true);
      } else {
        renderer.drawCenteredText(hintFontId, hintLineY, tr(STR_KB_HINT_MOVE_CURSOR), true);
        hintLineY += hintLh;
        if (inputType == InputType::Password) {
          const char* passTip = passwordVisible ? tr(STR_KB_HINT_HIDE_PASSWORD) : tr(STR_KB_HINT_SHOW_PASSWORD);
          renderer.drawCenteredText(hintFontId, hintLineY, passTip, true);
        }
      }
    } else {
      renderer.drawCenteredText(hintFontId, hintY, tr(STR_KB_HINT_EDIT_ENTRY), true);
    }
  }

  const bool touchKeyboard = gpio.hasTouch();
  const int keyHeight = std::max(touchKeyboard ? 52 : metrics.keyboardKeyHeight,
                                 renderer.getLineHeight(controlFontId) + (touchKeyboard ? 12 : 8));
  const int bottomKeyHeight = std::max(touchKeyboard ? 46 : metrics.keyboardBottomKeyHeight,
                                       renderer.getLineHeight(controlFontId) + (touchKeyboard ? 12 : 8));
  const int keySpacing = metrics.keyboardKeySpacing;
  const int contentCols = getContentColCount();
  // Side button hints occupy the right edge on this device. Center every
  // keyboard row in the remaining area rather than the full framebuffer, so
  // the last keys never draw underneath the physical-button column.
  const int keyboardAvailableWidth = availableWidth;
  const int keyboardWidthPercent = touchKeyboard ? 96 : metrics.keyboardWidthPercent;
  const int keyboardWidth = keyboardAvailableWidth * keyboardWidthPercent / 100;
  const int keyWidth = (keyboardWidth - (contentCols - 1) * keySpacing) / contentCols;
  const int keyboardContentX = gpio.deviceIsX3() ? metrics.sideButtonHintsWidth + sideHintClearance : 0;
  const int leftMargin =
      keyboardContentX + (keyboardAvailableWidth - (contentCols * keyWidth + (contentCols - 1) * keySpacing)) / 2;

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (keyHeight + keySpacing) * getContentRowCount() - bottomKeyHeight -
                                       bottomRowGap + metrics.keyboardVerticalOffset
                                 : inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;

  const int tipsLh = renderer.getLineHeight(hintFontId);
  const int underlineBottom = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing + 4;
  // Help is drawn in the same safe column as the keyboard. At the large
  // accessibility size the explanatory line is deliberately wrapped instead
  // of being clipped under the X4's Up/Down buttons.
  const int tipMaxWidth = keyboardAvailableWidth;
  const int maxTipLines = std::max(1, (keyboardStartY - underlineBottom) / tipsLh);
  std::vector<const char*> tipMessages;
  tipMessages.reserve(4);
  tipMessages.push_back(tr(STR_KB_TIPS));
  if (cursorMode) {
    tipMessages.push_back(tr(STR_KB_HINT_RETURN_KEYBOARD));
  } else if (urlMode) {
    tipMessages.push_back(tr(STR_KB_HINT_EXIT_URL_MODE));
    if (!text.empty()) tipMessages.push_back(tr(STR_KB_HINT_CLEAR_TEXT));
  } else if (symMode) {
    if (!text.empty()) tipMessages.push_back(tr(STR_KB_HINT_CLEAR_TEXT));
  } else {
    if (inputType == InputType::Url) {
      tipMessages.push_back(tr(STR_KB_HINT_SECONDARY_CHAR));
      tipMessages.push_back(tr(STR_KB_HINT_URL_SNIPPETS));
    } else {
      // Keep the familiar case hint on touch devices too, but name the control
      // the user actually sees: SHIFT instead of the old SELECT button.
      if (touchKeyboard) {
        static std::string touchShiftHint;
        touchShiftHint = shiftState > 0 ? tr(STR_KB_HINT_LOWER_SECONDARY) : tr(STR_KB_HINT_UPPER_SECONDARY);
        const char* words[] = {"ВЫБРАТЬ", "ВИБРАТИ", "SELECT"};
        for (const char* word : words) {
          const auto pos = touchShiftHint.find(word);
          if (pos != std::string::npos) {
            touchShiftHint.replace(pos, strlen(word), "SHIFT");
            break;
          }
        }
        tipMessages.push_back(touchShiftHint.c_str());
      } else {
        tipMessages.push_back(shiftState > 0 ? tr(STR_KB_HINT_LOWER_SECONDARY) : tr(STR_KB_HINT_UPPER_SECONDARY));
      }
    }
    if (!text.empty()) tipMessages.push_back(tr(STR_KB_HINT_CLEAR_TEXT));
  }

  std::vector<std::string> tipLines;
  tipLines.reserve(static_cast<size_t>(maxTipLines));
  for (size_t i = 0; i < tipMessages.size() && static_cast<int>(tipLines.size()) < maxTipLines; ++i) {
    const int remainingMessages = static_cast<int>(tipMessages.size() - i - 1);
    const int lineLimit = std::max(1, maxTipLines - static_cast<int>(tipLines.size()) - remainingMessages);
    auto wrapped = renderer.wrappedText(hintFontId, tipMessages[i], tipMaxWidth, lineLimit);
    for (auto& line : wrapped) {
      if (static_cast<int>(tipLines.size()) >= maxTipLines) break;
      tipLines.push_back(std::move(line));
    }
  }

  const int tipBlockHeight = static_cast<int>(tipLines.size()) * tipsLh;
  int tipY = underlineBottom + std::max(0, (keyboardStartY - underlineBottom - tipBlockHeight) / 2);
  for (const auto& line : tipLines) {
    const int textWidth = renderer.getTextWidth(hintFontId, line.c_str());
    renderer.drawText(hintFontId, keyboardContentX + (keyboardAvailableWidth - textWidth) / 2, tipY, line.c_str(), true);
    tipY += tipsLh;
  }

  const int bkSpacing = metrics.keyboardBottomKeySpacing;
  const int abcKeyWidth = (keyboardWidth - (COLS - 1) * keySpacing) / COLS;
  const int contentTotalWidth = COLS * abcKeyWidth + (COLS - 1) * keySpacing;
  const int bottomKeyWidth = (contentTotalWidth - (BOTTOM_KEY_COUNT - 1) * bkSpacing) / BOTTOM_KEY_COUNT;
  const int bottomLeftMargin =
      keyboardContentX +
      (keyboardAvailableWidth - (BOTTOM_KEY_COUNT * bottomKeyWidth + (BOTTOM_KEY_COUNT - 1) * bkSpacing)) / 2;

  int urlLeftMargin = leftMargin;
  if (urlMode) {
    const int urlTotalWidth = 3 * keyWidth + 2 * keySpacing;
    const int urlCenterX =
        bottomLeftMargin + static_cast<int>(SpecialKeyType::Space) * (bottomKeyWidth + bkSpacing) + bottomKeyWidth / 2;
    urlLeftMargin = urlCenterX - urlTotalWidth / 2;
  }

  const KeyDef(*layout)[COLS] = symMode ? symLayout : (inputType == InputType::Url ? urlLayout : abcLayout);
  const int contentRows = getContentRowCount();

  // Build the next touch table from the exact rectangles being drawn. Publish
  // it only after the whole frame is complete so loop() never sees half a
  // keyboard during a render on the other core.
  const uint8_t publishTable = static_cast<uint8_t>(1U - activeTouchTable.load(std::memory_order_relaxed));
  uint8_t publishCount = 0;
  auto addTouchKey = [&](const Rect& rect, const int row, const int col) {
    if (publishCount >= TOUCH_KEY_CAPACITY) return;
    touchKeys[publishTable][publishCount++] = TouchKey{rect, static_cast<int8_t>(row), static_cast<int8_t>(col)};
  };

  if (hasPasswordToggleTouch) addTouchKey(passwordToggleTouchRect, -2, 0);

  for (int row = 0; row < contentRows; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);
    const int rowLeftMargin = urlMode ? urlLeftMargin : leftMargin;

    for (int col = 0; col < contentCols; col++) {
      const int keyX = rowLeftMargin + col * (keyWidth + keySpacing);
      const bool isSelected = row == selectedRow && col == selectedCol;
      const bool activeKeySelected = isSelected && !cursorMode;

      if (urlMode) {
        const int snippetIdx = col + row * 3;
        if (snippetIdx < URL_SNIPPET_COUNT) {
          GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, urlSnippets[snippetIdx],
                              activeKeySelected, nullptr);
        }
      } else {
        char primaryBuf[2];
        char secondaryBuf[2];
        const char* primary = keyLabel(row, col, !symMode && shiftState > 0, primaryBuf);
        const char* secondary = keyLabel(row, col, !symMode && shiftState == 0, secondaryBuf);
        // A 14pt accessibility glyph plus its alternate cannot fit in the
        // top half of a key without the two bitmaps touching. The alternate
        // remains available through a long press and is explained above the
        // keyboard; hide only its tiny visual label in this mode.
        const bool showSecondary = !symMode && row == 0 && secondaryBuf[0] != '\0' &&
                                   uiControlFontId() != UI_14_FONT_ID;
        GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, primary, activeKeySelected,
                            showSecondary ? secondary : nullptr);
      }
      addTouchKey(Rect{keyX, rowY, keyWidth, keyHeight}, row, col);
    }
  }

  const int bottomRowY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;
  const bool bottomSelected = isBottomRow(selectedRow);

  struct BottomKeyInfo {
    KeyboardKeyType themeType;
    const char* label;
  };
  const BottomKeyInfo bottomKeys[BOTTOM_KEY_COUNT] = {
      {(symMode || urlMode || inputType == InputType::Url) ? KeyboardKeyType::Disabled : KeyboardKeyType::Shift,
       (symMode || urlMode || inputType == InputType::Url) ? shiftString[0] : shiftString[shiftState]},
      {KeyboardKeyType::Mode, urlMode ? "abc" : (symMode ? "abc" : languageModeLabel())},
      {inputType == InputType::Url ? KeyboardKeyType::Mode : KeyboardKeyType::Space,
       inputType == InputType::Url ? "URL" : nullptr},
      {KeyboardKeyType::Del, nullptr},
      {KeyboardKeyType::Ok, tr(STR_OK_BUTTON)},
  };

  for (int i = 0; i < BOTTOM_KEY_COUNT; i++) {
    const int keyX = bottomLeftMargin + i * (bottomKeyWidth + bkSpacing);
    const bool isSelected = bottomSelected && i == selectedCol;

    const bool activeKeySelected = isSelected && !cursorMode;
    GUI.drawKeyboardKey(renderer, Rect{keyX, bottomRowY, bottomKeyWidth, bottomKeyHeight}, bottomKeys[i].label,
                        activeKeySelected, nullptr, bottomKeys[i].themeType);
    addTouchKey(Rect{keyX, bottomRowY, bottomKeyWidth, bottomKeyHeight}, contentRows, i);
  }

  if (cursorMode) {
    int selKeyX, selKeyY, selKeyW, selKeyH;
    if (isBottomRow(selectedRow)) {
      selKeyX = bottomLeftMargin + selectedCol * (bottomKeyWidth + bkSpacing);
      selKeyY = bottomRowY;
      selKeyW = bottomKeyWidth;
      selKeyH = bottomKeyHeight;
    } else {
      const int rowLM = urlMode ? urlLeftMargin : leftMargin;
      selKeyX = rowLM + selectedCol * (keyWidth + keySpacing);
      selKeyY = keyboardStartY + selectedRow * (keyHeight + keySpacing);
      selKeyW = keyWidth;
      selKeyH = keyHeight;
    }
    if (isBottomRow(selectedRow)) {
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, bottomKeys[selectedCol].label, true,
                          nullptr, bottomKeys[selectedCol].themeType, true);
    } else if (urlMode) {
      const int idx = selectedCol + selectedRow * 3;
      if (idx < URL_SNIPPET_COUNT) {
        GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, urlSnippets[idx], true, nullptr,
                            KeyboardKeyType::Normal, true);
      }
    } else {
      char selPrimaryBuf[2];
      char selSecondaryBuf[2];
      const char* selPrimary = keyLabel(selectedRow, selectedCol, !symMode && shiftState > 0, selPrimaryBuf);
      const char* selSecondary = keyLabel(selectedRow, selectedCol, !symMode && shiftState == 0, selSecondaryBuf);
      const bool selShowSecondary = !symMode && selectedRow == 0 && selSecondaryBuf[0] != '\0' &&
                                    uiControlFontId() != UI_14_FONT_ID;
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, selPrimary, true,
                          selShowSecondary ? selSecondary : nullptr, KeyboardKeyType::Normal, true);
    }
  }

  touchKeyCounts[publishTable] = publishCount;
  activeTouchTable.store(publishTable, std::memory_order_release);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  GUI.drawSideButtonHints(renderer, ">", "<");

  renderer.displayBuffer();
}

void KeyboardEntryActivity::onComplete(std::string text) {
  setResult(KeyboardResult{std::move(text)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
