#pragma once

#include "slugia/types.hpp"

#include <span>

#include <vulkan/vulkan.h>

namespace slugia {

// Host-neutral contract for presenting slugia into a surface owned by an
// application or plug-in. Implementations must outlive VulkanRenderer.
class PlatformSurface {
public:
  virtual ~PlatformSurface() = default;

  PlatformSurface(const PlatformSurface&) = delete;
  PlatformSurface& operator=(const PlatformSurface&) = delete;

  [[nodiscard]] virtual std::span<const char* const>
  requiredInstanceExtensions() const noexcept = 0;
  virtual VkResult createVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator,
                                       VkSurfaceKHR* surface) const noexcept = 0;

  [[nodiscard]] virtual Vec2 framebufferSize() const noexcept = 0;
  [[nodiscard]] virtual float contentScale() const noexcept = 0;
  [[nodiscard]] virtual bool visible() const noexcept = 0;
  virtual void requestRedraw() noexcept = 0;

  // Standalone windows may wait/pump until a minimized surface becomes usable.
  // Embedded hosts should return immediately because they do not own the loop.
  virtual void waitForVisibleFramebuffer() {}

protected:
  PlatformSurface() = default;
};

} // namespace slugia
