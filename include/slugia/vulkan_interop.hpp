#pragma once

#include "slugia/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

namespace slugia {

class VulkanRenderer;

struct VulkanDeviceContext {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  std::uint32_t graphicsQueueFamily = 0;
};

struct VulkanFrameContext {
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  std::uint32_t framebufferWidth = 0;
  std::uint32_t framebufferHeight = 0;
  // Monotonic serial of the renderer submission being recorded.
  std::uint64_t submissionSerial = 0;
};

using VulkanFrameRecorder = void (*)(void* context,
                                     const VulkanFrameContext& frame) noexcept;
// Explicit opt-in bridge for hosts that need zero-copy Vulkan compute or transfer work.
// Ordinary slugia UI code does not need this header and remains backend-opaque.
class VulkanInterop final {
public:
  [[nodiscard]] VulkanDeviceContext deviceContext() const noexcept;
  // Highest submission known complete without blocking the caller.
  [[nodiscard]] std::uint64_t completedSubmissionSerial() const noexcept;

  // The buffer is owned by the caller and must come from deviceContext().device.
  // Pixel layout is tightly packed float32 A,R,G,B words. IDs are renderer-local.
  [[nodiscard]] ExternalImageId registerRgba32fBuffer(VkBuffer buffer,
                                                       VkDeviceSize offset,
                                                       VkDeviceSize range);

  // UI-preview path for CPU snapshots. slugia owns a persistently mapped,
  // host-coherent storage buffer so updates are a memcpy and do not recreate
  // descriptors. Pixel words use the renderer's external-image layout:
  // tightly packed float32 A,R,G,B.
  [[nodiscard]] ExternalImageId createOwnedRgba32fImage(
      std::uint32_t width, std::uint32_t height);
  [[nodiscard]] bool updateOwnedRgba32fImage(
      ExternalImageId image,
      std::span<const float> argb_words) noexcept;

  void unregisterExternalImage(ExternalImageId image) noexcept;

  // Recorder runs in the renderer's command buffer immediately before the UI render pass.
  // It must not submit, wait, throw, or retain the command buffer.
  void setFrameRecorder(VulkanFrameRecorder recorder, void* context) noexcept;

private:
  friend class VulkanRenderer;
  explicit VulkanInterop(VulkanRenderer& renderer) noexcept : renderer_(&renderer) {}
  VulkanRenderer* renderer_ = nullptr;
};

} // namespace slugia
