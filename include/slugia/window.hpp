#pragma once

#include "slugia/input.hpp"

#include <exception>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace slugia {

struct WindowConfig {
  int width = 1280;
  int height = 800;
  std::string title = "slugia";
  bool resizable = true;
  bool visible = true;
  // Set false only when the parent application initializes GLFW before this Window and
  // terminates it after every slugia Window has been destroyed.
  bool manageGlfwLifetime = true;
};

class Window {
public:
  explicit Window(const WindowConfig& config = {});
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool shouldClose() const;
  void requestClose();
  void setSize(int width, int height);
  // Invoked from the platform refresh event, including the Windows modal resize loop.
  void setRefreshCallback(std::function<void()> callback);
  // Called on the event thread when files are dropped on the native window. Paths remain owned by
  // the callback vector, so copy any path that must outlive the callback.
  void setDropCallback(std::function<void(const std::vector<std::string>&)> callback);
  void pollEvents();
  // Refreshes only the absolute pointer position. Call immediately before building latency-critical
  // drag UI when useful; button/key edges still come from pollEvents().
  void resampleCursor();
  void waitForVisibleFramebuffer();
  void setRawMouseMotion(bool enabled);
  [[nodiscard]] bool rawMouseMotionSupported() const;
  [[nodiscard]] Vec2 framebufferSize() const;
  [[nodiscard]] float contentScale() const;
  [[nodiscard]] const InputState& input() const { return input_; }
  [[nodiscard]] InputState& input() { return input_; }
  [[nodiscard]] GLFWwindow* native() const { return window_; }

private:
  void invokeRefreshCallback() noexcept;
  void updateFramebufferScale();
  [[nodiscard]] Vec2 cursorInFramebuffer(double x, double y) const;
  GLFWwindow* window_ = nullptr;
  bool ownsGlfwReference_ = false;
  Vec2 framebufferScale_ = {1.0f, 1.0f};
  InputState input_{};
  std::function<void()> refreshCallback_{};
  std::function<void(const std::vector<std::string>&)> dropCallback_{};
  std::exception_ptr callbackException_{};
};

} // namespace slugia
