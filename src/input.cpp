#include "slugia/input.hpp"

#include <algorithm>

namespace slugia {

namespace {
const ButtonState emptyButton{};

std::size_t mapMouseButton(int button) {
  switch (button) {
  case 0:
    return static_cast<std::size_t>(MouseButton::Left);
  case 1:
    return static_cast<std::size_t>(MouseButton::Right);
  case 2:
    return static_cast<std::size_t>(MouseButton::Middle);
  case 3:
    return static_cast<std::size_t>(MouseButton::Other1);
  case 4:
    return static_cast<std::size_t>(MouseButton::Other2);
  case 5:
    return static_cast<std::size_t>(MouseButton::Other3);
  case 6:
    return static_cast<std::size_t>(MouseButton::Other4);
  case 7:
    return static_cast<std::size_t>(MouseButton::Other5);
  default:
    return static_cast<std::size_t>(MouseButton::Count);
  }
}
} // namespace

const ButtonState& InputState::mouse(MouseButton button) const {
  const auto index = static_cast<std::size_t>(button);
  return index < mouse_.size() ? mouse_[index] : emptyButton;
}

const ButtonState& InputState::key(int keyCode) const {
  return keyCode >= 0 && keyCode < keyCount ? keys_[static_cast<std::size_t>(keyCode)]
                                            : emptyButton;
}

void InputState::beginFrame() {
  focusLostThisFrame_ = false;
  for (auto& state : mouse_) {
    state.pressed = false;
    state.released = false;
  }
  for (auto& state : keys_) {
    state.pressed = false;
    state.released = false;
  }
  previousCursor_ = cursor_;
  cursorDelta_ = {};
  rawDelta_ = {};
  rawDeltaSamples_.clear();
  scroll_.started = false;
  scroll_.ended = false;
  scroll_.delta = {};
  scroll_.direction = ScrollDirection::None;
  textInput_.clear();
  composition_.changedThisFrame = false;
  composition_.committedThisFrame = false;
}

void InputState::finishFrame(double nowSeconds) {
  refreshCursorDelta();
  if (scroll_.active && lastScrollTime_ >= 0.0 && nowSeconds - lastScrollTime_ > 0.12) {
    scroll_.active = false;
    scroll_.ended = true;
  }
}

void InputState::refreshCursorDelta() {
  cursorDelta_ = cursor_ - previousCursor_;
}

void InputState::onCursor(double x, double y) {
  const Vec2 next{static_cast<float>(x), static_cast<float>(y)};
  const Vec2 delta = next - cursor_;
  cursor_ = next;
  rawDelta_ = rawDelta_ + delta;
  if (delta.x != 0.0f || delta.y != 0.0f) {
    rawDeltaSamples_.push_back(delta);
  }
}

void InputState::onMouseButton(int button, int action) {
  const auto index = mapMouseButton(button);
  if (index >= mouse_.size())
    return;
  auto& state = mouse_[index];
  if (action == 1) {
    state.pressed = state.pressed || !state.down;
    state.down = true;
  }
  if (action == 0) {
    state.released = state.released || state.down;
    state.down = false;
  }
}

void InputState::onKey(int keyCode, int action) {
  if (keyCode < 0 || keyCode >= keyCount)
    return;
  auto& state = keys_[static_cast<std::size_t>(keyCode)];
  if (action == 1) {
    state.pressed = state.pressed || !state.down;
    state.down = true;
  }
  if (action == 0) {
    state.released = state.released || state.down;
    state.down = false;
  }
  if (action == 2)
    state.down = true;
}

void InputState::onScroll(double x, double y, double nowSeconds) {
  const bool newGesture =
      !scroll_.active || lastScrollTime_ < 0.0 || nowSeconds - lastScrollTime_ > 0.12;
  scroll_.started = scroll_.started || newGesture;
  scroll_.active = true;
  scroll_.delta = scroll_.delta + Vec2{static_cast<float>(x), static_cast<float>(y)};
  scroll_.direction =
      y > 0.0 ? ScrollDirection::Up : (y < 0.0 ? ScrollDirection::Down : ScrollDirection::None);
  lastScrollTime_ = nowSeconds;
}

void InputState::onCodepoint(std::uint32_t codepoint) {
  if (codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU))
    textInput_.push_back(static_cast<char32_t>(codepoint));
}

void InputState::onComposition(std::u32string_view text, std::size_t selectionStart,
                               std::size_t selectionLength) {
  composition_.text.assign(text);
  composition_.selectionStart = std::min(selectionStart, composition_.text.size());
  composition_.selectionLength = std::min(
    selectionLength, composition_.text.size() - composition_.selectionStart);
  composition_.active = !composition_.text.empty();
  composition_.changedThisFrame = true;
}

void InputState::onCompositionCommit(std::u32string_view text) {
  textInput_.append(text);
  composition_.text.clear();
  composition_.selectionStart = 0;
  composition_.selectionLength = 0;
  composition_.active = false;
  composition_.changedThisFrame = true;
  composition_.committedThisFrame = true;
}

void InputState::onCompositionCancel() {
  if (!composition_.active && composition_.text.empty()) return;
  composition_.text.clear();
  composition_.selectionStart = 0;
  composition_.selectionLength = 0;
  composition_.active = false;
  composition_.changedThisFrame = true;
}

void InputWriter::beginFrame() noexcept {
  state_.beginFrame();
}

void InputWriter::finishFrame(double nowSeconds) noexcept {
  state_.finishFrame(nowSeconds);
}

void InputWriter::cursor(Vec2 position) noexcept {
  state_.onCursor(position.x, position.y);
}

void InputWriter::mouseButton(MouseButton button, InputAction action) noexcept {
  state_.onMouseButton(static_cast<int>(button), static_cast<int>(action));
}

void InputWriter::key(int keyCode, InputAction action) noexcept {
  state_.onKey(keyCode, static_cast<int>(action));
}

void InputWriter::scroll(Vec2 delta, double nowSeconds) noexcept {
  state_.onScroll(delta.x, delta.y, nowSeconds);
}

void InputWriter::codepoint(std::uint32_t value) noexcept {
  state_.onCodepoint(value);
}

void InputWriter::composition(std::u32string_view text, std::size_t selectionStart,
                              std::size_t selectionLength) noexcept {
  state_.onComposition(text, selectionStart, selectionLength);
}

void InputWriter::commitComposition(std::u32string_view text) noexcept {
  state_.onCompositionCommit(text);
}

void InputWriter::cancelComposition() noexcept {
  state_.onCompositionCancel();
}

void InputWriter::focusLost() noexcept {
  state_.focusLostThisFrame_ = true;
  state_.onCompositionCancel();
  for (auto& state : state_.mouse_) {
    state.released = state.released || state.down;
    state.down = false;
  }
  for (auto& state : state_.keys_) {
    state.released = state.released || state.down;
    state.down = false;
  }
}

} // namespace slugia
