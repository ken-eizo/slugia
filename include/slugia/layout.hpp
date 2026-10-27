#pragma once

#include "slugia/types.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace slugia {

struct Insets {
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  float left = 0.0f;

  static constexpr Insets all(float value) { return {value, value, value, value}; }
  static constexpr Insets symmetric(float vertical, float horizontal) {
    return {vertical, horizontal, vertical, horizontal};
  }
};

[[nodiscard]] constexpr Rect inset(Rect bounds, Insets values) {
  const float width = std::max(0.0f, bounds.width - values.left - values.right);
  const float height = std::max(0.0f, bounds.height - values.top - values.bottom);
  return {bounds.x + values.left, bounds.y + values.top, width, height};
}

[[nodiscard]] constexpr Rect outset(Rect bounds, Insets values) {
  return {bounds.x - values.left, bounds.y - values.top,
          bounds.width + values.left + values.right,
          bounds.height + values.top + values.bottom};
}

enum class Axis : std::uint8_t { Horizontal, Vertical };

class LinearLayout {
public:
  constexpr LinearLayout(Rect bounds, Axis axis, float gap = 0.0f) noexcept
      : remaining_(bounds), axis_(axis), gap_(std::max(0.0f, gap)) {}

  [[nodiscard]] Rect take(float extent) noexcept {
    extent = std::clamp(extent, 0.0f,
                        axis_ == Axis::Horizontal ? remaining_.width : remaining_.height);
    Rect result = remaining_;
    if (axis_ == Axis::Horizontal) {
      result.width = extent;
      const float advance = std::min(remaining_.width, extent + gap_);
      remaining_.x += advance;
      remaining_.width -= advance;
    } else {
      result.height = extent;
      const float advance = std::min(remaining_.height, extent + gap_);
      remaining_.y += advance;
      remaining_.height -= advance;
    }
    return result;
  }

  [[nodiscard]] constexpr Rect remaining() const noexcept { return remaining_; }

private:
  Rect remaining_{};
  Axis axis_ = Axis::Horizontal;
  float gap_ = 0.0f;
};

// Positive values are fixed pixel columns. Negative values are flex weights; -1 and -2 mean
// one and two shares of the space left after fixed columns and gaps.
[[nodiscard]] inline std::vector<Rect> gridColumns(Rect bounds, std::span<const float> columns,
                                                   float gap = 0.0f) {
  std::vector<Rect> result;
  result.reserve(columns.size());
  const float total_gap = std::max(0.0f, gap) *
                          static_cast<float>(columns.empty() ? 0U : columns.size() - 1U);
  float fixed = 0.0f;
  float flex = 0.0f;
  for (const float column : columns) {
    if (column >= 0.0f) fixed += column;
    else flex += -column;
  }
  const float available = std::max(0.0f, bounds.width - total_gap);
  const float flexible = std::max(0.0f, available - fixed);
  float x = bounds.x;
  for (const float column : columns) {
    const float width = column >= 0.0f ? column : (flex > 0.0f ? flexible * -column / flex : 0.0f);
    result.push_back({x, bounds.y, width, bounds.height});
    x += width + std::max(0.0f, gap);
  }
  return result;
}

} // namespace slugia
