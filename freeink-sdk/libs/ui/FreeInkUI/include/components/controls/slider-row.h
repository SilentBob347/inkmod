#pragma once
#include "../../FreeInkUICore.h"
#include "../controls/button.h"
#include "../controls/capsule-slider.h"

namespace freeink {
namespace ui {
inline StyleSet sliderRowStepStyles(uint8_t radius) {
  StyleSet s{}; s.explicitlySet = true;
  s.normal.background = Paint::solid(Color::White); s.normal.foreground = Paint::solid(Color::Black);
  s.normal.border = Paint::solid(Color::Black); s.normal.borderWidth = 2; s.normal.radius = radius;
  s.selected = s.normal; s.focused = s.normal; s.active = s.normal;
  s.active.background = Paint::solid(Color::Black); s.active.foreground = Paint::solid(Color::White);
  s.disabled = s.normal; return s;
}
struct SliderRowProps {
  const char *label = nullptr; const char *value = nullptr; int32_t sliderValue = 0; int32_t max = 100;
  ActionId sliderAction = NO_ACTION; ActionId decrement = NO_ACTION; ActionId increment = NO_ACTION;
  int16_t decrementValue = -1; int16_t incrementValue = 1; const char *decrementLabel = "-"; const char *incrementLabel = "+";
  ActionId toggleAction = NO_ACTION; int16_t toggleValue = 0; BitmapRef toggleIcon{}; AssetRef toggleIconAsset{};
  uint16_t stepInputMask = InputTouch; TextStyle labelText{}; TextStyle valueText{}; TextStyle buttonText{}; StyleSet buttonStyles{};
  int16_t captionGap = 8; int16_t gap = 8; uint8_t buttonRadius = RADIUS_INHERIT; uint8_t capsuleRadius = RADIUS_INHERIT;
  int16_t stroke = 2; bool enabled = true;
};
inline int16_t sliderRowHeight(const DrawTarget &target, const SliderRowProps &props, int16_t controlHeight) {
  return static_cast<int16_t>(target.lineHeight(props.labelText.font) + props.captionGap + controlHeight);
}
template <size_t MaxInteractions>
void sliderRow(Frame<MaxInteractions> &frame, Rect rect, const SliderRowProps &props) {
  const int16_t lineH = frame.target().lineHeight(props.labelText.font);
  const Rect caption{rect.x, rect.y, rect.width, lineH};
  if (props.label) frame.target().text(caption, props.label, props.labelText);
  if (props.value) { TextStyle valueStyle = textStyleUnset(props.valueText) ? props.labelText : props.valueText; valueStyle.align = TextAlign::Right; frame.target().text(caption, props.value, valueStyle); }
  Rect band = rect; const int16_t consumed = static_cast<int16_t>(lineH + props.captionGap);
  band.y = static_cast<int16_t>(band.y + consumed); band.height = static_cast<int16_t>(rect.height > consumed ? rect.height - consumed : 0);
  if (band.height <= 0) return;
  const int16_t stepW = band.height;
  const StyleSet styles = props.buttonStyles.unset() ? sliderRowStepStyles(resolveRadius(props.buttonRadius, 18)) : props.buttonStyles;
  ButtonProps step; step.text = props.buttonText; step.styles = styles; step.inputMask = props.stepInputMask; step.enabled = props.enabled;
  int16_t bandRight = band.right();
  if (props.toggleAction != NO_ACTION) { ButtonProps toggleButton = step; toggleButton.label = nullptr; toggleButton.icon = props.toggleIcon; toggleButton.iconAsset = props.toggleIconAsset; toggleButton.action = props.toggleAction; toggleButton.value = props.toggleValue; button(frame, Rect{static_cast<int16_t>(bandRight - stepW), band.y, stepW, band.height}, toggleButton); bandRight = static_cast<int16_t>(bandRight - stepW - props.gap); }
  step.label = props.decrementLabel; step.action = props.decrement; step.value = props.decrementValue; button(frame, Rect{band.x, band.y, stepW, band.height}, step);
  const int16_t plusX = static_cast<int16_t>(bandRight - stepW);
  step.label = props.incrementLabel; step.action = props.increment; step.value = props.incrementValue; button(frame, Rect{plusX, band.y, stepW, band.height}, step);
  const int16_t trackX = static_cast<int16_t>(band.x + stepW + props.gap); const int16_t trackW = static_cast<int16_t>(plusX - props.gap - trackX);
  CapsuleSliderProps capsule; capsule.value = props.sliderValue; capsule.max = props.max; capsule.action = props.sliderAction; capsule.stroke = props.stroke; capsule.radius = props.capsuleRadius; capsule.enabled = props.enabled;
  capsuleSlider(frame, Rect{trackX, band.y, trackW, band.height}, capsule);
}
}  // namespace ui
}  // namespace freeink
