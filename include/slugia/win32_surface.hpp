#pragma once

#if !defined(_WIN32)
#error "slugia/win32_surface.hpp is only available on Windows"
#endif

#include "slugia/platform_surface.hpp"

#include <array>

namespace slugia {

// Lightweight HWND adapter for hosts that already own the Win32 window/message loop.
// Handles are typed as void* to keep <windows.h> out of public slugia headers.
class Win32PlatformSurface final : public PlatformSurface {
public:
  explicit Win32PlatformSurface(void* hwnd, void* moduleHandle = nullptr);

  [[nodiscard]] std::span<const char* const>
  requiredInstanceExtensions() const noexcept override;
  VkResult createVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator,
                               VkSurfaceKHR* surface) const noexcept override;
  [[nodiscard]] Vec2 framebufferSize() const noexcept override;
  [[nodiscard]] float contentScale() const noexcept override;
  [[nodiscard]] bool visible() const noexcept override;
  void requestRedraw() noexcept override;

  [[nodiscard]] void* nativeWindow() const noexcept { return hwnd_; }

private:
  void* hwnd_ = nullptr;
  void* moduleHandle_ = nullptr;
  std::array<const char*, 2> extensions_{};
};

} // namespace slugia
