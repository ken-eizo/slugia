#include "slugia/animation.hpp"

#include <algorithm>
#include <cmath>

namespace slugia {

float ease(Easing easingType, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  switch (easingType) {
    case Easing::Linear: return t;
    case Easing::EaseIn: return t * t * t;
    case Easing::EaseOut: {
      const float u = 1.0f - t;
      return 1.0f - u * u * u;
    }
    case Easing::EaseInOut:
      return t < 0.5f ? 4.0f * t * t * t
                      : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    case Easing::SmoothStep: return t * t * (3.0f - 2.0f * t);
    case Easing::Spring:
      return t == 1.0f ? 1.0f : 1.0f - std::exp(-7.0f * t) * std::cos(12.0f * t);
  }
  return t;
}

} // namespace slugia
