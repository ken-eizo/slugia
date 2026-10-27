#include "slugia/win32_surface.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>

#include <stdexcept>

namespace slugia {

Win32PlatformSurface::Win32PlatformSurface(void* hwnd, void* moduleHandle)
    : hwnd_(hwnd), moduleHandle_(moduleHandle ? moduleHandle : GetModuleHandleW(nullptr)) {
  if (!hwnd_) throw std::invalid_argument("Win32PlatformSurface requires a valid HWND");
  extensions_ = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
}

std::span<const char* const> Win32PlatformSurface::requiredInstanceExtensions() const noexcept {
  return extensions_;
}

VkResult Win32PlatformSurface::createVulkanSurface(
    VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const noexcept {
  VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  info.hinstance = reinterpret_cast<HINSTANCE>(moduleHandle_);
  info.hwnd = reinterpret_cast<HWND>(hwnd_);
  return vkCreateWin32SurfaceKHR(instance, &info, allocator, surface);
}
Vec2 Win32PlatformSurface::framebufferSize() const noexcept {
  RECT rect{};
  if (!GetClientRect(reinterpret_cast<HWND>(hwnd_), &rect)) return {};
  return {static_cast<float>(rect.right - rect.left),
          static_cast<float>(rect.bottom - rect.top)};
}

float Win32PlatformSurface::contentScale() const noexcept {
  const UINT dpi = GetDpiForWindow(reinterpret_cast<HWND>(hwnd_));
  return dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

bool Win32PlatformSurface::visible() const noexcept {
  const Vec2 size = framebufferSize();
  return IsWindowVisible(reinterpret_cast<HWND>(hwnd_)) && size.x > 0.0f && size.y > 0.0f;
}

void Win32PlatformSurface::requestRedraw() noexcept {
  InvalidateRect(reinterpret_cast<HWND>(hwnd_), nullptr, FALSE);
}

} // namespace slugia
