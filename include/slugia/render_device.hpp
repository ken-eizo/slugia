#pragma once

#include "slugia/vulkan_renderer.hpp"

#include <memory>

namespace slugia {

class VectorAtlas;
class PlatformSurface;
class VulkanInterop;
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
class Window;
#endif

// Public device/surface boundary. The device owns immutable rendering policy and
// atlas identity; each RenderSurface owns presentation state for one native surface.
class RenderSurface {
public:
  ~RenderSurface();
  RenderSurface(RenderSurface&&) noexcept;
  RenderSurface& operator=(RenderSurface&&) noexcept;
  RenderSurface(const RenderSurface&) = delete;
  RenderSurface& operator=(const RenderSurface&) = delete;

  void prepareFrame();
  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds, TextStyle style);
  std::size_t updateRetainedText(RetainedTextId text, std::string_view utf8,
                                 Rect layoutBounds, TextStyle style);
  void destroyRetainedText(RetainedTextId text);
  RetainedDrawListId createRetainedDrawList(const DrawList& list);
  std::size_t updateRetainedDrawList(RetainedDrawListId drawList, const DrawList& list);
  void destroyRetainedDrawList(RetainedDrawListId drawList);
  void draw(const DrawList& list);
  [[nodiscard]] FramePixels drawAndReadback(const DrawList& list);
  void waitIdle();
  [[nodiscard]] RendererStats stats() const;
  [[nodiscard]] const char* deviceName() const;
  [[nodiscard]] const char* presentModeName() const;
  [[nodiscard]] VulkanInterop& vulkanInterop() noexcept;

private:
  friend class RenderDevice;
  explicit RenderSurface(std::unique_ptr<VulkanRenderer> renderer);
  std::unique_ptr<VulkanRenderer> renderer_;
};

class RenderDevice {
public:
  explicit RenderDevice(const VectorAtlas& atlas, RendererConfig config = {});

  [[nodiscard]] RenderSurface createSurface(PlatformSurface& surface) const;
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
  [[nodiscard]] RenderSurface createSurface(Window& window) const;
#endif
  [[nodiscard]] const VectorAtlas& atlas() const noexcept { return *atlas_; }
  [[nodiscard]] const RendererConfig& config() const noexcept { return config_; }

private:
  const VectorAtlas* atlas_ = nullptr;
  RendererConfig config_{};
};

} // namespace slugia
