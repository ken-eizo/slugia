#include "slugia/window.hpp"

#define GLFW_INCLUDE_NONE
#if defined(__APPLE__)
#include <vulkan/vulkan.h>
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace slugia {

namespace {
int glfwReferenceCount = 0;

Window* windowFrom(GLFWwindow* window) {
  return static_cast<Window*>(glfwGetWindowUserPointer(window));
}

Key mapGlfwKey(int key) noexcept {
  switch (key) {
  case GLFW_KEY_SPACE: return Key::Space;
  case GLFW_KEY_A: return Key::A;
  case GLFW_KEY_C: return Key::C;
  case GLFW_KEY_V: return Key::V;
  case GLFW_KEY_X: return Key::X;
  case GLFW_KEY_Y: return Key::Y;
  case GLFW_KEY_Z: return Key::Z;
  case GLFW_KEY_K: return Key::K;
  case GLFW_KEY_ENTER: return Key::Enter;
  case GLFW_KEY_TAB: return Key::Tab;
  case GLFW_KEY_ESCAPE: return Key::Escape;
  case GLFW_KEY_BACKSPACE: return Key::Backspace;
  case GLFW_KEY_DELETE: return Key::Delete;
  case GLFW_KEY_LEFT: return Key::Left;
  case GLFW_KEY_RIGHT: return Key::Right;
  case GLFW_KEY_UP: return Key::Up;
  case GLFW_KEY_DOWN: return Key::Down;
  case GLFW_KEY_HOME: return Key::Home;
  case GLFW_KEY_END: return Key::End;
  case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
  case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
  case GLFW_KEY_LEFT_CONTROL: return Key::LeftControl;
  case GLFW_KEY_RIGHT_CONTROL: return Key::RightControl;
  case GLFW_KEY_LEFT_SUPER: return Key::LeftSuper;
  case GLFW_KEY_RIGHT_SUPER: return Key::RightSuper;
  default: return Key::Unknown;
  }
}

std::runtime_error glfwFailure(const char* operation) {
  const char* description = nullptr;
  const int code = glfwGetError(&description);
  std::string message = operation;
  message += " failed";
  if (description) message += ": " + std::string(description);
  if (code != GLFW_NO_ERROR) message += " (GLFW " + std::to_string(code) + ")";
  return std::runtime_error(message);
}

void retainGlfw() {
#if defined(__APPLE__)
  // Use the linked (and potentially bundle-local) MoltenVK entry points. Loading
  // a second system Vulkan library would mix implementations/require a global SDK.
  if (glfwReferenceCount == 0) glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
  if (glfwReferenceCount == 0 && glfwInit() != GLFW_TRUE) throw glfwFailure("glfwInit");
  ++glfwReferenceCount;
}

void releaseGlfw() {
  if (glfwReferenceCount > 0 && --glfwReferenceCount == 0) glfwTerminate();
}

}

Window::Window(const WindowConfig& config) {
  if (config.manageGlfwLifetime) {
    retainGlfw();
    ownsGlfwReference_ = true;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
  window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
  if (!window_) {
    auto failure = glfwFailure("glfwCreateWindow");
    if (ownsGlfwReference_) releaseGlfw();
    ownsGlfwReference_ = false;
    throw failure;
  }

  glfwSetWindowUserPointer(window_, this);
  updateFramebufferScale();
  glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
    const Vec2 position = windowFrom(w)->cursorInFramebuffer(x, y);
    windowFrom(w)->input_.onCursor(position.x, position.y);
  });
  glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
    windowFrom(w)->input_.onMouseButton(button, action);
  });
  glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
    const Key mapped = mapGlfwKey(key);
    if (mapped != Key::Unknown)
      windowFrom(w)->input_.onKey(static_cast<int>(mapped), action);
  });
  glfwSetScrollCallback(window_, [](GLFWwindow* w, double x, double y) {
    windowFrom(w)->input_.onScroll(x, y, glfwGetTime());
  });
  glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int codepoint) {
    windowFrom(w)->input_.onCodepoint(codepoint);
  });
  glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
    auto* owner = windowFrom(w);
    if (!owner->dropCallback_) return;
    std::vector<std::string> copied;
    copied.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
      if (paths[index]) copied.emplace_back(paths[index]);
    }
    try {
      owner->dropCallback_(copied);
    } catch (...) {
      owner->callbackException_ = std::current_exception();
    }
  });
  glfwSetWindowRefreshCallback(window_, [](GLFWwindow* w) {
    windowFrom(w)->invokeRefreshCallback();
  });
  glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int width, int height) {
    windowFrom(w)->updateFramebufferScale();
    if (width > 0 && height > 0) windowFrom(w)->invokeRefreshCallback();
  });
  glfwSetWindowSizeCallback(window_, [](GLFWwindow* w, int, int) {
    windowFrom(w)->updateFramebufferScale();
  });
  glfwSetWindowContentScaleCallback(window_, [](GLFWwindow* w, float, float) {
    windowFrom(w)->updateFramebufferScale();
  });

  double x = 0.0;
  double y = 0.0;
  glfwGetCursorPos(window_, &x, &y);
  const Vec2 position = cursorInFramebuffer(x, y);
  input_.onCursor(position.x, position.y);
}

