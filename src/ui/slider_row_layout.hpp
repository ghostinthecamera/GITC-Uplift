#pragma once

namespace uplift::ui {

// A one-line slider row's widths, in pixels: caption, slider, value and the reset button's place, each part followed by `gap` but the last.
struct SliderRowWidths {
  float caption = 0.f;  // the caption and the gap after it
  float slider = 0.f;
  float value = 0.f;
};

// Lays out a row `row_width` wide: the caption column takes `caption_needed` (the panel's widest caption and its gap) up to `caption_cap`
// of the row, the value column `value_needed` (the panel's widest value), the reset button its place, the slider the rest.
[[nodiscard]] constexpr SliderRowWidths SliderRowLayout(float row_width, float caption_needed, float caption_cap, float value_needed,
                                                        float reset_width, float gap) {
  SliderRowWidths widths;
  const float cap = row_width * caption_cap;
  widths.caption = caption_needed < cap ? caption_needed : cap;
  widths.value = value_needed;
  widths.slider = row_width - widths.caption - widths.value - reset_width - 2.f * gap;
  return widths;
}

}  // namespace uplift::ui
