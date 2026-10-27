#include "slugia/render_device.hpp"

#include "slugia/platform_surface.hpp"
#include "slugia/vulkan_interop.hpp"
#include "slugia/vector_atlas.hpp"
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
#include "slugia/window.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace slugia {

RenderSurface::RenderSurface(std::unique_ptr<VulkanRenderer> renderer)
    : renderer_(std::move(renderer)) {
  if (!renderer_) throw std::invalid_argument("RenderSurface requires a renderer");
}
RenderSurface::~RenderSurface() = default;
RenderSurface::RenderSurface(RenderSurface&&) noexcept = default;
RenderSurface& RenderSurface::operator=(RenderSurface&&) noexcept = default;

void RenderSurface::prepareFrame() { renderer_->prepareFrame(); }
RetainedTextId RenderSurface::createRetainedText(std::string_view utf8, Rect bounds,
                                                 TextStyle style) {
  return renderer_->createRetainedText(utf8, bounds, std::move(style));
}
std::size_t RenderSurface::updateRetainedText(RetainedTextId text, std::string_view utf8,
                                              Rect bounds, TextStyle style) {
  return renderer_->updateRetainedText(text, utf8, bounds, std::move(style));
}
void RenderSurface::destroyRetainedText(RetainedTextId text) {
  renderer_->destroyRetainedText(text);
}
RetainedDrawListId RenderSurface::createRetainedDrawList(const DrawList& list) {
  return renderer_->createRetainedDrawList(list);
}
std::size_t RenderSurface::updateRetainedDrawList(RetainedDrawListId id,
                                                  const DrawList& list) {
  return renderer_->updateRetainedDrawList(id, list);
}
void RenderSurface::destroyRetainedDrawList(RetainedDrawListId id) {
  renderer_->destroyRetainedDrawList(id);
}
void RenderSurface::draw(const DrawList& list) { renderer_->draw(list); }
FramePixels RenderSurface::drawAndReadback(const DrawList& list) {
  return renderer_->drawAndReadback(list);
}
void RenderSurface::waitIdle() { renderer_->waitIdle(); }
RendererStats RenderSurface::stats() const { return renderer_->stats(); }
const char* RenderSurface::deviceName() const { return renderer_->deviceName(); }
const char* RenderSurface::presentModeName() const { return renderer_->presentModeName(); }
VulkanInterop& RenderSurface::vulkanInterop() noexcept { return renderer_->vulkanInterop(); }

RenderDevice::RenderDevice(const VectorAtlas& atlas, RendererConfig config)
    : atlas_(&atlas), config_(config) {}

RenderSurface RenderDevice::createSurface(PlatformSurface& surface) const {
  return RenderSurface(std::make_unique<VulkanRenderer>(surface, *atlas_, config_));
}
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
RenderSurface RenderDevice::createSurface(Window& window) const {
  return RenderSurface(std::make_unique<VulkanRenderer>(window, *atlas_, config_));
}
#endif

} // namespace slugia
