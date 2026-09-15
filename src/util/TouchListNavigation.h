#pragma once

#include <algorithm>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

namespace TouchListNavigation {
struct Result {
  bool handled = false;
  bool activate = false;
};

inline Result handle(MappedInputManager& input, int& selectedIndex, int totalItems, const Rect& rect, int rowHeight) {
  if (!input.hasTouch() || totalItems <= 0 || rowHeight <= 0 || rect.height <= 0) return {};

  const int pageItems = std::max(1, rect.height / rowHeight);
  const auto swipe = input.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    return {true, false};
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    return {true, false};
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return {};
  if (x < rect.x || x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height) return {};

  const int pageStart = (std::max(0, selectedIndex) / pageItems) * pageItems;
  const int row = (y - rect.y) / rowHeight;
  const int index = pageStart + row;
  if (index < 0 || index >= totalItems) return {};

  selectedIndex = index;
  return {true, true};
}
}  // namespace TouchListNavigation