Window::~Window() {
  if (window_) glfwDestroyWindow(window_);
  if (ownsGlfwReference_) releaseGlfw();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }
void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
void Window::setSize(int width, int height) {
  glfwSetWindowSize(window_, std::max(width, 1), std::max(height, 1));
}
void Window::setRefreshCallback(std::function<void()> callback) {
  refreshCallback_ = std::move(callback);
}
void Window::setDropCallback(
    std::function<void(const std::vector<std::string>&)> callback) {
  dropCallback_ = std::move(callback);
}

void Window::invokeRefreshCallback() noexcept {
  if (!refreshCallback_ || callbackException_) return;
  try {
    refreshCallback_();
  } catch (...) {
    callbackException_ = std::current_exception();
  }
}

void Window::updateFramebufferScale() {
  int windowWidth = 0;
  int windowHeight = 0;
  int framebufferWidth = 0;
  int framebufferHeight = 0;
  glfwGetWindowSize(window_, &windowWidth, &windowHeight);
  glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
  framebufferScale_.x = windowWidth > 0 ? static_cast<float>(framebufferWidth) / windowWidth : 1.0f;
  framebufferScale_.y = windowHeight > 0 ? static_cast<float>(framebufferHeight) / windowHeight : 1.0f;
}

Vec2 Window::cursorInFramebuffer(double x, double y) const {
  return {static_cast<float>(x) * framebufferScale_.x,
          static_cast<float>(y) * framebufferScale_.y};
}

void Window::pollEvents() {
  input_.beginFrame();
  glfwPollEvents();
  if (callbackException_) {
    auto error = std::exchange(callbackException_, {});
    std::rethrow_exception(error);
  }
  // Sample once more after dispatching callbacks so high-rate pointer motion cannot leave the
  // declarative frame on an older coalesced callback position.
  resampleCursor();
  input_.finishFrame(glfwGetTime());
}

void Window::resampleCursor() {
  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window_, &cursorX, &cursorY);
  const Vec2 position = cursorInFramebuffer(cursorX, cursorY);
  input_.onCursor(position.x, position.y);
  input_.refreshCursorDelta();
}

void Window::waitForVisibleFramebuffer() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  while ((width == 0 || height == 0) && !shouldClose()) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &width, &height);
  }
}

void Window::setRawMouseMotion(bool enabled) {
  const bool rawMouseMotion = enabled && rawMouseMotionSupported();
  glfwSetInputMode(window_, GLFW_CURSOR, rawMouseMotion ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (rawMouseMotionSupported())
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, rawMouseMotion ? GLFW_TRUE : GLFW_FALSE);
}

bool Window::rawMouseMotionSupported() const {
  return glfwRawMouseMotionSupported() == GLFW_TRUE;
}

Vec2 Window::framebufferSize() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  return {static_cast<float>(width), static_cast<float>(height)};
}

float Window::contentScale() const {
  float x = 1.0f;
  float y = 1.0f;
  glfwGetWindowContentScale(window_, &x, &y);
  return (x + y) * 0.5f;
}

} // namespace slugia
