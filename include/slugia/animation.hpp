#pragma once

#include "slugia/types.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace slugia {

enum class Easing : std::uint8_t { Linear, EaseIn, EaseOut, EaseInOut, SmoothStep, Spring };

float ease(Easing easing, float t);

template <typename T>
class Tween {
public:
  using Interpolator = std::function<T(const T&, const T&, float)>;

  Tween() = default;
  Tween(T from, T to, float durationMs, Easing easing, Interpolator interpolator)
      : from_(std::move(from)), to_(std::move(to)), value_(from_),
        durationMs_(std::max(durationMs, 0.001f)), easing_(easing),
        interpolator_(std::move(interpolator)) {}

  void restart(T from, T to, float durationMs, Easing easing) {
    from_ = std::move(from);
    to_ = std::move(to);
    value_ = from_;
    durationMs_ = std::max(durationMs, 0.001f);
    elapsedMs_ = 0.0f;
    easing_ = easing;
    running_ = true;
  }

  const T& update(float deltaMs) {
    if (!running_) return value_;
    elapsedMs_ = std::min(elapsedMs_ + std::max(deltaMs, 0.0f), durationMs_);
    value_ = interpolator_(from_, to_, ease(easing_, elapsedMs_ / durationMs_));
    running_ = elapsedMs_ < durationMs_;
    return value_;
  }

  [[nodiscard]] const T& value() const { return value_; }
  [[nodiscard]] bool running() const { return running_; }

private:
  T from_{};
  T to_{};
  T value_{};
  float durationMs_ = 1.0f;
  float elapsedMs_ = 0.0f;
  Easing easing_ = Easing::Linear;
  Interpolator interpolator_{};
  bool running_ = true;
};

inline Tween<float> tween(float from, float to, float durationMs, Easing easing = Easing::EaseInOut) {
  return Tween<float>(from, to, durationMs, easing,
    [](const float& a, const float& b, float t) { return a + (b - a) * t; });
}

inline Tween<Color> tween(Color from, Color to, float durationMs, Easing easing = Easing::EaseInOut) {
  return Tween<Color>(from, to, durationMs, easing,
    [](const Color& a, const Color& b, float t) { return lerp(a, b, t); });
}

} // namespace slugia
