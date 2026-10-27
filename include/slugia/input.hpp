#pragma once

#include "slugia/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slugia {

enum class MouseButton : std::uint8_t {
  Left,
  Right,
  Middle,
  Other1,
  Other2,
  Other3,
  Other4,
  Other5,
  Count
};

struct ButtonState {
  bool pressed = false;
  bool released = false;
  bool down = false;
};

enum class ScrollDirection : std::int8_t { None = 0, Up = 1, Down = -1 };
enum class InputAction : std::uint8_t { Release = 0, Press = 1, Repeat = 2 };

// Stable platform-independent keys used by slugia UI. Native adapters map
// their backend key codes to these values while the legacy integer API remains available.
enum class Key : std::uint16_t {
  Unknown = 0,
  Space,
  A, C, V, X, Y, Z,
  Enter,
  Escape,
  Tab,
  Backspace,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  LeftShift,
  RightShift,
  LeftControl,
  RightControl,
  LeftSuper,
  RightSuper,
  K,
  Count
};

struct ScrollState {
  bool started = false;
  bool ended = false;
  bool active = false;
  Vec2 delta = {};
  ScrollDirection direction = ScrollDirection::None;
};

struct CompositionState {
  std::u32string text = {};
  std::size_t selectionStart = 0;
  std::size_t selectionLength = 0;
  bool active = false;
  bool changedThisFrame = false;
  bool committedThisFrame = false;
};

class InputState {
public:
  static constexpr int keyCount = 512;

  [[nodiscard]] Vec2 cursorPosition() const {
    return cursor_;
  }
  [[nodiscard]] Vec2 cursorDelta() const {
    return cursorDelta_;
  }
  [[nodiscard]] Vec2 rawMouseDelta() const {
    return rawDelta_;
  }
  [[nodiscard]] const std::vector<Vec2>& rawMouseDeltas() const {
    return rawDeltaSamples_;
  }
  [[nodiscard]] const ScrollState& scroll() const {
    return scroll_;
  }
  [[nodiscard]] const ButtonState& mouse(MouseButton button) const;
  [[nodiscard]] const ButtonState& key(int key) const;
  [[nodiscard]] const ButtonState& key(Key key) const {
    return this->key(static_cast<int>(key));
  }
  [[nodiscard]] const std::u32string& textInput() const {
    return textInput_;
  }
  [[nodiscard]] const CompositionState& composition() const {
    return composition_;
  }
  [[nodiscard]] bool focusLostThisFrame() const {
    return focusLostThisFrame_;
  }

private:
  friend class Window;
  friend class InputWriter;
  void beginFrame();
  void finishFrame(double nowSeconds);
  void refreshCursorDelta();
  void onCursor(double x, double y);
  void onMouseButton(int button, int action);
  void onKey(int key, int action);
  void onScroll(double x, double y, double nowSeconds);
  void onCodepoint(std::uint32_t codepoint);
  void onComposition(std::u32string_view text, std::size_t selectionStart,
                     std::size_t selectionLength);
  void onCompositionCommit(std::u32string_view text);
  void onCompositionCancel();

  std::array<ButtonState, static_cast<std::size_t>(MouseButton::Count)> mouse_{};
  std::array<ButtonState, keyCount> keys_{};
  Vec2 cursor_{};
  Vec2 previousCursor_{};
  Vec2 cursorDelta_{};
  Vec2 rawDelta_{};
  std::vector<Vec2> rawDeltaSamples_{};
  ScrollState scroll_{};
  std::u32string textInput_{};
  CompositionState composition_{};
  double lastScrollTime_ = -1.0;
  bool focusLostThisFrame_ = false;
};

// Narrow write-side API for native host adapters. Read-side UI code continues
// to receive an immutable InputState reference for the current frame.
class InputWriter {
public:
  explicit InputWriter(InputState& state) noexcept : state_(state) {}

  void beginFrame() noexcept;
  void finishFrame(double nowSeconds) noexcept;
  void cursor(Vec2 position) noexcept;
  void mouseButton(MouseButton button, InputAction action) noexcept;
  void key(int keyCode, InputAction action) noexcept;
  void key(Key key, InputAction action) noexcept {
    this->key(static_cast<int>(key), action);
  }
  void scroll(Vec2 delta, double nowSeconds) noexcept;
  void codepoint(std::uint32_t value) noexcept;
  void composition(std::u32string_view text, std::size_t selectionStart = 0,
                   std::size_t selectionLength = 0) noexcept;
  void commitComposition(std::u32string_view text) noexcept;
  void cancelComposition() noexcept;
  void focusLost() noexcept;

private:
  InputState& state_;
};

} // namespace slugia
