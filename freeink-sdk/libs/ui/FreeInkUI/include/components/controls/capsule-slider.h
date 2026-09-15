#pragma once
#include "../../FreeInkUICore.h"

namespace freeink {
namespace ui {
struct CapsuleSliderProps {
  int32_t value = 0;
  int32_t max = 100;
  ActionId action = NO_ACTION;
  int16_t actionValue = 0;
  uint16_t inputMask = InputTouch | InputDrag;
  Paint track = Paint::solid(Color::White);
  Paint fill = Paint::solid(Color::Black);
  Paint handle = Paint::solid(Color::White);
  Paint border = Paint::solid(Color::Black);
  int16_t stroke = 2;
  uint8_t radius = RADIUS_INHERIT;
  bool enabled = true;
};
template <size_t MaxInteractions>
void capsuleSlider(Frame<MaxInteractions> &frame, Rect rect, const CapsuleSliderProps &props) {
  const int16_t stroke = props.stroke < 1 ? 1 : props.stroke;
  const int16_t minTrack = static_cast<int16_t>(rect.height + 2 * stroke);
  if (rect.width < minTrack || rect.height <= 2 * stroke) return;
  if (props.enabled && props.action != NO_ACTION) frame.hit(rect, props.action, props.actionValue, props.inputMask);
  const int16_t halfOuter = static_cast<int16_t>(rect.height / 2);
  const uint8_t radius = props.radius >= halfOuter ? static_cast<uint8_t>(halfOuter) : props.radius;
  frame.target().fill(rect, props.track, radius);
  const int32_t max = props.max <= 0 ? 1 : props.max;
  int32_t value = props.value < 0 ? 0 : props.value;
  if (value > max) value = max;
  const Rect inner = rect.inset(Insets{stroke, stroke, stroke, stroke});
  const int16_t cap = static_cast<int16_t>(inner.height / 2);
  const int16_t travel = static_cast<int16_t>(inner.width - 2 * cap);
  const int16_t handleCx = static_cast<int16_t>(inner.x + cap + (static_cast<int32_t>(travel) * value) / max);
  int16_t fillW = static_cast<int16_t>(handleCx + cap - inner.x);
  if (fillW > inner.width) fillW = inner.width;
  const uint8_t innerRadius = props.radius >= cap ? static_cast<uint8_t>(cap) : props.radius;
  const Paint fill = props.enabled ? props.fill : Paint::dither(Color::LightGray);
  frame.target().fill(Rect{inner.x, inner.y, fillW, inner.height}, fill, innerRadius);
  frame.target().stroke(rect, props.border, static_cast<uint8_t>(stroke), radius);
  const Rect handle{static_cast<int16_t>(handleCx - cap), inner.y, static_cast<int16_t>(cap * 2), inner.height};
  frame.target().fill(handle, props.handle, innerRadius);
  frame.target().stroke(handle, props.border, static_cast<uint8_t>(stroke), innerRadius);
}
}  // namespace ui
}  // namespace freeink
