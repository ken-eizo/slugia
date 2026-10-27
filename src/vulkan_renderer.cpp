#include "slugia/vulkan_renderer.hpp"
#include "detail/atlas_band_pack.hpp"
#include "slugia/vulkan_interop.hpp"

#include "slughorn/slughorn.hpp"
#include "slughorn/render.hpp"
#include "slugia/platform_surface.hpp"
#include "slugia/vector_atlas.hpp"
#include "slugia/vector_path.hpp"
#include "slugia_embedded_shaders.hpp"
#include "detail/geometry_cache.hpp"
#include "detail/viewport_clip.hpp"
#include "detail/path_tiles.hpp"

#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
#include "slugia/window.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace slugia {

namespace {

// UI latency is the primary policy: do not let the CPU build a frame behind another queued frame.
constexpr std::size_t framesInFlight = 1;
class OptionalCpuPhaseTimer {
public:
  OptionalCpuPhaseTimer(bool enabled, double& destination)
      : enabled_(enabled), destination_(destination) {
    if (enabled_) start_ = std::chrono::steady_clock::now();
  }
  ~OptionalCpuPhaseTimer() {
    if (enabled_)
      destination_ += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start_).count();
  }
  OptionalCpuPhaseTimer(const OptionalCpuPhaseTimer&) = delete;
private:
  bool enabled_;
  double& destination_;
  std::chrono::steady_clock::time_point start_{};
};
constexpr const char* portabilitySubsetExtension = "VK_KHR_portability_subset";
constexpr std::uint32_t externalImageShape = std::numeric_limits<std::uint32_t>::max() - 3U;

bool outsideClip(Rect bounds, Rect clip, float padding = 0.5f) {
  return clip.width <= 0.0f || clip.height <= 0.0f ||
    bounds.x + bounds.width + padding < clip.x || bounds.x - padding > clip.x + clip.width ||
    bounds.y + bounds.height + padding < clip.y || bounds.y - padding > clip.y + clip.height;
}

#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
class GlfwPlatformSurface final : public PlatformSurface {
public:
  explicit GlfwPlatformSurface(Window& window) : window_(window) {
    std::uint32_t count = 0;
    const char** required = glfwGetRequiredInstanceExtensions(&count);
    if (!required || count == 0)
      throw std::runtime_error("GLFW returned no Vulkan extensions");
    extensions_.assign(required, required + count);
  }

  [[nodiscard]] std::span<const char* const> requiredInstanceExtensions() const noexcept override {
    return extensions_;
  }

  VkResult createVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator,
                               VkSurfaceKHR* surface) const noexcept override {
    return glfwCreateWindowSurface(instance, window_.native(), allocator, surface);
  }

  [[nodiscard]] Vec2 framebufferSize() const noexcept override {
    return window_.framebufferSize();
  }

  [[nodiscard]] float contentScale() const noexcept override {
    return window_.contentScale();
  }

  [[nodiscard]] bool visible() const noexcept override {
    const Vec2 size = window_.framebufferSize();
    return size.x > 0.0f && size.y > 0.0f;
  }

  void requestRedraw() noexcept override {
    glfwPostEmptyEvent();
  }

  void waitForVisibleFramebuffer() override {
    window_.waitForVisibleFramebuffer();
  }

private:
  Window& window_;
  std::vector<const char*> extensions_;
};
#endif
constexpr float roundedRectFillCoverage = 0.0f;
constexpr float roundedRectStrokeRingCoverage = 1.0f;
constexpr float roundedRectOpaqueBorderCoverage = 2.0f;
constexpr float roundedRectInnerFillCoverage = -1.0f;
constexpr float paintAlphaEpsilon = 1.0f / 65535.0f;

std::uint32_t packFixed16(float first, float second, float scale, float maximum) {
  const auto quantize = [scale, maximum](float value) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, maximum) * scale));
  };
  return quantize(first) | (quantize(second) << 16U);
}

bool isSrgbFormat(VkFormat format) {
  return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

float srgbToLinear(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return value <= 0.04045f ? value / 12.92f :
    std::pow((value + 0.055f) / 1.055f, 2.4f);
}

bool paintFullyOpaque(const Paint& paint) {
  return paint.opacity * std::min(paint.start.a, paint.end.a) >= 1.0f - paintAlphaEpsilon;
}

bool paintFullyTransparent(const Paint& paint) {
  return paint.opacity <= paintAlphaEpsilon ||
    std::max(paint.start.a, paint.end.a) <= paintAlphaEpsilon;
}

BorderWidths resolvedBorderWidths(const BorderStyle& border) {
  BorderWidths widths = border.resolvedWidths();
  widths.top = std::max(widths.top, 0.0f);
  widths.right = std::max(widths.right, 0.0f);
  widths.bottom = std::max(widths.bottom, 0.0f);
  widths.left = std::max(widths.left, 0.0f);
  return widths;
}

float strokeOutsideFactor(StrokeAlign align) {
  if (align == StrokeAlign::Center) return 0.5f;
  if (align == StrokeAlign::Outside) return 1.0f;
  return 0.0f;
}

Rect strokeBounds(Rect destination, BorderWidths widths, float outsideFactor) {
  const float outsideTop = widths.top * outsideFactor;
  const float outsideRight = widths.right * outsideFactor;
  const float outsideBottom = widths.bottom * outsideFactor;
  const float outsideLeft = widths.left * outsideFactor;
  return {
    destination.x - outsideLeft,
    destination.y - outsideTop,
    destination.width + outsideLeft + outsideRight,
    destination.height + outsideTop + outsideBottom
  };
}

bool sameRoundedRectGeometry(const RoundedRectCommand& first, const RoundedRectCommand& second) {
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  const auto sameRect = [&close](Rect a, Rect b) {
    return close(a.x, b.x) && close(a.y, b.y) &&
      close(a.width, b.width) && close(a.height, b.height);
  };
  return sameRect(first.destination, second.destination) && sameRect(first.clip, second.clip) &&
    close(first.radiiPx.topLeft, second.radiiPx.topLeft) &&
    close(first.radiiPx.topRight, second.radiiPx.topRight) &&
    close(first.radiiPx.bottomRight, second.radiiPx.bottomRight) &&
    close(first.radiiPx.bottomLeft, second.radiiPx.bottomLeft) &&
    close(first.continuousCorners.topLeftPercent,
          second.continuousCorners.topLeftPercent) &&
    close(first.continuousCorners.topRightPercent,
          second.continuousCorners.topRightPercent) &&
    close(first.continuousCorners.bottomRightPercent,
          second.continuousCorners.bottomRightPercent) &&
    close(first.continuousCorners.bottomLeftPercent,
          second.continuousCorners.bottomLeftPercent) &&
    first.border.align == second.border.align;
}

Rect intersectRects(Rect first, Rect second) {
  const float left = std::max(first.x, second.x);
  const float top = std::max(first.y, second.y);
  const float right = std::min(first.x + first.width, second.x + second.width);
  const float bottom = std::min(first.y + first.height, second.y + second.height);
  return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

bool sameSolidPaint(const Paint& first, const Paint& second) {
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  return first.kind == GradientKind::Solid && second.kind == GradientKind::Solid &&
    close(first.start.r, second.start.r) && close(first.start.g, second.start.g) &&
    close(first.start.b, second.start.b) && close(first.start.a, second.start.a) &&
    close(first.opacity, second.opacity);
}

bool opaqueInsideBorderCovers(const RoundedRectCommand& earlier,
                              const RoundedRectCommand& later) {
  if (earlier.border.align != StrokeAlign::Inside ||
      later.border.align != StrokeAlign::Inside ||
      !paintFullyOpaque(earlier.border.paint) ||
      !sameSolidPaint(earlier.border.paint, later.border.paint)) return false;
  const BorderWidths first = resolvedBorderWidths(earlier.border);
  const BorderWidths second = resolvedBorderWidths(later.border);
  const auto close = [](float a, float b) { return std::abs(a - b) <= 0.0001f; };
  const auto greaterOrEqual = [&close](float a, float b) { return a > b || close(a, b); };
  const float firstRight = earlier.destination.x + earlier.destination.width;
  const float firstBottom = earlier.destination.y + earlier.destination.height;
  const float secondRight = later.destination.x + later.destination.width;
  const float secondBottom = later.destination.y + later.destination.height;

  if (second.top > 0.0f &&
      (!greaterOrEqual(first.top, second.top) ||
       !close(earlier.destination.x, later.destination.x) ||
       !close(firstRight, secondRight) ||
       !close(earlier.destination.y, later.destination.y) ||
       !close(earlier.radiiPx.topLeft, later.radiiPx.topLeft) ||
       !close(earlier.radiiPx.topRight, later.radiiPx.topRight))) return false;
  if (second.bottom > 0.0f &&
      (!greaterOrEqual(first.bottom, second.bottom) ||
       !close(earlier.destination.x, later.destination.x) ||
       !close(firstRight, secondRight) || !close(firstBottom, secondBottom) ||
       !close(earlier.radiiPx.bottomLeft, later.radiiPx.bottomLeft) ||
       !close(earlier.radiiPx.bottomRight, later.radiiPx.bottomRight))) return false;
  if (second.left > 0.0f) {
    if (!greaterOrEqual(first.left, second.left) ||
        !close(earlier.destination.x, later.destination.x) ||
        earlier.destination.y > later.destination.y + 0.0001f ||
        firstBottom + 0.0001f < secondBottom) return false;
    if (close(earlier.destination.y, later.destination.y)) {
      if (!close(earlier.radiiPx.topLeft, later.radiiPx.topLeft)) return false;
    } else if (later.radiiPx.topLeft > 0.0001f) return false;
    if (close(firstBottom, secondBottom)) {
      if (!close(earlier.radiiPx.bottomLeft, later.radiiPx.bottomLeft)) return false;
    } else if (later.radiiPx.bottomLeft > 0.0001f) return false;
  }
  if (second.right > 0.0f) {
    if (!greaterOrEqual(first.right, second.right) || !close(firstRight, secondRight) ||
        earlier.destination.y > later.destination.y + 0.0001f ||
        firstBottom + 0.0001f < secondBottom) return false;
    if (close(earlier.destination.y, later.destination.y)) {
      if (!close(earlier.radiiPx.topRight, later.radiiPx.topRight)) return false;
    } else if (later.radiiPx.topRight > 0.0001f) return false;
    if (close(firstBottom, secondBottom)) {
      if (!close(earlier.radiiPx.bottomRight, later.radiiPx.bottomRight)) return false;
    } else if (later.radiiPx.bottomRight > 0.0001f) return false;
  }
  return second.maximum() > 0.0f;
}

void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* userData) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && userData)
    static_cast<std::atomic<std::uint32_t>*>(userData)->fetch_add(1, std::memory_order_relaxed);
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    std::cerr << "[Vulkan] " << (data && data->pMessage ? data->pMessage : "validation message")
              << '\n';
  return VK_FALSE;
}

bool hasInstanceExtension(const char* name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool hasLayer(const char* name) {
  std::uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::any_of(layers.begin(), layers.end(),
                     [name](const auto& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

struct QueueFamilies {
  std::optional<std::uint32_t> graphics;
  std::optional<std::uint32_t> present;
  [[nodiscard]] bool complete() const {
    return graphics.has_value() && present.has_value();
  }
};

QueueFamilies findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
  QueueFamilies result;
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      result.graphics = i;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
    if (present)
      result.present = i;
    if (result.complete())
      break;
  }
  return result;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

SwapchainSupport querySwapchain(VkPhysicalDevice device, VkSurfaceKHR surface) {
  SwapchainSupport result;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &result.capabilities);
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
  result.formats.resize(count);
  if (count)
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, result.formats.data());
  count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
  result.presentModes.resize(count);
  if (count)
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, result.presentModes.data());
  return result;
}

struct Instance {
  float positionRect[4];
  float emRect[4];
  float bandTransform[4];
  std::uint32_t shapeData[4];
  float color0[4];
  float color1[4];
  float paint[4];
  float gradient[4];
  float clip[4];
  float strokeWidths[4];
};
constexpr VkDeviceSize retainedUploadStagingLimit = 4U << 20U;
struct RetainedUploadRange {
  std::size_t first = 0, count = 0;
};

struct DynamicSlugCurve {
  float p12[4];
  float p3[4];
};
// Refined atlas blocks store log2(cells / 32) in bits 26..28. The original
// 8-bit band maxima remain valid for unrefined dynamic blocks.
std::uint32_t dynamicSlugQuerySize(std::uint32_t packed) {
  return (packed & 0x20000000U) != 0 ? 32U << ((packed >> 26U) & 7U) : 32U;
}
static_assert(sizeof(DynamicSlugCurve) == sizeof(float) * 8);
static_assert(offsetof(DynamicSlugCurve, p3) == sizeof(float) * 4);

struct PrimitiveInstance {
  float positionRect[4];
  std::uint16_t color[4];
  std::uint16_t clip[4]; // normalized minX, minY, maxX, maxY
};
static_assert(sizeof(PrimitiveInstance) == 32);

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
  VkDeviceSize allocatedBytes = 0;
};

struct DrawBatch {
  std::uint32_t retainedId = 0;
  bool retainedDrawList = false;
  std::uint32_t firstInstance = 0;
  std::uint32_t instanceCount = 0;
  Vec2 translation = {};
  float scale = 1.0f;
  Rect clip = {};
  float opacity = 1.0f;
  bool primitive = false;
  ExternalImageId externalImage = 0;
  bool projected = false;
  ProjectiveTransform2D projective{};
  Rect projectiveClip{};
  bool additiveCoverage = false;
};

// Private lowering target between public DrawList commands and GPU buffers.
// It intentionally separates compact primitives from the full Slug/vector instance stream.
struct RenderIR {
  std::vector<Instance> instances{};
  std::vector<PrimitiveInstance> primitives{};
  std::vector<DynamicSlugCurve> dynamicCurves{};
  std::vector<std::uint32_t> dynamicBandWords{};
  std::vector<DrawBatch> batches{};

  void clear() {
    instances.clear();
    primitives.clear();
    dynamicCurves.clear();
    dynamicBandWords.clear();
    batches.clear();
  }
};

struct alignas(16) PushConstants {
  float viewportScale[4];
  float translationOverride[4];
  float overrideClip[4];
  float opacity[4];
  float projectiveRows[3][4];
  float projectiveClip[4];
};
static_assert(sizeof(PushConstants) == 128);

struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

} // namespace

struct VulkanRenderer::Impl {
  std::unique_ptr<PlatformSurface> ownedPlatformSurface;
  PlatformSurface& platformSurface;
  const VectorAtlas& vectorAtlas;
  RendererConfig config;
  RendererStats statistics{};
  std::atomic<std::uint32_t> validationErrors{0};
  Buffer readback{};
  bool captureFrame = false;
  FramePixels captured{};

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties deviceProperties{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;
  QueueFamilies queueFamilies{};

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
  VkExtent2D extent{};
  std::vector<VkImage> swapchainImages;
  std::vector<VkImageView> swapchainViews;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkSemaphore> renderFinished;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipeline primitivePipeline = VK_NULL_HANDLE;
  VkPipeline additivePipeline = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  Texture curveTexture{};
  Texture bandTexture{};
  Buffer externalImageFallback{};
  Buffer persistentSlugCurves{};
  Buffer persistentSlugBands{};
  Buffer vectorQuadIndices{};
  mutable std::uint32_t persistentSlugCurveCount = 0;
  mutable std::uint32_t persistentSlugBandWordCount = 0;
  mutable std::size_t persistentSlugUploadedBytes = 0;
  std::unordered_map<std::uint64_t,std::array<std::uint32_t,4>> atlasStorageShapes;
  std::uint32_t atlasRefinedShapeCount = 0;
  std::uint32_t atlasPackedBandShapeCount = 0;
  std::array<std::uint32_t, 7> atlasRefinedShapesByCells{};
  std::size_t atlasStorageCurveBytes = 0,atlasStorageBandBytes = 0;
  std::size_t retainedUploadStagingPeakBytes = 0;
  struct ExternalImageResource {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    Buffer ownedBuffer{};
    VkDeviceSize range = 0;
    bool live = false;
  };
  std::vector<ExternalImageResource> externalImages{};
  VulkanFrameRecorder frameRecorder = nullptr;
  void* frameRecorderContext = nullptr;

  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, framesInFlight> commandBuffers{};
  struct Frame {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timestampsWritten = false;
    bool writeTimestamps = false;
    std::uint64_t timestampSubmissionSerial = 0;
    Buffer instances{};
    Buffer primitiveInstances{};
    Buffer dynamicCurves{};
    Buffer dynamicBands{};
  };
  std::array<Frame, framesInFlight> frames{};
  std::size_t currentFrame = 0;
  RenderIR frameIR{};
  struct GlyphRunKey {
    std::string text;
    std::string fontName;
    std::string fontStyle;
    std::uint16_t weight = 400;
    bool italic = false;
    bool operator==(const GlyphRunKey&) const = default;
  };
  struct GlyphRunKeyHash {
    std::size_t operator()(const GlyphRunKey& key) const noexcept {
      std::size_t hash = std::hash<std::string>{}(key.text);
      const auto combine = [&hash](std::size_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
      };
      combine(std::hash<std::string>{}(key.fontName));
      combine(std::hash<std::string>{}(key.fontStyle));
      combine(key.weight);
      combine(key.italic ? 1U : 0U);
      return hash;
    }
  };
  struct RetainedGeometry {
    VkDeviceSize offset = 0;
    VkDeviceSize capacity = 0;
    std::vector<Instance> shadow{};
    std::array<double,4> viewBounds{};
    std::uint32_t instanceCount = 0;
    Buffer dynamicCurves{};
    Buffer dynamicBands{};
    VkDescriptorPool dynamicDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet dynamicDescriptorSet = VK_NULL_HANDLE;
    std::vector<DrawBatch> batches;
  };
  struct FreeRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
  };
  Buffer retainedArena{};
  VkDeviceSize retainedArenaUsed = 0;
  std::vector<FreeRange> retainedFreeRanges{};
  std::vector<RetainedGeometry> retainedTexts{};
  std::vector<RetainedGeometry> retainedDrawLists{};
  std::unordered_map<std::uint64_t, const slughorn::Atlas::Shape*> viewOutlines;
  std::unordered_map<std::uint64_t,std::vector<detail::PathTile>> viewTiles;
  std::size_t viewTileBytes=0;
  struct CachedGlyph {
    bool present = false;
    std::uint32_t codepoint = 0;
    slughorn::Atlas::Shape shape{};
    float xAdvance = 0.0f;
    float yAdvance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
  };
  struct CachedGlyphRun {
    std::vector<CachedGlyph> glyphs{};
    float xAdvance = 0.0f;
    float yAdvance = 0.0f;
    bool shaped = false;
    bool rightToLeft = false;
    std::uint64_t lastUsed = 0;
  };
  struct CompiledDynamicSlugShape {
    std::vector<DynamicSlugCurve> curves{};
    std::vector<std::uint32_t> bandWords{};
    std::uint32_t bandMaxX = 0;
    std::uint32_t bandMaxY = 0;
    std::uint32_t indirectionSize = 32;
    bool packedBandIndices = false;
    float width = 0.0f;
    float height = 0.0f;
    std::uint32_t persistentCurveBase = 0;
    std::uint32_t persistentBandBase = 0;
    bool persistentResident = false;
    std::uint32_t useCount = 0;
    std::uint64_t cacheId = 0;
  };
  mutable detail::GeometryCache<CompiledDynamicSlugShape> dynamicSlugPathCache;
  mutable detail::GeometryCache<std::vector<QuadraticBezier2D>> dynamicGeometryCache;
  mutable std::uint64_t dynamicSlugUseCounter = 0;
  mutable std::unordered_map<std::uint64_t,
                             std::pair<std::uint32_t,std::uint32_t>>
    frameDynamicShapeLocations{};
  mutable std::uint64_t dynamicSlugCacheHits = 0;
  mutable std::uint64_t dynamicSlugCacheMisses = 0;
  mutable std::unordered_map<GlyphRunKey, CachedGlyphRun, GlyphRunKeyHash> glyphRunCache{};
  mutable std::uint64_t glyphRunUseCounter = 0;
  std::uint32_t timestampValidBits = 0;
  std::uint64_t submittedFrameCount = 0;
  std::uint64_t completedSubmissionSerial = 0;
  std::array<std::uint64_t, framesInFlight> frameSubmissionSerial{};
  bool framePrepared = false;
  std::uint32_t preparedImageIndex = 0;
  VkResult preparedAcquireResult = VK_SUCCESS;

  Impl(PlatformSurface& inputSurface, const VectorAtlas& atlas, RendererConfig inputConfig)
      : platformSurface(inputSurface), vectorAtlas(atlas), config(inputConfig),
        dynamicSlugPathCache(config.dynamicPathCacheBytes, 2048),
        dynamicGeometryCache(config.dynamicGeometryCacheBytes, 1536) {
    initialize();
  }

  Impl(std::unique_ptr<PlatformSurface> inputSurface, const VectorAtlas& atlas,
       RendererConfig inputConfig)
      : ownedPlatformSurface(std::move(inputSurface)), platformSurface(*ownedPlatformSurface),
        vectorAtlas(atlas), config(inputConfig),
        dynamicSlugPathCache(config.dynamicPathCacheBytes, 2048),
        dynamicGeometryCache(config.dynamicGeometryCacheBytes, 1536) {
    initialize();
  }

  void initialize() {
    try {
      const auto cells = config.atlasBandRefinementCells;
      if (config.enableAtlasBandRefinement &&
          (cells < 32 || cells > 2048 || (cells & (cells - 1)) != 0))
        throw std::invalid_argument(
            "Atlas band cells must be a power of two from 32 through 2048");
      if (!vectorAtlas.built())
        throw std::invalid_argument("VectorAtlas::build() must be called first");
      createInstance();
      createSurface();
      pickPhysicalDevice();
      createDevice();
      createCommandPool();
      if (config.enableIndexedVectorQuads) {
        // The existing shader maps 0,3 to the same corner and 2,4 to the
        // same corner. Reuse those outputs without changing the shader or
        // either triangle's positions, winding, diagonal or instance order.
        constexpr std::uint16_t indices[]{0, 1, 2, 0, 2, 5};
        createBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     vectorQuadIndices, true);
        std::memcpy(vectorQuadIndices.mapped, indices, sizeof(indices));
      }
      createTextures();
      createBuffer(4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   externalImageFallback, true);
      std::memset(externalImageFallback.mapped, 0, 4 * sizeof(float));
      const VkDeviceSize persistentBytes = std::min<VkDeviceSize>(
        config.persistentGeometryBufferBytes, deviceProperties.limits.maxStorageBufferRange);
      createBuffer(std::max<VkDeviceSize>(sizeof(DynamicSlugCurve), persistentBytes),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   persistentSlugCurves, true);
      createBuffer(std::max<VkDeviceSize>(sizeof(std::uint32_t), persistentBytes),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   persistentSlugBands, true);
      prepareAtlasStoragePaths();
      createDescriptorResources();
      createSwapchainResources();
      createFrames();
    } catch (...) {
      destroy();
      throw;
    }
  }

  ~Impl() {
    destroy();
  }

  void createInstance() {
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
#ifndef SLUGIA_ENABLE_VALIDATION
    config.validation = false;
#endif
    config.validation = config.validation && hasLayer(validationLayer);

    const auto requiredExtensions = platformSurface.requiredInstanceExtensions();
    if (requiredExtensions.empty())
      throw std::runtime_error("PlatformSurface returned no Vulkan extensions");
    std::vector<const char*> extensions(requiredExtensions.begin(), requiredExtensions.end());
    if (config.validation && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateFlags flags = 0;
    if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "slugia";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "slugia";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = debugCallback;
    debugInfo.pUserData = &validationErrors;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.flags = flags;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (config.validation) {
      createInfo.enabledLayerCount = 1;
      createInfo.ppEnabledLayerNames = &validationLayer;
      createInfo.pNext = &debugInfo;
    }
    check(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");

    if (config.validation) {
      auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      if (create)
        check(create(instance, &debugInfo, nullptr, &debugMessenger),
              "vkCreateDebugUtilsMessengerEXT");
    }
  }

  void createSurface() {
    check(platformSurface.createVulkanSurface(instance, nullptr, &surface),
          "PlatformSurface::createVulkanSurface");
  }

  void pickPhysicalDevice() {
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (!count)
      throw std::runtime_error("No Vulkan physical device was found");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
      const auto families = findQueueFamilies(candidate, surface);
      if (!families.complete() || !hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        continue;
      const auto support = querySwapchain(candidate, surface);
      if (support.formats.empty() || support.presentModes.empty())
        continue;
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(candidate, &properties);
      // The fragment shader exposes external pixels plus two curve/band pairs.
      // Vulkan 1.1 alone guarantees only four per-stage storage descriptors.
      if (properties.apiVersion < VK_API_VERSION_1_1 ||
          properties.limits.maxPerStageDescriptorStorageBuffers < 5 ||
          properties.limits.maxFragmentCombinedOutputResources < 6)
        continue;
      int score = static_cast<int>(properties.limits.maxImageDimension2D);
      if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 100000;
      if (score > bestScore) {
        bestScore = score;
        physicalDevice = candidate;
        queueFamilies = families;
        deviceProperties = properties;
      }
    }
    if (physicalDevice == VK_NULL_HANDLE)
      throw std::runtime_error("No present-capable Vulkan 1.1 device supports VK_KHR_swapchain "
                               "and five fragment storage-buffer descriptors plus one color output");
  }

  void createDevice() {
    std::set<std::uint32_t> uniqueFamilies{*queueFamilies.graphics, *queueFamilies.present};
    std::vector<VkDeviceQueueCreateInfo> queues;
    const float priority = 1.0f;
    for (auto family : uniqueFamilies) {
      VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue.queueFamilyIndex = family;
      queue.queueCount = 1;
      queue.pQueuePriorities = &priority;
      queues.push_back(queue);
    }
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (hasDeviceExtension(physicalDevice, portabilitySubsetExtension))
      extensions.push_back(portabilitySubsetExtension);

    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
    createInfo.pQueueCreateInfos = queues.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.pEnabledFeatures = &features;
    check(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, *queueFamilies.graphics, 0, &graphicsQueue);
    vkGetDeviceQueue(device, *queueFamilies.present, 0, &presentQueue);
  }

  void createCommandPool() {
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = *queueFamilies.graphics;
    check(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
  }

  std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No compatible Vulkan memory type");
  }

  void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                    Buffer& output, bool map) {
    output.size = std::max<VkDeviceSize>(size, 4);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = output.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &bufferInfo, nullptr, &output.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, output.buffer, &requirements);
    output.allocatedBytes = requirements.size;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits, properties);
    check(vkAllocateMemory(device, &allocate, nullptr, &output.memory), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device, output.buffer, output.memory, 0), "vkBindBufferMemory");
    if (map)
      check(vkMapMemory(device, output.memory, 0, output.size, 0, &output.mapped), "vkMapMemory");
  }

  void destroyBuffer(Buffer& buffer) {
    if (buffer.mapped)
      vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer)
      vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory)
      vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
  }

  VkCommandBuffer beginSingleUse() {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &allocate, &command),
          "vkAllocateCommandBuffers(upload)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(upload)");
    return command;
  }

  void endSingleUse(VkCommandBuffer command) {
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer(upload)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(upload)");
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(upload)");
    vkFreeCommandBuffers(device, commandPool, 1, &command);
  }

  VkFormat atlasTextureFormat(slughorn::Atlas::TextureData::Format format) const {
    using Format = slughorn::Atlas::TextureData::Format;
    switch (format) {
      case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case Format::RGBA16UI: return VK_FORMAT_R16G16B16A16_UINT;
      case Format::RG16UI: return VK_FORMAT_R16G16_UINT;
      case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
      case Format::RGB32F: break;
    }
    throw std::runtime_error("Unsupported Slug atlas texture format");
  }

  void createTexture(const slughorn::Atlas::TextureData& source, Texture& output) {
    const VkFormat format = atlasTextureFormat(source.format);
    if (source.empty() || source.width == 0 || source.height == 0)
      throw std::runtime_error("Slug atlas returned an empty texture");
    Buffer staging{};
    createBuffer(source.bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, true);
    std::memcpy(staging.mapped, source.bytes.data(), source.bytes.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {source.width, source.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(device, &imageInfo, nullptr, &output.image), "vkCreateImage(atlas)");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, output.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &allocate, nullptr, &output.memory), "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, output.image, output.memory, 0), "vkBindImageMemory");

    VkCommandBuffer command = beginSingleUse();
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = output.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    vkCmdCopyBufferToImage(command, staging.buffer, output.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = output.image;
    toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toShader);
    endSingleUse(command);
    destroyBuffer(staging);

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = output.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view, nullptr, &output.view), "vkCreateImageView(atlas)");
  }

  void destroyTexture(Texture& texture) {
    if (texture.view)
      vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image)
      vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory)
      vkFreeMemory(device, texture.memory, nullptr);
    texture = {};
  }

  void createTextures() {
    const auto& atlas = vectorAtlas.native();
    createTexture(atlas.getCurveTextureData(), curveTexture);
    createTexture(atlas.getBandTextureData(), bandTexture);
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = 0.0f;
    check(vkCreateSampler(device, &info, nullptr, &sampler), "vkCreateSampler");
  }

  void writeImageDescriptorSet(VkDescriptorSet target, VkBuffer buffer,
                               VkDeviceSize offset, VkDeviceSize range) {
    VkDescriptorImageInfo curve{sampler, curveTexture.view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo band{sampler, bandTexture.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo pixels{buffer, offset, range};
    VkDescriptorBufferInfo fallback{externalImageFallback.buffer,0,externalImageFallback.size};
    VkDescriptorBufferInfo persistentCurves{
      persistentSlugCurves.buffer,0,persistentSlugCurves.size};
    VkDescriptorBufferInfo persistentBands{
      persistentSlugBands.buffer,0,persistentSlugBands.size};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = target;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[0].pImageInfo = &curve;
    writes[1].pImageInfo = &band;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = target;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &pixels;
    for(std::uint32_t i=3;i<7;++i){
      writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet=target;
      writes[i].dstBinding=i;
      writes[i].descriptorCount=1;
      writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[3].pBufferInfo=&fallback;
    writes[4].pBufferInfo=&fallback;
    writes[5].pBufferInfo=&persistentCurves;
    writes[6].pBufferInfo=&persistentBands;
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }

  void writeDynamicDescriptorSet(VkDescriptorSet target,const Buffer& curves,
                                 const Buffer& bands) {
    const Buffer& c=curves.buffer!=VK_NULL_HANDLE?curves:externalImageFallback;
    const Buffer& b=bands.buffer!=VK_NULL_HANDLE?bands:externalImageFallback;
    VkDescriptorBufferInfo curveInfo{c.buffer,0,c.size};
    VkDescriptorBufferInfo bandInfo{b.buffer,0,b.size};
    std::array<VkWriteDescriptorSet,2> writes{};
    for(std::uint32_t i=0;i<2;++i){
      writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet=target;
      writes[i].dstBinding=3+i;
      writes[i].descriptorCount=1;
      writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo=&curveInfo;
    writes[1].pBufferInfo=&bandInfo;
    vkUpdateDescriptorSets(device,static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
  }

  static std::array<VkDescriptorPoolSize, 2> imageDescriptorPoolSizes() {
    return {{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}}};
  }

  void createDescriptorResources() {
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for(std::uint32_t i=2;i<7;++i){
      bindings[i].binding=i;
      bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount=1;
      bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptorSetLayout),
          "vkCreateDescriptorSetLayout");

    const auto sizes = imageDescriptorPoolSizes();
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptorSetLayout;
    check(vkAllocateDescriptorSets(device, &allocate, &descriptorSet),
          "vkAllocateDescriptorSets");
    writeImageDescriptorSet(descriptorSet, externalImageFallback.buffer, 0,
                            externalImageFallback.size);
  }

  ExternalImageId registerExternalImage(VkBuffer buffer, VkDeviceSize offset,
                                        VkDeviceSize range) {
    if (buffer == VK_NULL_HANDLE || range == 0)
      throw std::invalid_argument("External RGBA32F buffer must be valid and non-empty");
    ExternalImageResource resource;
    const auto sizes = imageDescriptorPoolSizes();
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device, &pool, nullptr, &resource.pool),
          "vkCreateDescriptorPool(external image)");
    try {
      VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = resource.pool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &descriptorSetLayout;
      check(vkAllocateDescriptorSets(device, &allocate, &resource.descriptorSet),
            "vkAllocateDescriptorSets(external image)");
      writeImageDescriptorSet(resource.descriptorSet, buffer, offset, range);
      resource.range = range;
      resource.live = true;
      externalImages.push_back(resource);
      return static_cast<ExternalImageId>(externalImages.size());
    } catch (...) {
      if (resource.pool) vkDestroyDescriptorPool(device, resource.pool, nullptr);
      throw;
    }
  }

  ExternalImageId createOwnedExternalImage(
      std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0)
      throw std::invalid_argument(
          "Owned RGBA32F image dimensions must be non-zero");
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * height;
    if (pixels >
        (std::numeric_limits<VkDeviceSize>::max)() /
            (4u * sizeof(float)))
      throw std::overflow_error(
          "Owned RGBA32F image is too large");

    ExternalImageResource resource;
    resource.range = static_cast<VkDeviceSize>(
        pixels * 4u * sizeof(float));
    createBuffer(
        resource.range,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        resource.ownedBuffer, true);
    std::memset(
        resource.ownedBuffer.mapped, 0,
        static_cast<std::size_t>(resource.range));

    const auto sizes = imageDescriptorPoolSizes();
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount =
        static_cast<std::uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(
        vkCreateDescriptorPool(
            device, &pool, nullptr, &resource.pool),
        "vkCreateDescriptorPool(owned external image)");
    try {
      VkDescriptorSetAllocateInfo allocate{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = resource.pool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &descriptorSetLayout;
      check(
          vkAllocateDescriptorSets(
              device, &allocate, &resource.descriptorSet),
          "vkAllocateDescriptorSets(owned external image)");
      writeImageDescriptorSet(
          resource.descriptorSet,
          resource.ownedBuffer.buffer, 0, resource.range);
      resource.live = true;
      externalImages.push_back(std::move(resource));
      return static_cast<ExternalImageId>(
          externalImages.size());
    } catch (...) {
      if (resource.pool)
        vkDestroyDescriptorPool(
            device, resource.pool, nullptr);
      destroyBuffer(resource.ownedBuffer);
      throw;
    }
  }

  bool updateOwnedExternalImage(
      ExternalImageId image,
      std::span<const float> words) noexcept {
    if (image == 0 || image > externalImages.size())
      return false;
    auto& resource = externalImages[image - 1U];
    if (!resource.live ||
        resource.ownedBuffer.mapped == nullptr)
      return false;
    const auto expected =
        static_cast<std::size_t>(
            resource.range / sizeof(float));
    if (words.size() != expected) return false;
    std::memcpy(
        resource.ownedBuffer.mapped, words.data(),
        static_cast<std::size_t>(resource.range));
    return true;
  }

  void unregisterExternalImage(ExternalImageId image) noexcept {
    if (image == 0 || image > externalImages.size()) return;
    auto& resource = externalImages[image - 1U];
    if (!resource.live) return;
    if (device != VK_NULL_HANDLE)
      (void)vkDeviceWaitIdle(device);
    if (resource.pool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(device, resource.pool, nullptr);
    destroyBuffer(resource.ownedBuffer);
    resource = {};
  }

  [[nodiscard]] const ExternalImageResource*
  externalImageResource(ExternalImageId image) const noexcept {
    if (image == 0 || image > externalImages.size()) return nullptr;
    const auto& resource = externalImages[image - 1U];
    return resource.live ? &resource : nullptr;
  }

  VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& format : formats)
      if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
    for (const auto& format : formats)
      if (format.format == VK_FORMAT_R8G8B8A8_SRGB &&
          format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return format;
    return formats.front();
  }

  VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    if (!config.vsync) {
      if (config.allowTearing)
        for (auto mode : modes)
          if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            return mode;
      for (auto mode : modes)
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
          return mode;
      for (auto mode : modes)
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
          return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  }

  VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
      return capabilities.currentExtent;
    const Vec2 framebuffer = platformSurface.framebufferSize();
    return {std::clamp(static_cast<std::uint32_t>(std::max(framebuffer.x, 1.0f)),
                       capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(std::max(framebuffer.y, 1.0f)),
                       capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
  }

  void createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapchainFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    check(vkCreateRenderPass(device, &info, nullptr, &renderPass), "vkCreateRenderPass");
  }

  VkShaderModule createShaderModule(const unsigned char* bytes, std::size_t byteCount) {
    if (byteCount % 4 != 0)
      throw std::runtime_error("Invalid SPIR-V byte length");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = byteCount;
    info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
  }

  void createPipeline() {
    VkShaderModule vertexModule =
        createShaderModule(embedded::vectorVert, embedded::vectorVertSize);
    VkShaderModule fragmentModule =
        createShaderModule(embedded::vectorFrag, embedded::vectorFragSize);
    VkShaderModule primitiveVertexModule =
        createShaderModule(embedded::primitiveVert, embedded::primitiveVertSize);
    VkShaderModule primitiveFragmentModule =
        createShaderModule(embedded::primitiveFrag, embedded::primitiveFragSize);
    VkPipelineShaderStageCreateInfo vertexStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexModule;
    vertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo fragmentStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentModule;
    fragmentStage.pName = "main";
    const std::array<std::uint32_t, 2> curveLayout{
        config.enableCompactPersistentCurves ? 3U : 4U,
        config.enablePackedAtlasBands ? 1U : 0U};
    const std::array<VkSpecializationMapEntry, 2> curveLayoutEntries{
        VkSpecializationMapEntry{0, 0, sizeof(std::uint32_t)},
        VkSpecializationMapEntry{1, sizeof(std::uint32_t), sizeof(std::uint32_t)}};
    const VkSpecializationInfo curveStrideInfo{
        static_cast<std::uint32_t>(curveLayoutEntries.size()), curveLayoutEntries.data(),
        sizeof(curveLayout), curveLayout.data()};
    fragmentStage.pSpecializationInfo = &curveStrideInfo;
    const std::array stages{vertexStage, fragmentStage};

    VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::array<VkVertexInputAttributeDescription, 10> attributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, positionRect)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, emRect)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, bandTransform)},
      {3, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(Instance, shapeData)},
      {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color0)},
      {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color1)},
      {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, paint)},
      {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, gradient)},
      {8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, clip)},
      {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, strokeWidths)}
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptorSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout),
          "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateGraphicsPipelines(vector)");
    attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
    attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
    attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&additivePipeline),
          "vkCreateGraphicsPipelines(coverage accumulation)");
    attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    VkPipelineShaderStageCreateInfo primitiveVertexStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    primitiveVertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    primitiveVertexStage.module = primitiveVertexModule;
    primitiveVertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo primitiveFragmentStage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    primitiveFragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    primitiveFragmentStage.module = primitiveFragmentModule;
    primitiveFragmentStage.pName = "main";
    const std::array primitiveStages{primitiveVertexStage, primitiveFragmentStage};
    VkVertexInputBindingDescription primitiveBinding{
      0, sizeof(PrimitiveInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    std::array<VkVertexInputAttributeDescription, 3> primitiveAttributes{{
      {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PrimitiveInstance, positionRect)},
      {1, 0, VK_FORMAT_R16G16B16A16_UNORM, offsetof(PrimitiveInstance, color)},
      {2, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(PrimitiveInstance, clip)}
    }};
    VkPipelineVertexInputStateCreateInfo primitiveVertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    primitiveVertexInput.vertexBindingDescriptionCount = 1;
    primitiveVertexInput.pVertexBindingDescriptions = &primitiveBinding;
    primitiveVertexInput.vertexAttributeDescriptionCount =
      static_cast<std::uint32_t>(primitiveAttributes.size());
    primitiveVertexInput.pVertexAttributeDescriptions = primitiveAttributes.data();
    pipelineInfo.pStages = primitiveStages.data();
    pipelineInfo.pVertexInputState = &primitiveVertexInput;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                    &primitivePipeline),
          "vkCreateGraphicsPipelines(primitive)");

    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);
    vkDestroyShaderModule(device, primitiveVertexModule, nullptr);
    vkDestroyShaderModule(device, primitiveFragmentModule, nullptr);
  }

  void createSwapchainResources(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE) {
    platformSurface.waitForVisibleFramebuffer();
    const auto support = querySwapchain(physicalDevice, surface);
    const auto chosenFormat = chooseFormat(support.formats);
    extent = chooseExtent(support.capabilities);
    swapchainPresentMode = choosePresentMode(support.presentModes);
    // Absolute-latency IMMEDIATE uses the smallest legal swapchain. MAILBOX keeps one additional
    // replacement image so the presentation engine can always retain only the newest frame.
    std::uint32_t imageCount = support.capabilities.minImageCount +
                               (swapchainPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? 1U : 0U);
    if (support.capabilities.maxImageCount && imageCount > support.capabilities.maxImageCount)
      imageCount = support.capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosenFormat.format;
    info.imageColorSpace = chosenFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (config.enableReadback) {
      if (!(support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Surface does not support readback");
      info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    const std::array families{*queueFamilies.graphics, *queueFamilies.present};
    if (queueFamilies.graphics != queueFamilies.present) {
      info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
      info.pQueueFamilyIndices = families.data();
    } else
      info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = support.capabilities.currentTransform;
    if (config.transparentSurface) {
      constexpr VkCompositeAlphaFlagsKHR preferred[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
      };
      bool found = false;
      for (const auto candidate : preferred) {
        if ((support.capabilities.supportedCompositeAlpha & candidate) != 0) {
          info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(candidate);
          found = true;
          break;
        }
      }
      if (!found)
        throw std::runtime_error("Surface does not support alpha compositing");
    } else {
      info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    info.presentMode = swapchainPresentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device, &info, nullptr, &newSwapchain), "vkCreateSwapchainKHR");
    swapchain = newSwapchain;
    if (oldSwapchain)
      vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    const bool formatChanged =
        swapchainFormat != VK_FORMAT_UNDEFINED && swapchainFormat != chosenFormat.format;
    if (formatChanged) {
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (primitivePipeline)
        vkDestroyPipeline(device, primitivePipeline, nullptr);
      if (additivePipeline)
        vkDestroyPipeline(device, additivePipeline, nullptr);
      if (pipelineLayout)
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      if (renderPass)
        vkDestroyRenderPass(device, renderPass, nullptr);
      pipeline = VK_NULL_HANDLE;
      primitivePipeline = VK_NULL_HANDLE;
      additivePipeline = VK_NULL_HANDLE;
      pipelineLayout = VK_NULL_HANDLE;
      renderPass = VK_NULL_HANDLE;
    }
    swapchainFormat = chosenFormat.format;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
    swapchainViews.resize(imageCount);
    for (std::size_t i = 0; i < imageCount; ++i) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = swapchainImages[i];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = swapchainFormat;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      check(vkCreateImageView(device, &view, nullptr, &swapchainViews[i]),
            "vkCreateImageView(swapchain)");
    }
    if (!renderPass) {
      createRenderPass();
      createPipeline();
    }
    framebuffers.resize(imageCount);
    for (std::size_t i = 0; i < imageCount; ++i) {
      VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer.renderPass = renderPass;
      framebuffer.attachmentCount = 1;
      framebuffer.pAttachments = &swapchainViews[i];
      framebuffer.width = extent.width;
      framebuffer.height = extent.height;
      framebuffer.layers = 1;
      check(vkCreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[i]),
            "vkCreateFramebuffer");
    }
    renderFinished.resize(imageCount);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& value : renderFinished)
      check(vkCreateSemaphore(device, &semaphore, nullptr, &value), "vkCreateSemaphore(present)");
  }

  void destroySwapchainImages(bool destroySwapchain) {
    for (auto semaphore : renderFinished)
      if (semaphore)
        vkDestroySemaphore(device, semaphore, nullptr);
    renderFinished.clear();
    for (auto framebuffer : framebuffers)
      vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (auto view : swapchainViews)
      vkDestroyImageView(device, view, nullptr);
    swapchainViews.clear();
    swapchainImages.clear();
    if (destroySwapchain && swapchain) {
      vkDestroySwapchainKHR(device, swapchain, nullptr);
      swapchain = VK_NULL_HANDLE;
    }
  }

  void destroySwapchainResources() {
    destroySwapchainImages(true);
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (primitivePipeline)
      vkDestroyPipeline(device, primitivePipeline, nullptr);
    if (additivePipeline)
      vkDestroyPipeline(device, additivePipeline, nullptr);
    if (pipelineLayout)
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (renderPass)
      vkDestroyRenderPass(device, renderPass, nullptr);
    pipeline = VK_NULL_HANDLE;
    primitivePipeline = VK_NULL_HANDLE;
    additivePipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
  }

  void recreateSwapchain() {
    platformSurface.waitForVisibleFramebuffer();
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(resize)");
    const VkSwapchainKHR oldSwapchain = swapchain;
    destroySwapchainImages(false);
    swapchain = VK_NULL_HANDLE;
    createSwapchainResources(oldSwapchain);
    framePrepared = false;
  }

  void createFrames() {
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = commandPool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = static_cast<std::uint32_t>(commandBuffers.size());
    check(vkAllocateCommandBuffers(device, &command, commandBuffers.data()),
          "vkAllocateCommandBuffers(frames)");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    std::uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queueProperties.data());
    timestampValidBits =
        queueFamilies.graphics ? queueProperties[*queueFamilies.graphics].timestampValidBits : 0;
    const bool timestampsSupported = config.gpuTimingInterval > 0 && timestampValidBits > 0 &&
                                     deviceProperties.limits.timestampPeriod > 0.0f;
    statistics.gpuTimingAvailable=timestampsSupported;
    const std::size_t initialCapacity = std::max<std::size_t>(1, config.initialVertexCapacity);
    for (auto& frame : frames) {
      check(vkCreateSemaphore(device, &semaphore, nullptr, &frame.imageAvailable),
            "vkCreateSemaphore(acquire)");
      check(vkCreateFence(device, &fence, nullptr, &frame.fence), "vkCreateFence");
      createBuffer(initialCapacity * sizeof(Instance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.instances, true);
      createBuffer(initialCapacity * sizeof(PrimitiveInstance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.primitiveInstances, true);
      createBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.dynamicCurves, true);
      createBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   frame.dynamicBands, true);
      if (timestampsSupported) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2;
        check(vkCreateQueryPool(device, &query, nullptr, &frame.timestamps),
              "vkCreateQueryPool(timestamp)");
      }
    }
  }

  void appendResolvedShape(std::vector<Instance>& instances, const slughorn::Atlas::Shape& shape,
                           Rect destination, const Paint& paintValue, Rect clip,
                           float italicShear = 0.0f) const {
    if (shape.width <= 0 || shape.height <= 0 || destination.width <= 0 || destination.height <= 0)
      return;
    // Store the true authored quad. The Slug reference renderer does not bake a constant
    // fringe into geometry; its vertex shader dynamically dilates the boundary by exactly
    // half a viewport pixel and offsets the em-space sample position through the inverse
    // Jacobian. Baking 1.25 px here was both wasteful at large scale and could still miss
    // samples after retained scaling.
    constexpr float rasterFringe = 0.5f;
    const float left = destination.x;
    const float right = destination.x + destination.width;
    const float top = destination.y;
    const float bottom = destination.y + destination.height;
    const float shear = italicShear * destination.height;
    if (right + std::max(shear, 0.0f) + rasterFringe <= clip.x ||
        left + std::min(shear, 0.0f) - rasterFringe >= clip.x + clip.width ||
        bottom + rasterFringe <= clip.y || top - rasterFringe >= clip.y + clip.height)
      return;

    Instance quad{};
    const float positionRect[4]{left, top, right, bottom};
    const float emRect[4]{static_cast<float>(shape.bearingX),
                          static_cast<float>(shape.bearingY),
                          static_cast<float>(shape.bearingX + shape.width),
                          static_cast<float>(shape.bearingY - shape.height)};
    std::copy(std::begin(positionRect), std::end(positionRect), quad.positionRect);
    std::copy(std::begin(emRect), std::end(emRect), quad.emRect);
    quad.bandTransform[0] = static_cast<float>(shape.bandScaleX);
    quad.bandTransform[1] = static_cast<float>(shape.bandScaleY);
    quad.bandTransform[2] = static_cast<float>(shape.bandOffsetX);
    quad.bandTransform[3] = static_cast<float>(shape.bandOffsetY);
    quad.shapeData[0] = shape.bandTexX;
    quad.shapeData[1] = shape.bandTexY;
    quad.shapeData[2] = shape.bandMaxX;
    // Match Eric Lengyel's reference glyph packing: low 8 bits are band max Y and bit 12 is E.
    quad.shapeData[3] = (shape.bandMaxY & 0xFFu) | (shape.evenOdd ? 0x1000u : 0u);
    if(config.enableAtlasStoragePaths) {
      const auto converted=atlasStorageShapes.find((std::uint64_t(shape.bandTexY)<<32)|shape.bandTexX);
      if(converted!=atlasStorageShapes.end()) {
        std::copy_n(converted->second.begin(),4,quad.shapeData);
        if ((quad.shapeData[3] & 0x20000000U) != 0) {
          const float scale = float(dynamicSlugQuerySize(quad.shapeData[3])) / 32.0f;
          for (auto &value : quad.bandTransform)
            value *= scale;
        }
      }
    }
    const float first[4]{paintValue.start.r, paintValue.start.g, paintValue.start.b,
                         paintValue.start.a};
    const float second[4]{paintValue.end.r, paintValue.end.g, paintValue.end.b, paintValue.end.a};
    std::copy(std::begin(first), std::end(first), quad.color0);
    std::copy(std::begin(second), std::end(second), quad.color1);
    quad.paint[0] = static_cast<float>(paintValue.kind);
    quad.paint[1] = paintValue.opacity;
    quad.paint[2] = paintValue.shaderParameter;
    quad.paint[3] = shear;
    quad.gradient[0] = paintValue.origin.x;
    quad.gradient[1] = paintValue.origin.y;
    quad.gradient[2] = paintValue.target.x;
    quad.gradient[3] = paintValue.target.y;
    quad.clip[0] = clip.x;
    quad.clip[1] = clip.y;
    quad.clip[2] = clip.width;
    quad.clip[3] = clip.height;
    instances.push_back(quad);
  }

  void appendShape(std::vector<Instance>& instances, ShapeId id, Rect destination,
                   const Paint& paintValue, Rect clip, float italicShear = 0.0f) const {
    const auto shape = vectorAtlas.native().getShape(slughorn::Key(id));
    if (shape)
      appendResolvedShape(instances, *shape, destination, paintValue, clip, italicShear);
  }

  static std::uint16_t packUnorm16(float value) noexcept {
    return static_cast<std::uint16_t>(std::lround(
      std::clamp(value, 0.0f, 1.0f) * 65535.0f));
  }

  [[nodiscard]] std::uint16_t packClipCoordinate(float value, std::uint32_t dimension) const noexcept {
    if (dimension == 0) return 0;
    const float normalized = std::clamp(value / static_cast<float>(dimension), 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
  }

  static bool integralPixel(float value) noexcept {
    return std::isfinite(value) && std::abs(value - std::nearbyint(value)) <= 0.0001f;
  }

  [[nodiscard]] bool compactRectangleCompatible(const RoundedRectCommand& command) const noexcept {
    const auto& color = command.paint.start;
    const auto widths = resolvedBorderWidths(command.border);
    const bool borderInvisible = widths.maximum() <= 0.0f || paintFullyTransparent(command.border.paint);
    const bool square = command.radiiPx.topLeft <= 0.0001f &&
      command.radiiPx.topRight <= 0.0001f && command.radiiPx.bottomRight <= 0.0001f &&
      command.radiiPx.bottomLeft <= 0.0001f;
    const bool unitColor = color.r >= 0.0f && color.r <= 1.0f &&
      color.g >= 0.0f && color.g <= 1.0f && color.b >= 0.0f && color.b <= 1.0f &&
      color.a >= 0.0f && color.a <= 1.0f && command.paint.opacity >= 0.0f &&
      command.paint.opacity <= 1.0f && command.opacity >= 0.0f && command.opacity <= 1.0f;
    const float right = command.destination.x + command.destination.width;
    const float bottom = command.destination.y + command.destination.height;
    return command.destination.width > 0.0f && command.destination.height > 0.0f &&
      command.paint.kind == GradientKind::Solid && borderInvisible && square && unitColor &&
      integralPixel(command.destination.x) && integralPixel(command.destination.y) &&
      integralPixel(right) && integralPixel(bottom);
  }

  void appendCompactRectangle(std::vector<PrimitiveInstance>& primitives,
                              const RoundedRectCommand& command) const {
    const float right = command.destination.x + command.destination.width;
    const float bottom = command.destination.y + command.destination.height;
    const float clipRight = command.clip.x + command.clip.width;
    const float clipBottom = command.clip.y + command.clip.height;
    if (right <= command.clip.x || command.destination.x >= clipRight ||
        bottom <= command.clip.y || command.destination.y >= clipBottom) return;
    PrimitiveInstance primitive{};
    primitive.positionRect[0] = command.destination.x;
    primitive.positionRect[1] = command.destination.y;
    primitive.positionRect[2] = right;
    primitive.positionRect[3] = bottom;
    const Color color = command.paint.start;
    primitive.color[0] = packUnorm16(color.r);
    primitive.color[1] = packUnorm16(color.g);
    primitive.color[2] = packUnorm16(color.b);
    primitive.color[3] =
        packUnorm16(color.a * command.paint.opacity * command.opacity);
    const float leftClip =
        std::clamp(command.clip.x, 0.0f, static_cast<float>(extent.width));
    const float topClip =
        std::clamp(command.clip.y, 0.0f, static_cast<float>(extent.height));
    const float rightClip =
        std::clamp(clipRight, leftClip, static_cast<float>(extent.width));
    const float bottomClip =
        std::clamp(clipBottom, topClip, static_cast<float>(extent.height));
    primitive.clip[0] = packClipCoordinate(leftClip, extent.width);
    primitive.clip[1] = packClipCoordinate(topClip, extent.height);
    primitive.clip[2] = packClipCoordinate(rightClip, extent.width);
    primitive.clip[3] = packClipCoordinate(bottomClip, extent.height);
    primitives.push_back(primitive);
  }

  bool refineAtlasStorageBands(CompiledDynamicSlugShape &compiled,
                               const slughorn::Atlas::Shape &shape,
                               VkDeviceSize extraBandBytes,
                               std::uint32_t cells) {
    const auto subdivision = cells / 32U;
    if (!config.enableAtlasBandRefinement || shape.curves.size() < 128 ||
        shape.bandScaleX == 0 || shape.bandScaleY == 0 ||
        !std::isfinite(float(shape.bandScaleX * subdivision)) ||
        !std::isfinite(float(shape.bandScaleY * subdivision)) ||
        !std::isfinite(float(shape.bandOffsetX * subdivision)) ||
        !std::isfinite(float(shape.bandOffsetY * subdivision)))
      return false;
    const auto &original = compiled.bandWords;
    const std::size_t maxWords =
        original.size() + std::size_t(extraBandBytes / sizeof(std::uint32_t));
    if (maxWords < 6 * cells)
      return false;
    std::vector<std::uint32_t> refined(6 * cells, 0);
    refined.reserve(maxWords);
    for (std::uint32_t q = 0; q < cells; ++q) {
      refined[q] = q;
      refined[cells + q] = q;
    }
    std::uint32_t header = 2 * cells;
    const auto refineAxis = [&](bool horizontal) {
      const double scale = subdivision * double(horizontal ? shape.bandScaleY
                                                           : shape.bandScaleX);
      const double offset =
          subdivision *
          double(horizontal ? shape.bandOffsetY : shape.bandOffsetX);
      for (std::uint32_t q = 0; q < cells; ++q) {
        double lo = (q - offset) / scale, hi = (q + 1.0 - offset) / scale;
        if (lo > hi)
          std::swap(lo, hi);
        if (q == 0)
          (scale > 0 ? lo : hi) = scale > 0
                                      ? -std::numeric_limits<double>::infinity()
                                      : std::numeric_limits<double>::infinity();
        if (q + 1 == cells)
          (scale > 0 ? hi : lo) =
              scale > 0 ? std::numeric_limits<double>::infinity()
                        : -std::numeric_limits<double>::infinity();
        const auto parent = original[(horizontal ? 0 : 32) + q / subdivision];
        const auto sourceHeader =
            64 + 2 * (parent + (horizontal ? 0 : shape.bandMaxY + 1));
        const auto count = original[sourceHeader],
                   sourceOffset = original[sourceHeader + 1];
        const auto start = refined.size();
        std::uint32_t kept = 0;
        for (std::uint32_t j = 0; j < count; ++j) {
          const auto id = detail::atlasBandCurveIndex(original.data(), sourceOffset, j, compiled.packedBandIndices);
          const auto &c = compiled.curves[id];
          const double a = c.p12[horizontal ? 1 : 0],
                       b = c.p12[horizontal ? 3 : 2],
                       d = c.p3[horizontal ? 1 : 0];
          const double low = std::min({a, b, d}), high = std::max({a, b, d});
          const double padding =
              64.0 * std::numeric_limits<float>::epsilon() *
              (std::max(std::abs(low), std::abs(high)) +
               std::abs(offset / scale) + std::abs(1.0 / scale));
          if (high >= lo - padding && low <= hi + padding) {
            if (!compiled.packedBandIndices || (kept & 1U) == 0) {
              if (refined.size() == maxWords) return false;
              refined.push_back(id);
            } else {
              refined.back() |= id << 16U;
            }
            ++kept;
          }
        }
        refined[header++] = kept;
        refined[header++] = std::uint32_t(start);
      }
      return true;
    };
    if (!refineAxis(true) || !refineAxis(false))
      return false;
    compiled.bandWords = std::move(refined);
    compiled.indirectionSize = cells;
    compiled.bandMaxX = cells - 1;
    compiled.bandMaxY = cells - 1;
    return true;
  }

  void prepareAtlasStoragePaths() {
    if (!config.enableAtlasStoragePaths ||
        config.persistentGeometryBufferBytes == 0)
      return;
    std::vector<const slughorn::Atlas::Shape *> candidates;
    for (const auto &[key, shape] : vectorAtlas.native().getShapes())
      if (!shape.curves.empty() && shape.width > 0 && shape.height > 0)
        candidates.push_back(&shape);
    std::sort(candidates.begin(), candidates.end(),
              [](const auto *a, const auto *b) {
                if (a->curves.size() != b->curves.size())
                  return a->curves.size() > b->curves.size();
                return std::pair{a->bandTexY, a->bandTexX} <
                       std::pair{b->bandTexY, b->bandTexX};
              });
    struct Prepared {
      const slughorn::Atlas::Shape *shape;
      CompiledDynamicSlugShape compiled;
    };
    std::vector<Prepared> prepared;
    const auto availableCurves =
        persistentSlugCurves.size -
        std::size_t(persistentSlugCurveCount) * persistentCurveStride();
    const auto availableBands =
        persistentSlugBands.size -
        std::size_t(persistentSlugBandWordCount) * sizeof(std::uint32_t);
    VkDeviceSize plannedCurveBytes = 0, plannedBandBytes = 0;
    for (const auto *shape : candidates) {
      if (prepared.size() >= 256)
        break;
      if (shape->curves.size() * persistentCurveStride() >
          availableCurves - plannedCurveBytes)
        continue;
      // Decode the packed representation, including RGBA16F rounding and
      // endpoint sharing. Never regenerate geometry or repartition the bands.
      const auto decoded = slughorn::render::decode(
          *shape, vectorAtlas.native().getCurveTextureData(),
          vectorAtlas.native().getBandTextureData());
      CompiledDynamicSlugShape compiled;
      compiled.bandMaxX = shape->bandMaxX;
      compiled.bandMaxY = shape->bandMaxY;
      compiled.curves.reserve(decoded.curves.size());
      for (const auto &q : decoded.curves) {
        DynamicSlugCurve c{};
        c.p12[0] = q.x1;
        c.p12[1] = q.y1;
        c.p12[2] = q.x2;
        c.p12[3] = q.y2;
        c.p3[0] = q.x3;
        c.p3[1] = q.y3;
        compiled.curves.push_back(c);
      }
      compiled.bandWords.resize(64 +
                                2 * (shape->bandMaxY + shape->bandMaxX + 2));
      for (std::size_t i = 0; i < 32; ++i) {
        compiled.bandWords[i] = decoded.indirY[i];
        compiled.bandWords[32 + i] = decoded.indirX[i];
      }
      std::uint32_t header = 64;
      const auto append = [&](const auto &offsets, const auto &indices) {
        for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
          compiled.bandWords[header] = offsets[i + 1] - offsets[i];
          compiled.bandWords[header + 1] =
              std::uint32_t(compiled.bandWords.size());
          header += 2;
          compiled.bandWords.insert(compiled.bandWords.end(),
                                    indices.begin() + offsets[i],
                                    indices.begin() + offsets[i + 1]);
        }
      };
      append(decoded.hbandOffsets, decoded.hbandIndices);
      append(decoded.vbandOffsets, decoded.vbandIndices);
      if (config.enablePackedAtlasBands)
        compiled.packedBandIndices = detail::packAtlasBandIndices(
            compiled.bandWords, 32, shape->bandMaxY + shape->bandMaxX + 2,
            compiled.curves.size());
      const auto curveBytes = compiled.curves.size() * persistentCurveStride();
      const auto bandBytes = compiled.bandWords.size() * sizeof(std::uint32_t);
      if (curveBytes > availableCurves - plannedCurveBytes ||
          bandBytes > availableBands - plannedBandBytes)
        continue;
      plannedCurveBytes += curveBytes;
      plannedBandBytes += bandBytes;
      prepared.push_back({shape, std::move(compiled)});
    }
    auto extraBandBytes = availableBands - plannedBandBytes;
    for (auto &entry : prepared) {
      const auto *shape = entry.shape;
      auto &compiled = entry.compiled;
      const auto before = compiled.bandWords.size() * sizeof(std::uint32_t);
      // Reserve every admitted original path first. Refinement only spends
      // spare bytes, so it cannot evict a path from a small storage budget.
      bool refined = false;
      for (auto cells = config.atlasBandRefinementCells;; cells >>= 1U) {
        refined = refineAtlasStorageBands(compiled, *shape, extraBandBytes, cells);
        if (refined || cells <= 512) break;
      }
      const auto after = compiled.bandWords.size() * sizeof(std::uint32_t);
      if (after >= before)
        extraBandBytes -= after - before;
      else
        extraBandBytes += before - after;
      if (!ensurePersistentSlugResident(compiled))
        continue;
      if (refined)
        ++atlasRefinedShapeCount;
      if (compiled.packedBandIndices)
        ++atlasPackedBandShapeCount;
      atlasStorageCurveBytes +=
          compiled.curves.size() * persistentCurveStride();
      atlasStorageBandBytes +=
          compiled.bandWords.size() * sizeof(std::uint32_t);
      std::uint32_t bandQueryShift = 0;
      for (auto cells = compiled.indirectionSize; cells > 32; cells >>= 1)
        ++bandQueryShift;
      if (refined)
        ++atlasRefinedShapesByCells.at(bandQueryShift);
      atlasStorageShapes.emplace(
          (std::uint64_t(shape->bandTexY) << 32) | shape->bandTexX,
          std::array<std::uint32_t, 4>{
              0xFFFFFFF8U, compiled.persistentCurveBase,
              compiled.persistentBandBase,
              0x80000000U | (refined ? 0x20000000U : 0U) |
                  (compiled.packedBandIndices ? detail::packedAtlasBandFlag : 0U) |
                  (bandQueryShift << 26U) |
                  (compiled.bandMaxX & 255U) |
                  ((compiled.bandMaxY & 255U) << 8U) |
                  (shape->evenOdd ? 0x10000U : 0U)});
      // This constructor-only staging is not a cache. In particular a refined
      // vector may reserve the remaining GPU budget; do not retain every
      // previously uploaded vector's capacity until the entire atlas finishes.
      std::vector<DynamicSlugCurve>().swap(compiled.curves);
      std::vector<std::uint32_t>().swap(compiled.bandWords);
    }
  }

  std::size_t persistentCurveStride() const {
    return config.enableCompactPersistentCurves ? 6 * sizeof(float) : sizeof(DynamicSlugCurve);
  }

  bool ensurePersistentSlugResident(CompiledDynamicSlugShape& shape) const {
    if (shape.persistentResident) return true;
    if (config.persistentGeometryBufferBytes == 0) return false;

    const VkDeviceSize curveBytes =
      static_cast<VkDeviceSize>(shape.curves.size() * persistentCurveStride());
    const VkDeviceSize bandBytes =
      static_cast<VkDeviceSize>(shape.bandWords.size() * sizeof(std::uint32_t));
    const VkDeviceSize curveOffset =
      static_cast<VkDeviceSize>(persistentSlugCurveCount) * persistentCurveStride();
    const VkDeviceSize bandOffset =
      static_cast<VkDeviceSize>(persistentSlugBandWordCount) * sizeof(std::uint32_t);
    if (curveOffset + curveBytes > persistentSlugCurves.size ||
        bandOffset + bandBytes > persistentSlugBands.size)
      return false;

    shape.persistentCurveBase = persistentSlugCurveCount;
    shape.persistentBandBase = persistentSlugBandWordCount;

    if (curveBytes != 0) {
      auto* target = static_cast<std::byte*>(persistentSlugCurves.mapped) + curveOffset;
      if (config.enableCompactPersistentCurves) {
        // Copy the six existing float32 coordinates bit-for-bit. The last two
        // floats are padding, not additional control points.
        for (std::size_t i = 0; i < shape.curves.size(); ++i)
          std::memcpy(target + i * persistentCurveStride(), &shape.curves[i], persistentCurveStride());
      } else {
        std::memcpy(target, shape.curves.data(), static_cast<std::size_t>(curveBytes));
      }
    }
    if (bandBytes != 0)
      std::memcpy(static_cast<std::byte*>(persistentSlugBands.mapped) + bandOffset,
                  shape.bandWords.data(), static_cast<std::size_t>(bandBytes));

    auto* residentBands =
      static_cast<std::uint32_t*>(persistentSlugBands.mapped) + shape.persistentBandBase;
    const std::uint32_t numHeaders =
      (shape.bandMaxY + 1u) + (shape.bandMaxX + 1u);
    for (std::uint32_t header = 0; header < numHeaders; ++header)
      residentBands[2u * shape.indirectionSize + header * 2u + 1u] +=
        shape.persistentBandBase;

    persistentSlugCurveCount += static_cast<std::uint32_t>(shape.curves.size());
    persistentSlugBandWordCount += static_cast<std::uint32_t>(shape.bandWords.size());
    shape.persistentResident = true;
    persistentSlugUploadedBytes += static_cast<std::size_t>(curveBytes + bandBytes);
    return true;
  }

  void appendDynamicVectorPath(std::vector<Instance>& instances,
                               std::vector<DynamicSlugCurve>& curveData,
                               std::vector<std::uint32_t>& bandWords,
                               const DynamicVectorPathCommand& command, bool cullToClip = true) const {
    constexpr std::uint32_t indirectionSize = 32u;
    if (command.curves.empty() || command.opacity <= 0.0f ||
        paintFullyTransparent(command.paint) ||
        (cullToClip && (command.clip.width <= 0.0f || command.clip.height <= 0.0f))) return;
    if (command.curves.size() > (std::numeric_limits<std::uint32_t>::max() - 128u) / 32u)
      throw std::length_error("Dynamic path exceeds Slug's 32-bit band addressing");

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = -std::numeric_limits<float>::max();
    float maxY = -std::numeric_limits<float>::max();
    for (const auto& c : command.curves) {
      if (!std::isfinite(c.p0.x) || !std::isfinite(c.p0.y) ||
          !std::isfinite(c.p1.x) || !std::isfinite(c.p1.y) ||
          !std::isfinite(c.p2.x) || !std::isfinite(c.p2.y))
        throw std::invalid_argument("Dynamic path coordinates must be finite");
      minX = std::min({minX, c.p0.x, c.p1.x, c.p2.x});
      minY = std::min({minY, c.p0.y, c.p1.y, c.p2.y});
      maxX = std::max({maxX, c.p0.x, c.p1.x, c.p2.x});
      maxY = std::max({maxY, c.p0.y, c.p1.y, c.p2.y});
    }
    const float width = std::max(maxX - minX, 1.0e-6f);
    const float height = std::max(maxY - minY, 1.0e-6f);
    if (!std::isfinite(width) || !std::isfinite(height))
      throw std::invalid_argument("Dynamic path bounds exceed the finite coordinate range");
    // Control-point bounds conservatively enclose every quadratic. Keep the half-pixel
    // Slug dilation when culling, including content on a clip boundary.
    constexpr float fringe = 0.5f;
    if (cullToClip &&
        (maxX + fringe < command.clip.x || minX - fringe > command.clip.x + command.clip.width ||
         maxY + fringe < command.clip.y || minY - fringe > command.clip.y + command.clip.height))
      return;

    const auto appendBits = [](std::string& key, float value) {
      if (value == 0.0f) value = 0.0f; // canonicalize negative zero
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      key.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
    };

    std::string cacheKey;
    const bool cacheEnabled = command.cacheable && config.dynamicPathCacheBytes != 0;
    if (cacheEnabled) {
      const std::uint32_t count = static_cast<std::uint32_t>(command.curves.size());
      cacheKey.append(reinterpret_cast<const char*>(&count), sizeof(count));
      const std::uint8_t rule = command.fillRule == FillRule::EvenOdd ? 1u : 0u;
      cacheKey.push_back(static_cast<char>(rule));
      appendBits(cacheKey, width);
      appendBits(cacheKey, height);
      cacheKey.reserve(cacheKey.size() + command.curves.size() * 6u * sizeof(std::uint32_t));
      for (const auto& source : command.curves) {
        appendBits(cacheKey, source.p0.x - minX); appendBits(cacheKey, source.p0.y - minY);
        appendBits(cacheKey, source.p1.x - minX); appendBits(cacheKey, source.p1.y - minY);
        appendBits(cacheKey, source.p2.x - minX); appendBits(cacheKey, source.p2.y - minY);
      }
    }

    const auto compileShape = [&](CompiledDynamicSlugShape& compiled) {
      compiled = {};
      compiled.width = width;
      compiled.height = height;
      const std::size_t numCurves = command.curves.size();
      compiled.bandMaxX = static_cast<std::uint32_t>(
        std::min<std::size_t>(16, std::max<std::size_t>(1, numCurves / 2))) - 1u;
      compiled.bandMaxY = compiled.bandMaxX;
      const std::uint32_t numBandsX = compiled.bandMaxX + 1u;
      const std::uint32_t numBandsY = compiled.bandMaxY + 1u;

      std::vector<float> curveMinX(numCurves), curveMaxX(numCurves);
      std::vector<float> curveMinY(numCurves), curveMaxY(numCurves);
      compiled.curves.reserve(numCurves);

      for (std::uint32_t index = 0; index < numCurves; ++index) {
        const auto& source = command.curves[index];
        const QuadraticBezier2D c{
          {source.p0.x - minX, source.p0.y - minY},
          {source.p1.x - minX, source.p1.y - minY},
          {source.p2.x - minX, source.p2.y - minY}};
        DynamicSlugCurve gpu{};
        gpu.p12[0] = c.p0.x; gpu.p12[1] = c.p0.y;
        gpu.p12[2] = c.p1.x; gpu.p12[3] = c.p1.y;
        gpu.p3[0] = c.p2.x; gpu.p3[1] = c.p2.y;
        compiled.curves.push_back(gpu);
        curveMinX[index] = std::min({c.p0.x, c.p1.x, c.p2.x});
        curveMaxX[index] = std::max({c.p0.x, c.p1.x, c.p2.x});
        curveMinY[index] = std::min({c.p0.y, c.p1.y, c.p2.y});
        curveMaxY[index] = std::max({c.p0.y, c.p1.y, c.p2.y});
      }

      std::vector<float> hboundaries(numBandsY + 1u);
      hboundaries.front() = 0.0f;
      hboundaries.back() = height;
      for (std::uint32_t i = 1; i < numBandsY; ++i) {
        const float snapped = std::round(
          static_cast<float>(i) / static_cast<float>(numBandsY) *
          static_cast<float>(indirectionSize)) / static_cast<float>(indirectionSize);
        hboundaries[i] = snapped * height;
      }

      std::vector<float> vboundaries(numBandsX + 1u);
      vboundaries.front() = 0.0f;
      vboundaries.back() = width;
      for (std::uint32_t i = 1; i < numBandsX; ++i) {
        const float snapped = std::round(
          static_cast<float>(i) / static_cast<float>(numBandsX) *
          static_cast<float>(indirectionSize)) / static_cast<float>(indirectionSize);
        vboundaries[i] = snapped * width;
      }

      std::vector<std::vector<std::uint32_t>> horizontal(numBandsY);
      std::vector<std::vector<std::uint32_t>> vertical(numBandsX);
      for (std::uint32_t band = 0; band < numBandsY; ++band) {
        auto& list = horizontal[band];
        const float lo = hboundaries[band], hi = hboundaries[band + 1u];
        for (std::uint32_t index = 0; index < numCurves; ++index)
          if (curveMaxY[index] >= lo && curveMinY[index] <= hi) list.push_back(index);
        std::sort(list.begin(), list.end(), [&](std::uint32_t lhs, std::uint32_t rhs) {
          if (curveMaxX[lhs] != curveMaxX[rhs]) return curveMaxX[lhs] > curveMaxX[rhs];
          return lhs < rhs;
        });
      }
      for (std::uint32_t band = 0; band < numBandsX; ++band) {
        auto& list = vertical[band];
        const float lo = vboundaries[band], hi = vboundaries[band + 1u];
        for (std::uint32_t index = 0; index < numCurves; ++index)
          if (curveMaxX[index] >= lo && curveMinX[index] <= hi) list.push_back(index);
        std::sort(list.begin(), list.end(), [&](std::uint32_t lhs, std::uint32_t rhs) {
          if (curveMaxY[lhs] != curveMaxY[rhs]) return curveMaxY[lhs] > curveMaxY[rhs];
          return lhs < rhs;
        });
      }

      std::array<std::uint32_t, indirectionSize> indirY{};
      std::array<std::uint32_t, indirectionSize> indirX{};
      for (std::uint32_t q = 0; q < indirectionSize; ++q) {
        const float frac = (static_cast<float>(q) + 0.5f) /
                           static_cast<float>(indirectionSize);
        const float emY = frac * height;
        std::uint32_t yBand = numBandsY - 1u;
        for (std::uint32_t band = 0; band + 1u < numBandsY; ++band)
          if (emY < hboundaries[band + 1u]) { yBand = band; break; }
        indirY[q] = yBand;

        const float emX = frac * width;
        std::uint32_t xBand = numBandsX - 1u;
        for (std::uint32_t band = 0; band + 1u < numBandsX; ++band)
          if (emX < vboundaries[band + 1u]) { xBand = band; break; }
        indirX[q] = xBand;
      }

      const std::uint32_t headerBase = 2u * indirectionSize;
      const std::uint32_t headerWords = 2u * (numBandsY + numBandsX);
      compiled.bandWords.assign(headerBase + headerWords, 0u);
      for (std::uint32_t q = 0; q < indirectionSize; ++q) {
        compiled.bandWords[q] = indirY[q];
        compiled.bandWords[indirectionSize + q] = indirX[q];
      }

      std::uint32_t headerCursor = headerBase;
      const auto writeBands = [&](const std::vector<std::vector<std::uint32_t>>& bands) {
        for (const auto& list : bands) {
          compiled.bandWords[headerCursor] = static_cast<std::uint32_t>(list.size());
          compiled.bandWords[headerCursor + 1u] =
            static_cast<std::uint32_t>(compiled.bandWords.size());
          headerCursor += 2u;
          compiled.bandWords.insert(compiled.bandWords.end(), list.begin(), list.end());
        }
      };
      writeBands(horizontal);
      writeBands(vertical);
    };

    CompiledDynamicSlugShape transient;
    CompiledDynamicSlugShape* compiled = nullptr;
    bool cached = false;
    if (cacheEnabled) {
      if (auto* found = dynamicSlugPathCache.find(cacheKey)) {
        compiled = found;
        compiled->useCount = 2; // Only first use versus reuse matters for promotion.
        ++dynamicSlugCacheHits;
        cached = true;
      } else {
        compileShape(transient);
        transient.useCount = 1;
        transient.cacheId = ++dynamicSlugUseCounter;
        const std::size_t bytes = sizeof(CompiledDynamicSlugShape) +
          transient.curves.capacity() * sizeof(DynamicSlugCurve) +
          transient.bandWords.capacity() * sizeof(std::uint32_t);
        compiled = dynamicSlugPathCache.insert(std::move(cacheKey), std::move(transient), bytes);
        cached = compiled != nullptr;
        if (!cached) compiled = &transient;
        ++dynamicSlugCacheMisses;
      }
    } else {
      compileShape(transient);
      compiled = &transient;
    }

    const bool persistent =
      cached && compiled->useCount >= 2u &&
      ensurePersistentSlugResident(*compiled);
    std::uint32_t curveBase = persistent ? compiled->persistentCurveBase : 0u;
    std::uint32_t blockBase = persistent ? compiled->persistentBandBase : 0u;

    if (!persistent) {
      bool alreadyUploaded = false;
      if (cached) {
        if (auto uploaded = frameDynamicShapeLocations.find(compiled->cacheId);
            uploaded != frameDynamicShapeLocations.end()) {
          curveBase = uploaded->second.first;
          blockBase = uploaded->second.second;
          alreadyUploaded = true;
        }
      }

      if (!alreadyUploaded) {
        curveBase = static_cast<std::uint32_t>(curveData.size());
        blockBase = static_cast<std::uint32_t>(bandWords.size());
        curveData.insert(curveData.end(), compiled->curves.begin(), compiled->curves.end());
        bandWords.insert(bandWords.end(), compiled->bandWords.begin(), compiled->bandWords.end());

        const std::uint32_t numHeaders =
          (compiled->bandMaxY + 1u) + (compiled->bandMaxX + 1u);
        for (std::uint32_t header = 0; header < numHeaders; ++header) {
          const std::uint32_t offsetWord =
            blockBase + 2u * indirectionSize + header * 2u + 1u;
          bandWords[offsetWord] += blockBase;
        }
        if (cached)
          frameDynamicShapeLocations.emplace(compiled->cacheId, std::pair{curveBase, blockBase});
      }
    }

    Instance quad{};
    const float positionBounds[4]{minX, minY, maxX, maxY};
    const float localBounds[4]{0.0f, 0.0f, compiled->width, compiled->height};
    std::copy(std::begin(positionBounds), std::end(positionBounds), quad.positionRect);
    std::copy(std::begin(localBounds), std::end(localBounds), quad.emRect);
    quad.shapeData[0] = 0xFFFFFFF8U;
    quad.shapeData[1] = curveBase;
    quad.shapeData[2] = blockBase;
    quad.shapeData[3] =
      (compiled->bandMaxX & 0xFFu) |
      ((compiled->bandMaxY & 0xFFu) << 8u) |
      (command.fillRule == FillRule::EvenOdd ? 0x10000u : 0u) |
      (persistent ? 0x80000000u : 0u);

    quad.bandTransform[0] = static_cast<float>(indirectionSize) / compiled->width;
    quad.bandTransform[1] = static_cast<float>(indirectionSize) / compiled->height;
    quad.bandTransform[2] = 0.0f;
    quad.bandTransform[3] = 0.0f;

    const auto& paint = command.paint;
    const float first[4]{paint.start.r, paint.start.g, paint.start.b, paint.start.a};
    const float second[4]{paint.end.r, paint.end.g, paint.end.b, paint.end.a};
    std::copy(std::begin(first), std::end(first), quad.color0);
    std::copy(std::begin(second), std::end(second), quad.color1);
    quad.paint[0] = static_cast<float>(paint.kind);
    // compileRenderIR applies the original display command's opacity exactly once.
    quad.paint[1] = paint.opacity;
    quad.paint[2] = paint.shaderParameter;
    quad.paint[3] = 0.0f;
    quad.gradient[0] = paint.origin.x; quad.gradient[1] = paint.origin.y;
    quad.gradient[2] = paint.target.x; quad.gradient[3] = paint.target.y;
    quad.clip[0] = command.clip.x; quad.clip[1] = command.clip.y;
    quad.clip[2] = command.clip.width; quad.clip[3] = command.clip.height;
    instances.push_back(quad);
  }

  std::vector<QuadraticBezier2D> roundedRectCurvesCached(
      Rect bounds, std::array<Vec2,4> radii, CornerSmoothing smoothing) const {
    std::string key;
    key.push_back('R');
    key.reserve((2u + 8u + 4u) * sizeof(std::uint32_t));
    const auto appendFloat = [&](float value) {
      if (value == 0.0f) value = 0.0f;
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      key.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
    };
    appendFloat(bounds.width);
    appendFloat(bounds.height);
    for (const auto& radius : radii) {
      appendFloat(radius.x);
      appendFloat(radius.y);
    }
    appendFloat(smoothing.topLeftPercent);
    appendFloat(smoothing.topRightPercent);
    appendFloat(smoothing.bottomRightPercent);
    appendFloat(smoothing.bottomLeftPercent);

    std::vector<QuadraticBezier2D> curves;
    if (auto* found = dynamicGeometryCache.find(key)) {
      curves = *found;
    } else {
      curves = makeContinuousRoundedRectQuadraticsXY(
        {0.0f,0.0f,bounds.width,bounds.height}, radii, smoothing);
      if (config.dynamicGeometryCacheBytes != 0) {
        auto copy = curves;
        const std::size_t bytes = sizeof(copy) + copy.capacity() * sizeof(QuadraticBezier2D);
        dynamicGeometryCache.insert(std::move(key), std::move(copy), bytes);
      }
    }

    if (bounds.x != 0.0f || bounds.y != 0.0f) {
      const Vec2 offset{bounds.x,bounds.y};
      for (auto& curve : curves) {
        curve.p0 = curve.p0 + offset;
        curve.p1 = curve.p1 + offset;
        curve.p2 = curve.p2 + offset;
      }
    }
    return curves;
  }

  void appendRoundedRectSlug(std::vector<Instance>& instances,
                             std::vector<DynamicSlugCurve>& curveData,
                             std::vector<std::uint32_t>& bandWords,
                             const RoundedRectCommand& command, bool cullToClip = true) const {
    const BorderWidths widths=resolvedBorderWidths(command.border);
    const bool hasBorder=widths.maximum()>0.0f&&!paintFullyTransparent(command.border.paint);
    if (command.opacity <= 0.0f || (cullToClip && outsideClip(
          strokeBounds(command.destination, widths, strokeOutsideFactor(command.border.align)),
          command.clip))) return;
    const auto smoothing = config.enableContinuousCorners
      ? command.continuousCorners : CornerSmoothing::all(0.0f);
    const auto emit=[&](std::vector<QuadraticBezier2D> curves,Rect bounds,
                        const Paint& paint,FillRule rule=FillRule::NonZero){
      if(curves.empty()||bounds.width<=0.0f||bounds.height<=0.0f||paintFullyTransparent(paint)) return;
      DynamicVectorPathCommand path{
        std::move(curves),bounds,command.clip,paint,rule,command.opacity,true};
      appendDynamicVectorPath(instances,curveData,bandWords,path,cullToClip);
    };

    const std::array<Vec2,4> authoredXY{{
      {command.radiiPx.topLeft,command.radiiPx.topLeft},
      {command.radiiPx.topRight,command.radiiPx.topRight},
      {command.radiiPx.bottomRight,command.radiiPx.bottomRight},
      {command.radiiPx.bottomLeft,command.radiiPx.bottomLeft}}};

    if(!hasBorder){
      emit(roundedRectCurvesCached(
             command.destination,authoredXY,smoothing),
           command.destination,command.paint);
      return;
    }

    const float outside=strokeOutsideFactor(command.border.align);
    const float inside=1.0f-outside;
    const Rect outer=strokeBounds(command.destination,widths,outside);
    const Rect inner{
      command.destination.x+widths.left*inside,
      command.destination.y+widths.top*inside,
      std::max(0.0f,command.destination.width-(widths.left+widths.right)*inside),
      std::max(0.0f,command.destination.height-(widths.top+widths.bottom)*inside)};

    auto outerR=authoredXY;
    auto innerR=authoredXY;
    const std::array<Vec2,4> outsideDelta{{
      {widths.left*outside,widths.top*outside},
      {widths.right*outside,widths.top*outside},
      {widths.right*outside,widths.bottom*outside},
      {widths.left*outside,widths.bottom*outside}}};
    const std::array<Vec2,4> insideDelta{{
      {widths.left*inside,widths.top*inside},
      {widths.right*inside,widths.top*inside},
      {widths.right*inside,widths.bottom*inside},
      {widths.left*inside,widths.bottom*inside}}};
    for(std::size_t i=0;i<4;++i){
      const bool rounded=authoredXY[i].x>1.0e-6f||authoredXY[i].y>1.0e-6f;
      if(rounded){
        outerR[i]=outerR[i]+outsideDelta[i];
        innerR[i].x=std::max(0.0f,innerR[i].x-insideDelta[i].x);
        innerR[i].y=std::max(0.0f,innerR[i].y-insideDelta[i].y);
      }else{
        outerR[i]={0.0f,0.0f};
        innerR[i]={0.0f,0.0f};
      }
    }

    auto borderCurves=roundedRectCurvesCached(
      outer,outerR,smoothing);
    if(inner.width>0.0f&&inner.height>0.0f){
      auto hole=roundedRectCurvesCached(inner,innerR,smoothing);
      reverseQuadraticContour(hole);
      borderCurves.insert(borderCurves.end(),
                          std::make_move_iterator(hole.begin()),std::make_move_iterator(hole.end()));
    }

    const bool opaqueBorder=paintFullyOpaque(command.border.paint);
    if(!opaqueBorder)
      emit(roundedRectCurvesCached(
             command.destination,authoredXY,smoothing),
           command.destination,command.paint);
    emit(std::move(borderCurves),outer,command.border.paint);
    if(opaqueBorder&&!paintFullyTransparent(command.paint)&&inner.width>0.0f&&inner.height>0.0f)
      emit(roundedRectCurvesCached(inner,innerR,smoothing),
           inner,command.paint);
  }

  static void appendGeometryFloat(std::string& key, float value) {
    if (value == 0.0f) value = 0.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    key.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
  }

  static std::string strokeGeometryKey(
      char tag, std::initializer_list<float> values, LineCap cap) {
    std::string key;
    key.reserve(2u + values.size() * sizeof(std::uint32_t));
    key.push_back(tag);
    key.push_back(static_cast<char>(cap));
    for (float value : values) appendGeometryFloat(key, value);
    return key;
  }

  template<class Builder>
  std::vector<QuadraticBezier2D> cachedStrokeGeometry(
      std::string key, Builder&& builder) const {
    if (auto* found = dynamicGeometryCache.find(key)) return *found;
    auto curves = builder();
    if (config.dynamicGeometryCacheBytes == 0) return curves;
    auto copy = curves;
    const std::size_t bytes = sizeof(copy) + copy.capacity() * sizeof(QuadraticBezier2D);
    dynamicGeometryCache.insert(std::move(key), std::move(copy), bytes);
    return curves;
  }

  void appendLine(std::vector<Instance>& instances,
                  std::vector<DynamicSlugCurve>& curveData,
                  std::vector<std::uint32_t>& bandWords,
                  const LineCommand& command, bool cullToClip = true) const {
    const float minX=std::min(command.from.x,command.to.x), minY=std::min(command.from.y,command.to.y);
    const Rect bounds{minX,minY,std::abs(command.to.x-command.from.x),
                                std::abs(command.to.y-command.from.y)};
    if (command.opacity <= 0.0f ||
        (cullToClip && outsideClip(bounds,command.clip,command.style.width+0.5f))) return;
    auto curves=cachedStrokeGeometry(
      strokeGeometryKey('L',{command.from.x,command.from.y,command.to.x,command.to.y,
                             command.style.width},command.style.cap),
      [&]{return makeLineStrokeQuadratics(
        command.from,command.to,command.style.width,command.style.cap);});
    if(curves.empty()) return;
    DynamicVectorPathCommand path{std::move(curves),{},command.clip,command.style.paint,
                                  FillRule::NonZero,command.opacity,true};
    path.bounds=quadraticBounds(path.curves);
    appendDynamicVectorPath(instances,curveData,bandWords,path,cullToClip);
  }

  void appendCubicBezier(std::vector<Instance>& instances,
                         std::vector<DynamicSlugCurve>& curveData,
                         std::vector<std::uint32_t>& bandWords,
                         const CubicBezierCommand& command, bool cullToClip = true) const {
    const float minX=std::min({command.from.x,command.control1.x,command.control2.x,command.to.x});
    const float minY=std::min({command.from.y,command.control1.y,command.control2.y,command.to.y});
    const float maxX=std::max({command.from.x,command.control1.x,command.control2.x,command.to.x});
    const float maxY=std::max({command.from.y,command.control1.y,command.control2.y,command.to.y});
    if (command.opacity <= 0.0f || (cullToClip &&
        outsideClip({minX,minY,maxX-minX,maxY-minY},command.clip,command.style.width+0.5f))) return;
    auto curves=cachedStrokeGeometry(
      strokeGeometryKey('C',{command.from.x,command.from.y,
                             command.control1.x,command.control1.y,
                             command.control2.x,command.control2.y,
                             command.to.x,command.to.y,command.style.width},
                        command.style.cap),
      [&]{return makeCubicStrokeQuadratics(
        command.from,command.control1,command.control2,command.to,
        command.style.width,command.style.cap);});
    if(curves.empty()) return;
    DynamicVectorPathCommand path{std::move(curves),{},command.clip,command.style.paint,
                                  FillRule::NonZero,command.opacity,true};
    path.bounds=quadraticBounds(path.curves);
    appendDynamicVectorPath(instances,curveData,bandWords,path,cullToClip);
  }

  void appendArc(std::vector<Instance>& instances,
                 std::vector<DynamicSlugCurve>& curveData,
                 std::vector<std::uint32_t>& bandWords,
                 const ArcCommand& command, bool cullToClip = true) const {
    const Rect bounds{command.center.x-command.radius,command.center.y-command.radius,
                      2.0f*command.radius,2.0f*command.radius};
    if (command.opacity <= 0.0f ||
        (cullToClip && outsideClip(bounds,command.clip,command.style.width+0.5f))) return;
    auto curves=cachedStrokeGeometry(
      strokeGeometryKey('A',{command.center.x,command.center.y,command.radius,
                             command.startRadians,command.sweepRadians,command.style.width},
                        command.style.cap),
      [&]{return makeArcStrokeQuadratics(
        command.center,command.radius,command.startRadians,command.sweepRadians,
        command.style.width,command.style.cap);});
    if(curves.empty()) return;
    DynamicVectorPathCommand path{std::move(curves),{},command.clip,command.style.paint,
                                  FillRule::NonZero,command.opacity,true};
    path.bounds=quadraticBounds(path.curves);
    appendDynamicVectorPath(instances,curveData,bandWords,path,cullToClip);
  }

  const CachedGlyphRun& glyphRun(std::string_view text, const TextStyle& style) const {
    GlyphRunKey key{
      std::string(text), style.fontName, style.fontStyle, style.weight, style.italic};
    if (auto found = glyphRunCache.find(key); found != glyphRunCache.end()) {
      found->second.lastUsed = ++glyphRunUseCounter;
      return found->second;
    }
    const std::size_t cacheCapacity = std::max<std::size_t>(1, config.glyphRunCacheCapacity);
    if (glyphRunCache.size() >= cacheCapacity) {
      auto oldest = glyphRunCache.begin();
      for (auto it = std::next(glyphRunCache.begin()); it != glyphRunCache.end(); ++it)
        if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
      glyphRunCache.erase(oldest);
    }
    CachedGlyphRun run;
    if (const auto shaped = vectorAtlas.shapeText(
          text, style.fontName, style.weight, style.italic, style.fontStyle)) {
      run.shaped = true;
      run.rightToLeft = shaped->rightToLeft;
      run.xAdvance = shaped->xAdvance;
      run.yAdvance = shaped->yAdvance;
      run.glyphs.reserve(shaped->glyphs.size());
      for (const auto& positioned : shaped->glyphs) {
        const auto glyph = vectorAtlas.native().getShape(slughorn::Key(positioned.shape));
        if (glyph) {
          run.glyphs.push_back({
            true, positioned.cluster, *glyph,
            positioned.xAdvance, positioned.yAdvance,
            positioned.xOffset, positioned.yOffset
          });
        } else {
          run.glyphs.push_back({
            false, positioned.cluster, {},
            positioned.xAdvance, positioned.yAdvance,
            positioned.xOffset, positioned.yOffset
          });
        }
      }
    } else {
      const auto codepoints = decodeUtf8(text);
      run.glyphs.reserve(codepoints.size());
      for (const auto codepoint : codepoints) {
        const ShapeId glyphId = vectorAtlas.glyph(
          codepoint, style.fontName, style.weight, style.italic, style.fontStyle);
        const auto glyph = vectorAtlas.native().getShape(slughorn::Key(glyphId));
        if (glyph) run.glyphs.push_back({true, codepoint, *glyph});
        else run.glyphs.push_back({false, codepoint, {}});
      }
    }
    run.lastUsed = ++glyphRunUseCounter;
    return glyphRunCache.emplace(std::move(key), std::move(run)).first->second;
  }

  float textWidth(const CachedGlyphRun& run, const TextStyle& style) const {
    if (run.shaped) {
      const float spacing = run.glyphs.size() > 1
        ? static_cast<float>(run.glyphs.size() - 1) * style.letterSpacing
        : 0.0f;
      return std::max(0.0f, std::abs(run.xAdvance) * style.size + spacing);
    }

    float width = 0.0f;
    std::uint32_t previous = 0;
    for (const auto& cached : run.glyphs) {
      if (previous != 0) {
        width += vectorAtlas.kerning(
          previous, cached.codepoint, style.fontName, style.weight, style.italic, style.fontStyle) *
          style.size;
      }
      const float advance = cached.present ? static_cast<float>(cached.shape.advance) : 0.6f;
      width += advance * style.size + style.letterSpacing;
      previous = cached.codepoint;
    }
    return std::max(0.0f, width - (run.glyphs.empty() ? 0.0f : style.letterSpacing));
  }

  void appendGlyphRun(std::vector<Instance>& instances, const CachedGlyphRun& run,
                      const TextStyle& style, float& x, float baseline,
                      float size, float spacing, Rect clip, float slant) const {
    if (run.shaped) {
      const float signedAdvance = run.xAdvance * size;
      const float direction = signedAdvance < -0.00001f ||
                              (std::abs(signedAdvance) <= 0.00001f && run.rightToLeft)
        ? -1.0f : 1.0f;
      float penX = x + (signedAdvance < 0.0f ? -signedAdvance : 0.0f);
      for (std::size_t index = 0; index < run.glyphs.size(); ++index) {
        const auto& cached = run.glyphs[index];
        if (cached.present) {
          const auto& glyph = cached.shape;
          if (glyph.width > 0 && glyph.height > 0) {
            Rect destination{
              penX + cached.xOffset * size + static_cast<float>(glyph.bearingX) * size,
              baseline - cached.yOffset * size - static_cast<float>(glyph.bearingY) * size,
              static_cast<float>(glyph.width) * size,
              static_cast<float>(glyph.height) * size,
            };
            appendResolvedShape(instances, glyph, destination, style.paint, clip, slant);
            if (style.bold) {
              destination.x += std::max(0.55f, size * 0.035f);
              appendResolvedShape(instances, glyph, destination, style.paint, clip, slant);
            }
          }
        }
        penX += cached.xAdvance * size;
        if (index + 1 < run.glyphs.size()) penX += spacing * direction;
      }
      x += std::abs(run.xAdvance) * size +
           (run.glyphs.size() > 1
              ? static_cast<float>(run.glyphs.size() - 1) * spacing
              : 0.0f);
      return;
    }

    std::uint32_t previousCodepoint = 0;
    for (const auto& cached : run.glyphs) {
      if (previousCodepoint != 0) {
        x += vectorAtlas.kerning(
          previousCodepoint, cached.codepoint, style.fontName, style.weight,
          style.italic, style.fontStyle) * size;
      }
      if (!cached.present) {
        x += size * 0.6f + spacing;
        previousCodepoint = cached.codepoint;
        continue;
      }
      const auto& glyph = cached.shape;
      const float advance = static_cast<float>(glyph.advance) * size;
      if (glyph.width > 0 && glyph.height > 0) {
        Rect destination{
          x + static_cast<float>(glyph.bearingX) * size,
          baseline - static_cast<float>(glyph.bearingY) * size,
          static_cast<float>(glyph.width) * size,
          static_cast<float>(glyph.height) * size,
        };
        appendResolvedShape(instances, glyph, destination, style.paint, clip, slant);
        if (style.bold) {
          destination.x += std::max(0.55f, size * 0.035f);
          appendResolvedShape(instances, glyph, destination, style.paint, clip, slant);
        }
      }
      x += advance + spacing;
      previousCodepoint = cached.codepoint;
    }
  }

  float textWidth(std::string_view text, const TextStyle& style, float scale) const {
    auto scaled = style;
    scaled.size *= scale;
    scaled.letterSpacing *= scale;
    return textWidth(glyphRun(text, style), scaled);
  }

  float textBaselineOffset(const TextStyle& style, float scale = 1.0f) const {
    const float size = style.size * scale;
    const float lineAdvance = size * std::max(style.lineHeight, 0.01f);
    const auto metrics = vectorAtlas.fontMetrics(
      style.fontName, style.weight, style.italic, style.fontStyle).value_or(FontFaceMetrics{});
    const float ascender = std::max(metrics.ascender, 0.0f) * size;
    const float descender = std::max(metrics.descender, 0.0f) * size;
    const float naturalHeight = ascender + descender;
    const float leading = std::max(0.0f, lineAdvance - naturalHeight);
    return leading * 0.5f + ascender;
  }

  void appendRichText(std::vector<Instance>& instances, const TextCommand& command) const {
    struct Fragment {
      std::string_view text;
      const TextStyle* style = nullptr;
    };
    struct Line {
      std::size_t first = 0;
      std::size_t count = 0;
      float width = 0.0f;
      float height = 0.0f;
      float baselineOffset = 0.0f;
      float maximumSize = 0.0f;
      float trailingSpacing = 0.0f;
    };

    const float runScale = std::max(command.runScale, 0.01f);
    std::vector<Fragment> fragments;
    fragments.reserve(command.runs.size() + 1);
    std::vector<Line> lines(1);
    lines.reserve(command.runs.size() + 1);
    std::string marker;
    if (command.style.listMarker != ListMarker::None) {
      marker = command.style.listMarker == ListMarker::Bullet ? "* " : "1. ";
      fragments.push_back({marker, &command.style});
      auto& first = lines.front();
      first.count = 1;
      first.width = textWidth(marker, command.style, 1.0f);
      first.height = command.style.size * std::max(command.style.lineHeight, 0.01f);
      first.baselineOffset = textBaselineOffset(command.style);
      first.maximumSize = command.style.size;
      first.trailingSpacing = command.style.letterSpacing;
    }
    for (const auto& run : command.runs) {
      std::size_t start = 0;
      while (start <= run.text.size()) {
        const std::size_t end = run.text.find('\n', start);
        const std::string_view fragment(
          run.text.data() + start,
          (end == std::string::npos ? run.text.size() : end) - start);
        auto& line = lines.back();
        if (!fragment.empty()) {
          if (line.count != 0) line.width += line.trailingSpacing;
          fragments.push_back({fragment, &run.style});
          ++line.count;
          line.width += textWidth(fragment, run.style, runScale);
          line.trailingSpacing = run.style.letterSpacing * runScale;
          line.maximumSize = std::max(line.maximumSize, run.style.size * runScale);
          line.height = std::max(
            line.height, run.style.size * runScale * std::max(run.style.lineHeight, 0.01f));
          line.baselineOffset = std::max(
            line.baselineOffset, textBaselineOffset(run.style, runScale));
        }
        if (end == std::string::npos) break;
        lines.push_back(Line{.first = fragments.size()});
        start = end + 1;
      }
    }

    const float fallbackHeight = command.style.size * std::max(command.style.lineHeight, 0.01f);
    float totalHeight = 0.0f;
    for (auto& line : lines) {
      if (line.height <= 0.0f) line.height = fallbackHeight;
      if (line.baselineOffset <= 0.0f)
        line.baselineOffset = textBaselineOffset(command.style);
      if (line.maximumSize <= 0.0f) line.maximumSize = command.style.size;
      totalHeight += line.height;
    }
    float top = command.bounds.y;
    if (command.style.verticalAlign == VerticalAlign::Center)
      top += (command.bounds.height - totalHeight) * 0.5f;
    else if (command.style.verticalAlign == VerticalAlign::Bottom)
      top += command.bounds.height - totalHeight;

    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
      const auto& line = lines[lineIndex];
      if (top + line.height >= command.clip.y &&
          top <= command.clip.y + command.clip.height) {
        const float indent = lineIndex == 0 ? std::max(command.style.indent, 0.0f) : 0.0f;
        const float availableWidth = std::max(0.0f, command.bounds.width - indent);
        float x = command.bounds.x + indent;
        if (command.style.align == HorizontalAlign::Center)
          x += (availableWidth - line.width) * 0.5f;
        else if (command.style.align == HorizontalAlign::Right)
          x += availableWidth - line.width;
        const float baseline = top + line.baselineOffset;
        for (std::size_t index = 0; index < line.count; ++index) {
          const auto& fragment = fragments[line.first + index];
          const auto& authored = *fragment.style;
          const float size = authored.size * runScale;
          const float spacing = authored.letterSpacing * runScale;
          const float fragmentStart = x;
          const float fragmentWidth = textWidth(fragment.text, authored, runScale);
          const float slant = authored.italic &&
            !vectorAtlas.hasFontFace(authored.fontName, true, authored.fontStyle) ? 0.18f : 0.0f;
          const auto& resolvedRun = glyphRun(fragment.text, authored);
          appendGlyphRun(
            instances, resolvedRun, authored, x, baseline, size, spacing, command.clip, slant);
          x = fragmentStart + fragmentWidth;
          if (authored.underline && fragmentWidth > 0.0f) {
            appendShape(
              instances, vectorAtlas.glyph(
                '_', authored.fontName, authored.weight, authored.italic, authored.fontStyle),
              {fragmentStart, baseline + size * 0.07f, fragmentWidth,
               std::max(1.0f, size * 0.065f)},
              authored.paint, command.clip);
          }
          if (authored.strikethrough && fragmentWidth > 0.0f) {
            appendShape(
              instances, vectorAtlas.glyph(
                '-', authored.fontName, authored.weight, authored.italic, authored.fontStyle),
              {fragmentStart, baseline - size * 0.32f, fragmentWidth,
               std::max(1.0f, size * 0.06f)},
              authored.paint, command.clip);
          }
          if (index + 1 < line.count) x += spacing;
        }
      }
      top += line.height;
    }
  }

  void appendText(std::vector<Instance>& instances, const TextCommand& command) const {
    if (!command.runs.empty()) {
      appendRichText(instances, command);
      return;
    }
    std::string prefixed;
    std::string_view text = command.text();
    if (command.style.listMarker != ListMarker::None) {
      const std::string_view marker = command.style.listMarker == ListMarker::Bullet ? "* " : "1. ";
      prefixed.reserve(marker.size() + text.size());
      prefixed.append(marker);
      prefixed.append(text);
      text = prefixed;
    }
    std::size_t lineStart = 0;
    std::size_t lineNumber = 0;
    const std::size_t lineCount =
      1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    const float lineAdvance = command.style.size * std::max(command.style.lineHeight, 0.01f);
    const float textBlockHeight = lineAdvance * static_cast<float>(lineCount);
    float firstLineTop = command.bounds.y;
    if (command.style.verticalAlign == VerticalAlign::Center)
      firstLineTop += (command.bounds.height - textBlockHeight) * 0.5f;
    else if (command.style.verticalAlign == VerticalAlign::Bottom)
      firstLineTop += command.bounds.height - textBlockHeight;
    const float slant = command.style.italic &&
      !vectorAtlas.hasFontFace(
        command.style.fontName, true, command.style.fontStyle) ? 0.18f : 0.0f;
    while (lineStart <= text.size()) {
      const std::size_t lineEnd = text.find('\n', lineStart);
      const std::string_view line(text.data() + lineStart,
                                  (lineEnd == std::string::npos ? text.size() : lineEnd) -
                                      lineStart);
      const float top = firstLineTop + static_cast<float>(lineNumber) * lineAdvance;
      const float lineBottom = top + command.style.size * std::max(command.style.lineHeight, 1.25f);
      if (lineBottom < command.clip.y ||
          top - command.style.size * 0.5f > command.clip.y + command.clip.height) {
        if (lineEnd == std::string::npos)
          break;
        lineStart = lineEnd + 1;
        ++lineNumber;
        continue;
      }
      const auto& resolvedRun = glyphRun(line, command.style);
      const bool needsWidth = command.style.align != HorizontalAlign::Left ||
                              command.style.underline || command.style.strikethrough;
      const float width = needsWidth ? textWidth(resolvedRun, command.style) : 0.0f;
      const float indent = lineNumber == 0 ? std::max(command.style.indent, 0.0f) : 0.0f;
      const float availableWidth = std::max(0.0f, command.bounds.width - indent);
      float x = command.bounds.x + indent;
      if (command.style.align == HorizontalAlign::Center) x += (availableWidth - width) * 0.5f;
      else if (command.style.align == HorizontalAlign::Right) x += availableWidth - width;
      const float baseline = top + textBaselineOffset(command.style);
      const float lineX = x;
      appendGlyphRun(
        instances, resolvedRun, command.style, x, baseline, command.style.size,
        command.style.letterSpacing, command.clip, slant);
      if (width > 0.0f && command.style.underline) {
        Rect decoration{lineX, baseline + command.style.size * 0.07f, width, std::max(1.0f, command.style.size * 0.065f)};
        appendShape(instances, vectorAtlas.glyph(
                      '_', command.style.fontName, command.style.weight, command.style.italic,
                      command.style.fontStyle), decoration,
                    command.style.paint, command.clip);
      }
      if (width > 0.0f && command.style.strikethrough) {
        Rect decoration{lineX, baseline - command.style.size * 0.32f, width, std::max(1.0f, command.style.size * 0.06f)};
        appendShape(instances, vectorAtlas.glyph(
                      '-', command.style.fontName, command.style.weight, command.style.italic,
                      command.style.fontStyle), decoration,
                    command.style.paint, command.clip);
      }
      if (lineEnd == std::string::npos)
        break;
      lineStart = lineEnd + 1;
      ++lineNumber;
    }
  }

  static VkDeviceSize alignRetainedBytes(VkDeviceSize value) noexcept {
    constexpr VkDeviceSize alignment = 16;
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void growRetainedArena(VkDeviceSize requiredCapacity) {
    if (requiredCapacity <= retainedArena.size) return;
    VkDeviceSize nextSize = retainedArena.size
      ? retainedArena.size * 2
      : std::max<VkDeviceSize>(16, static_cast<VkDeviceSize>(config.retainedArenaInitialBytes));
    while (nextSize < requiredCapacity) nextSize *= 2;
    Buffer replacement{};
    createBuffer(nextSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, replacement, false);
    if (retainedArena.buffer && retainedArenaUsed != 0) {
      VkCommandBuffer command = beginSingleUse();
      VkBufferMemoryBarrier readable{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      readable.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      readable.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      readable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      readable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      readable.buffer = retainedArena.buffer;
      readable.size = retainedArenaUsed;
      vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &readable, 0, nullptr);
      VkBufferCopy copy{0, 0, retainedArenaUsed};
      vkCmdCopyBuffer(command, retainedArena.buffer, replacement.buffer, 1, &copy);
      VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ready.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
      ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ready.buffer = replacement.buffer;
      ready.size = retainedArenaUsed;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &ready,
                           0, nullptr);
      endSingleUse(command);
    }
    destroyBuffer(retainedArena);
    retainedArena = replacement;
  }

  VkDeviceSize allocateRetainedRange(VkDeviceSize requestedBytes) {
    const VkDeviceSize bytes = alignRetainedBytes(requestedBytes);
    for (std::size_t index = 0; index < retainedFreeRanges.size(); ++index) {
      auto& range = retainedFreeRanges[index];
      if (range.size < bytes) continue;
      const VkDeviceSize offset = range.offset;
      if (range.size == bytes) {
        retainedFreeRanges.erase(retainedFreeRanges.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        range.offset += bytes;
        range.size -= bytes;
      }
      return offset;
    }
    const VkDeviceSize offset = retainedArenaUsed;
    const VkDeviceSize required = offset + bytes;
    growRetainedArena(required);
    retainedArenaUsed = required;
    return offset;
  }

  void freeRetainedRange(VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) return;
    retainedFreeRanges.push_back({offset, size});
    std::sort(retainedFreeRanges.begin(), retainedFreeRanges.end(),
              [](const FreeRange& a, const FreeRange& b) { return a.offset < b.offset; });
    std::vector<FreeRange> merged;
    merged.reserve(retainedFreeRanges.size());
    for (const auto& range : retainedFreeRanges) {
      if (!merged.empty() && merged.back().offset + merged.back().size >= range.offset) {
        const VkDeviceSize end = std::max(merged.back().offset + merged.back().size,
                                          range.offset + range.size);
        merged.back().size = end - merged.back().offset;
      } else {
        merged.push_back(range);
      }
    }
    retainedFreeRanges = std::move(merged);
    while (!retainedFreeRanges.empty()) {
      const auto& tail = retainedFreeRanges.back();
      if (tail.offset + tail.size != retainedArenaUsed) break;
      retainedArenaUsed = tail.offset;
      retainedFreeRanges.pop_back();
    }
  }

  void releaseRetainedDynamic(RetainedGeometry& retained) {
    if(retained.dynamicDescriptorPool!=VK_NULL_HANDLE)
      vkDestroyDescriptorPool(device, retained.dynamicDescriptorPool, nullptr);
    retained.dynamicDescriptorPool = VK_NULL_HANDLE;
    retained.dynamicDescriptorSet = VK_NULL_HANDLE;
    destroyBuffer(retained.dynamicCurves);
    destroyBuffer(retained.dynamicBands);
  }

  std::size_t uploadRetainedDynamic(RetainedGeometry &retained,
                                    const std::vector<DynamicSlugCurve> &curves,
                                    const std::vector<std::uint32_t> &bands) {
    if (curves.empty() || bands.empty()) {
      if (retained.dynamicDescriptorPool != VK_NULL_HANDLE ||
          retained.dynamicCurves.buffer != VK_NULL_HANDLE ||
          retained.dynamicBands.buffer != VK_NULL_HANDLE) {
        check(vkQueueWaitIdle(graphicsQueue),
              "vkQueueWaitIdle(clear retained dynamic)");
        releaseRetainedDynamic(retained);
      }
      return 0;
    }

    const VkDeviceSize curveBytes = curves.size() * sizeof(DynamicSlugCurve);
    const VkDeviceSize bandBytes = bands.size() * sizeof(std::uint32_t);
    const bool recreate = retained.dynamicDescriptorSet == VK_NULL_HANDLE ||
                          retained.dynamicCurves.size < curveBytes ||
                          retained.dynamicBands.size < bandBytes;
    if (!recreate) {
      if (std::memcmp(retained.dynamicCurves.mapped, curves.data(),
                      static_cast<std::size_t>(curveBytes)) == 0 &&
          std::memcmp(retained.dynamicBands.mapped, bands.data(),
                      static_cast<std::size_t>(bandBytes)) == 0)
        return 0;
      check(vkQueueWaitIdle(graphicsQueue),
            "vkQueueWaitIdle(update retained dynamic)");
    }
    if (recreate) {
      if (retained.dynamicDescriptorPool != VK_NULL_HANDLE)
        check(vkQueueWaitIdle(graphicsQueue),
              "vkQueueWaitIdle(retained dynamic resize)");
      releaseRetainedDynamic(retained);
      createBuffer(curveBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   retained.dynamicCurves, true);
      createBuffer(bandBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   retained.dynamicBands, true);
      const auto sizes = imageDescriptorPoolSizes();
      VkDescriptorPoolCreateInfo pool{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool.maxSets = 1;
      pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
      pool.pPoolSizes = sizes.data();
      check(vkCreateDescriptorPool(device, &pool, nullptr,
                                   &retained.dynamicDescriptorPool),
            "vkCreateDescriptorPool(retained Slug)");
      VkDescriptorSetAllocateInfo allocate{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = retained.dynamicDescriptorPool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &descriptorSetLayout;
      check(vkAllocateDescriptorSets(device, &allocate,
                                     &retained.dynamicDescriptorSet),
            "vkAllocateDescriptorSets(retained Slug)");
      writeImageDescriptorSet(retained.dynamicDescriptorSet,
                              externalImageFallback.buffer, 0,
                              externalImageFallback.size);
      writeDynamicDescriptorSet(retained.dynamicDescriptorSet,
                                retained.dynamicCurves, retained.dynamicBands);
    }
    std::memcpy(retained.dynamicCurves.mapped, curves.data(),
                static_cast<std::size_t>(curveBytes));
    std::memcpy(retained.dynamicBands.mapped, bands.data(),
                static_cast<std::size_t>(bandBytes));
    return static_cast<std::size_t>(curveBytes + bandBytes);
  }

  void releaseRetainedGeometry(RetainedGeometry &retained) {
    freeRetainedRange(retained.offset, retained.capacity);
    releaseRetainedDynamic(retained);
    retained.offset = 0;
    retained.capacity = 0;
    retained.instanceCount = 0;
    retained.shadow.clear();
    retained.shadow.shrink_to_fit();
    retained.batches.clear();
    retained.batches.shrink_to_fit();
  }

  std::size_t
  uploadRetainedRanges(VkDeviceSize destinationOffset,
                       const std::vector<Instance> &instances,
                       const std::vector<RetainedUploadRange> &ranges) {
    VkDeviceSize totalBytes = 0;
    for (const auto &range : ranges)
      totalBytes += range.count * sizeof(Instance);
    if (totalBytes == 0)
      return 0;
    const auto stagingBytes = std::min(totalBytes, retainedUploadStagingLimit);
    Buffer staging{};
    try {
      createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   staging, true);
      retainedUploadStagingPeakBytes =
          std::max(retainedUploadStagingPeakBytes,
                   static_cast<std::size_t>(stagingBytes));
      std::vector<VkBufferCopy> copies;
      copies.reserve(std::min<std::size_t>(ranges.size(), 1024));
      VkDeviceSize used = 0;
      const auto flush = [&] {
        if (copies.empty())
          return;
        const auto command = beginSingleUse();
        VkBufferMemoryBarrier writable{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        writable.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        writable.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        writable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        writable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        writable.buffer = retainedArena.buffer;
        writable.offset = destinationOffset;
        writable.size = instances.size() * sizeof(Instance);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &writable, 0, nullptr);
        vkCmdCopyBuffer(command, staging.buffer, retainedArena.buffer,
                        static_cast<std::uint32_t>(copies.size()),
                        copies.data());
        VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ready.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ready.buffer = retainedArena.buffer;
        ready.offset = destinationOffset;
        ready.size = instances.size() * sizeof(Instance);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
                             1, &ready, 0, nullptr);
        // Wait before overwriting the same bounded staging buffer.
        endSingleUse(command);
        copies.clear();
        used = 0;
      };
      for (const auto &range : ranges) {
        const VkDeviceSize start = range.first * sizeof(Instance);
        const VkDeviceSize length = range.count * sizeof(Instance);
        for (VkDeviceSize consumed = 0; consumed < length;) {
          const auto bytes = std::min(length - consumed, stagingBytes - used);
          std::memcpy(static_cast<std::byte *>(staging.mapped) + used,
                      reinterpret_cast<const std::byte *>(instances.data()) +
                          start + consumed,
                      static_cast<std::size_t>(bytes));
          copies.push_back({used, destinationOffset + start + consumed, bytes});
          used += bytes;
          consumed += bytes;
          if (used == stagingBytes)
            flush();
        }
      }
      flush();
    } catch (...) {
      destroyBuffer(staging);
      throw;
    }
    destroyBuffer(staging);
    return static_cast<std::size_t>(totalBytes);
  }

  std::size_t uploadRetainedGeometry(RetainedGeometry &retained,
                                     std::vector<Instance> &&instances) {
    retained.viewBounds = {std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()};
    for (const auto &retainedInstance : instances) {
      retained.viewBounds[0] =
          std::min(retained.viewBounds[0],
                   double(retainedInstance.positionRect[0]) +
                       std::min(double(retainedInstance.paint[3]), 0.0));
      retained.viewBounds[1] = std::min(
          retained.viewBounds[1], double(retainedInstance.positionRect[1]));
      retained.viewBounds[2] =
          std::max(retained.viewBounds[2],
                   double(retainedInstance.positionRect[2]) +
                       std::max(double(retainedInstance.paint[3]), 0.0));
      retained.viewBounds[3] = std::max(
          retained.viewBounds[3], double(retainedInstance.positionRect[3]));
    }
    const VkDeviceSize requiredBytes = instances.size() * sizeof(Instance);
    if (instances.empty()) {
      if (retained.capacity != 0) {
        check(vkQueueWaitIdle(graphicsQueue),
              "vkQueueWaitIdle(clear retained geometry)");
        releaseRetainedGeometry(retained);
      }
      return 0;
    }

    if (requiredBytes > retained.capacity) {
      const VkDeviceSize grownCapacity = alignRetainedBytes(
          retained.capacity == 0
              ? requiredBytes
              : std::max(requiredBytes,
                         retained.capacity + retained.capacity / 2));
      const VkDeviceSize oldOffset = retained.offset;
      const VkDeviceSize oldCapacity = retained.capacity;
      const VkDeviceSize newOffset = allocateRetainedRange(grownCapacity);
      try {
        uploadRetainedRanges(newOffset, instances, {{0, instances.size()}});
      } catch (...) {
        freeRetainedRange(newOffset, grownCapacity);
        throw;
      }
      if (oldCapacity != 0)
        freeRetainedRange(oldOffset, oldCapacity);
      retained.offset = newOffset;
      retained.capacity = grownCapacity;
      retained.instanceCount = static_cast<std::uint32_t>(instances.size());
      retained.shadow = std::move(instances);
      return static_cast<std::size_t>(requiredBytes);
    }

    std::vector<RetainedUploadRange> ranges;
    for (std::size_t index = 0; index < instances.size();) {
      const bool changed =
          index >= retained.shadow.size() ||
          std::memcmp(&instances[index], &retained.shadow[index],
                      sizeof(Instance)) != 0;
      if (!changed) {
        ++index;
        continue;
      }
      const std::size_t first = index++;
      while (index < instances.size()) {
        const bool nextChanged =
            index >= retained.shadow.size() ||
            std::memcmp(&instances[index], &retained.shadow[index],
                        sizeof(Instance)) != 0;
        if (!nextChanged)
          break;
        ++index;
      }
      ranges.push_back({first, index - first});
    }
    if (ranges.empty()) {
      retained.instanceCount = static_cast<std::uint32_t>(instances.size());
      retained.shadow = std::move(instances);
      return 0;
    }

    std::size_t dirtyInstances = 0;
    for (const auto &range : ranges)
      dirtyInstances += range.count;
    if (ranges.size() > 1024 && dirtyInstances * 4 > instances.size()) {
      ranges.clear();
      ranges.push_back({0, instances.size()});
      dirtyInstances = instances.size();
    }
    const auto uploaded =
        uploadRetainedRanges(retained.offset, instances, ranges);
    retained.instanceCount = static_cast<std::uint32_t>(instances.size());
    retained.shadow = std::move(instances);
    return uploaded;
  }

  void buildRetainedTextInstances(std::string_view utf8, Rect layoutBounds,
                                  TextStyle style,
                                  std::vector<Instance> &instances) const {
    instances.clear();
    if (utf8.empty() || style.size <= 0.0f || layoutBounds.width <= 0.0f)
      return;
    TextCommand command{{},
                        utf8,
                        {},
                        layoutBounds,
                        {-10000000.0f, -10000000.0f, 20000000.0f, 20000000.0f},
                        std::move(style)};
    appendText(instances, command);
  }

  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds,
                                    TextStyle style) {
    std::vector<Instance> instances;
    buildRetainedTextInstances(utf8, layoutBounds, std::move(style), instances);
    if (instances.empty())
      return 0;
    RetainedGeometry retained;
    uploadRetainedGeometry(retained, std::move(instances));
    retainedTexts.push_back(std::move(retained));
    return static_cast<RetainedTextId>(retainedTexts.size());
  }

  std::size_t updateRetainedText(RetainedTextId id, std::string_view utf8,
                                 Rect layoutBounds, TextStyle style) {
    if (id == 0 || id > retainedTexts.size())
      throw std::out_of_range("Invalid retained text id");
    std::vector<Instance> instances;
    buildRetainedTextInstances(utf8, layoutBounds, std::move(style), instances);
    return uploadRetainedGeometry(retainedTexts[id - 1U], std::move(instances));
  }

  void destroyRetainedText(RetainedTextId id) {
    if (id == 0 || id > retainedTexts.size())
      throw std::out_of_range("Invalid retained text id");
    auto& retained = retainedTexts[id - 1U];
    if (retained.capacity == 0 && retained.instanceCount == 0) return;
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(destroy retained text)");
    releaseRetainedGeometry(retained);
  }

  RetainedDrawListId createRetainedDrawList(const DrawList& list) {
    RenderIR compiled;
    compileRenderIR(list, compiled, false);
    for (const DrawBatch& batch : compiled.batches) {
      if (batch.retainedId != 0 || batch.externalImage != 0)
        throw std::invalid_argument("Retained DrawLists cannot contain retained or external resources");
    }
    if (compiled.instances.empty()) return 0;
    RetainedGeometry retained;
    uploadRetainedGeometry(retained, std::move(compiled.instances));
    uploadRetainedDynamic(retained,compiled.dynamicCurves,compiled.dynamicBandWords);
    retained.batches=std::move(compiled.batches);
    retainedDrawLists.push_back(std::move(retained));
    return static_cast<RetainedDrawListId>(retainedDrawLists.size());
  }

  std::size_t updateRetainedDrawList(RetainedDrawListId id, const DrawList& list) {
    if (id == 0 || id > retainedDrawLists.size())
      throw std::out_of_range("Invalid retained DrawList id");
    RenderIR compiled;
    compileRenderIR(list, compiled, false);
    for (const DrawBatch& batch : compiled.batches) {
      if (batch.retainedId != 0 || batch.externalImage != 0)
        throw std::invalid_argument("Retained DrawLists cannot contain retained or external resources");
    }
    auto& retained=retainedDrawLists[id-1U];
    std::size_t bytes=uploadRetainedGeometry(retained,std::move(compiled.instances));
    bytes+=uploadRetainedDynamic(retained,compiled.dynamicCurves,compiled.dynamicBandWords);
    retained.batches=std::move(compiled.batches);
    return bytes;
  }

  void destroyRetainedDrawList(RetainedDrawListId id) {
    if (id == 0 || id > retainedDrawLists.size())
      throw std::out_of_range("Invalid retained DrawList id");
    auto& retained = retainedDrawLists[id - 1U];
    if (retained.capacity == 0 && retained.instanceCount == 0) return;
    check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle(destroy retained DrawList)");
    releaseRetainedGeometry(retained);
  }

  void appendExternalImage(std::vector<Instance>& instances,
                           const ExternalImageCommand& command) const {
    if (externalImageResource(command.image) == nullptr) return;
    Instance quad{};
    const float bounds[4]{command.destination.x, command.destination.y,
                          command.destination.x + command.destination.width,
                          command.destination.y + command.destination.height};
    std::copy(std::begin(bounds), std::end(bounds), quad.positionRect);
    std::copy(std::begin(bounds), std::end(bounds), quad.emRect);
    quad.shapeData[0] = externalImageShape;
    quad.shapeData[1] = command.width;
    quad.shapeData[2] = command.height;
    quad.paint[1] = 1.0f; // The command stream applies opacity after lowering.
    quad.clip[0] = command.clip.x; quad.clip[1] = command.clip.y;
    quad.clip[2] = command.clip.width; quad.clip[3] = command.clip.height;
    instances.push_back(quad);
  }

  bool retainedViewFitsFloat(const RetainedGeometry& retained,const View2D& view)const {
    if(view.zoom<1e-7)return false;
    const double tx=view.origin.x-view.centerX*view.zoom,ty=view.origin.y-view.centerY*view.zoom;
    if(std::max(std::abs(tx),std::abs(ty))>32768)return false;
    for(int i=0;i<4;++i) {
      const double position=(retained.viewBounds[i]-(i%2?view.centerY:view.centerX))*view.zoom+(i%2?view.origin.y:view.origin.x);
      if(!std::isfinite(position)||std::abs(position)>32768)return false;
    }
    return true;
  }

  void appendRetainedTileScissors(RenderIR& ir, const RetainedGeometry& retained,
                                  const DrawBatch& original) {
    if(!config.enablePathTileCulling) { ir.batches.push_back(original);return; }
    if(viewOutlines.empty())for(const auto& [key,shape]:vectorAtlas.native().getShapes())
      viewOutlines.emplace((std::uint64_t(shape.bandTexY)<<32)|shape.bandTexX,&shape);
    for(std::uint32_t i=original.firstInstance;i<original.firstInstance+original.instanceCount;++i) {
      const auto& retainedInstance=retained.shadow.at(i);
      DrawBatch single=original;single.firstInstance=i;single.instanceCount=1;
      const auto appendUntiled=[&] {
        if(!ir.batches.empty()) {
          auto& previous=ir.batches.back();
          if(previous.retainedId==single.retainedId && previous.retainedDrawList==single.retainedDrawList &&
             !previous.projected && !previous.primitive && previous.externalImage==0 &&
             previous.additiveCoverage==single.additiveCoverage && previous.opacity==single.opacity &&
             previous.scale==single.scale && previous.translation.x==single.translation.x && previous.translation.y==single.translation.y &&
             previous.clip.x==single.clip.x && previous.clip.y==single.clip.y &&
             previous.clip.width==single.clip.width && previous.clip.height==single.clip.height &&
             previous.firstInstance+previous.instanceCount==single.firstInstance) {
            ++previous.instanceCount;return;
          }
        }
        ir.batches.push_back(single);
      };
      const double x=double(retainedInstance.positionRect[0])*original.scale+original.translation.x;
      const double y=double(retainedInstance.positionRect[1])*original.scale+original.translation.y;
      const double w=double(retainedInstance.positionRect[2]-retainedInstance.positionRect[0])*original.scale;
      const double h=double(retainedInstance.positionRect[3]-retainedInstance.positionRect[1])*original.scale;
      const std::uint64_t key=(std::uint64_t(retainedInstance.shapeData[1])<<32)|retainedInstance.shapeData[0];
      const auto shape=viewOutlines.find(key);
      if(retainedInstance.paint[3]!=0 || w<128 || h<128 || shape==viewOutlines.end() || shape->second->curves.size()<64) {
        appendUntiled();continue;
      }
      auto cached=viewTiles.find(key);
      if(cached==viewTiles.end()) {
        // Occupancy needs no clipped geometry. Admission is bounded before
        // constructing the grid; a denied shape simply keeps its original draw.
        if(viewTiles.size()>=128 || viewTileBytes>2U*1024U*1024U-256*sizeof(detail::PathTile)) {
          appendUntiled();continue;
        }
        auto tiles=detail::pathTiles(*shape->second,false);
        const auto bytes=tiles.capacity()*sizeof(detail::PathTile);
        if(bytes>2U*1024U*1024U-viewTileBytes) {appendUntiled();continue;}
        viewTileBytes+=bytes;
        cached=viewTiles.emplace(key,std::move(tiles)).first;
      }
      if(cached->second.size()>=detail::pathTileGrid*detail::pathTileGrid*3/4) {
        appendUntiled();continue;
      }
      for(const auto& tile:cached->second) {
        constexpr double grid=detail::pathTileGrid;
        // Pixel ownership is half-open and integral. Preserve original geometry
        // and the GPU transform to reduce numerical differences. GPU color
        // parity is still required before enabling this experimental path.
        // The original Slug quad extends half a pixel beyond its authored
        // bounds. Keep that fringe on the grid's outer edges; internal edges
        // remain disjoint so they cannot blend the same pixel twice.
        const double l=std::max({double(original.clip.x),0.0,std::ceil(x+tile.x/grid*w-(tile.x==0?1.0:.5))});
        const double t=std::max({double(original.clip.y),0.0,std::ceil(y+tile.y/grid*h-(tile.y==0?1.0:.5))});
        const double r=std::min({double(original.clip.x)+original.clip.width,double(extent.width),std::ceil(x+(tile.x+1)/grid*w-(tile.x+1==detail::pathTileGrid?0.0:.5))});
        const double b=std::min({double(original.clip.y)+original.clip.height,double(extent.height),std::ceil(y+(tile.y+1)/grid*h-(tile.y+1==detail::pathTileGrid?0.0:.5))});
        if(r<=l || b<=t)continue;
        auto clipped=single;clipped.clip={float(l),float(t),float(r-l),float(b-t)};
        ir.batches.push_back(clipped);
      }
    }
  }

  void appendViewedRetained(RenderIR& ir, const RetainedGeometry& retained,
                            const View2D& view, Rect clip, float opacity) {
    const double left=std::max(double(clip.x),0.0),top=std::max(double(clip.y),0.0);
    const double right=std::min(double(clip.x)+clip.width,double(extent.width));
    const double bottom=std::min(double(clip.y)+clip.height,double(extent.height));
    if(right<=left || bottom<=top)return;
    clip={float(left),float(top),float(right-left),float(bottom-top)};
    if(viewOutlines.empty())for(const auto& [key,shape]:vectorAtlas.native().getShapes())
      viewOutlines.emplace((std::uint64_t(shape.bandTexY)<<32)|shape.bandTexX,&shape);
    const auto emitBatch=[&](std::size_t first,bool additive) {
      const auto count=std::uint32_t(ir.instances.size()-first);
      if(count==0)return;
      if(!ir.batches.empty()) {
        auto& b=ir.batches.back();
        if(b.retainedId==0 && !b.primitive && !b.projected && b.externalImage==0 &&
           b.additiveCoverage==additive && b.firstInstance+b.instanceCount==first) {
          b.instanceCount+=count;return;
        }
      }
      DrawBatch b{0,false,std::uint32_t(first),count};b.additiveCoverage=additive;
      ir.batches.push_back(b);
    };
    const auto drawRange=[&](std::uint32_t start,std::uint32_t count,bool additive) {
      const auto first=ir.instances.size();
      for(std::size_t index=start;index<std::size_t(start)+count;++index) {
        const auto& source=retained.shadow.at(index);
        const double x0=(double(source.positionRect[0])-view.centerX)*view.zoom+view.origin.x;
        const double y0=(double(source.positionRect[1])-view.centerY)*view.zoom+view.origin.y;
        const double w=(double(source.positionRect[2])-source.positionRect[0])*view.zoom;
        const double h=(double(source.positionRect[3])-source.positionRect[1])*view.zoom;
        const double shear=source.paint[3]*view.zoom;
        if(!std::isfinite(x0)||!std::isfinite(y0)||!std::isfinite(w)||!std::isfinite(h))
          throw std::overflow_error("View2D transform overflow");
        if(w<=0 || h<=0 || x0+w+std::max(shear,0.0)+.5<left || x0-.5>right || y0+h+.5<top || y0-.5>bottom)continue;
        Instance quad=source;
        quad.paint[1]*=opacity;
        quad.clip[0]=clip.x;quad.clip[1]=clip.y;quad.clip[2]=clip.width;quad.clip[3]=clip.height;
        // Keep the ordinary immutable atlas path for well-conditioned quads.
        // Large quads are clipped in double BEFORE float conversion below.
        if(std::max({std::abs(x0),std::abs(y0),w,h})<=32768.0) {
          if(config.enablePathTileCulling && shear==0 && source.shapeData[0]!=0xFFFFFFF8U && w>=128 && h>=128) {
            const std::uint64_t key=(std::uint64_t(source.shapeData[1])<<32)|source.shapeData[0];
            const auto outline=viewOutlines.find(key);
            if(outline!=viewOutlines.end() && outline->second->curves.size()>=64) {
              auto found=viewTiles.find(key);
              if(found==viewTiles.end()) {
                auto tiles=detail::pathTiles(*outline->second);
                std::size_t bytes=tiles.capacity()*sizeof(detail::PathTile);
                for(const auto& tile:tiles)bytes+=tile.curves.capacity()*sizeof(QuadraticBezier2D);
                if(viewTiles.size()>=128 || bytes>2U*1024U*1024U-viewTileBytes)tiles.clear();
                else viewTileBytes+=bytes;
                found=viewTiles.emplace(key,std::move(tiles)).first;
              }
              auto& tiles=found->second;
              if(!tiles.empty() && (tiles.size()<detail::pathTileGrid*detail::pathTileGrid*3/4 || !tiles.front().curves.empty())) {
                constexpr double grid=detail::pathTileGrid;
                for(auto& tile:tiles) {
                  const double u0=tile.x/grid,v0=tile.y/grid,u1=(tile.x+1)/grid,v1=(tile.y+1)/grid;
                  const double l=x0+u0*w,t=y0+v0*h,r=x0+u1*w,b=y0+v1*h;
                  if(r<=left||l>=right||b<=top||t>=bottom)continue;
                  if(!tile.curves.empty() && !tile.attempted) {
                    // Compile normalized tile geometry once. Only a resident
                    // result is kept: low-memory profiles fall back to the
                    // original atlas instead of recompiling every frame.
                    tile.attempted=true;
                    std::vector<Instance> tempInstances;
                    std::vector<DynamicSlugCurve> tempCurves;
                    std::vector<std::uint32_t> tempBands;
                    tile.bounds=quadraticBounds(tile.curves);
                    DynamicVectorPathCommand command{tile.curves,tile.bounds,{},Paint::solid({1,1,1,1}),
                      outline->second->evenOdd?FillRule::EvenOdd:FillRule::NonZero,1,true};
                    frameDynamicShapeLocations.clear();
                    appendDynamicVectorPath(tempInstances,tempCurves,tempBands,command,false);
                    appendDynamicVectorPath(tempInstances,tempCurves,tempBands,command,false);
                    frameDynamicShapeLocations.clear();
                    if(!tempInstances.empty() && (tempInstances.back().shapeData[3]&0x80000000U)!=0) {
                      tile.resident=true;
                      const auto& compiled=tempInstances.back();
                      std::copy_n(compiled.shapeData,4,tile.shapeData.begin());
                      std::copy_n(compiled.bandTransform,4,tile.bandTransform.begin());
                      std::copy_n(compiled.emRect,4,tile.emRect.begin());
                    }
                  }
                  if(tile.resident) {
                    Instance part=quad;
                    std::copy_n(tile.shapeData.begin(),4,part.shapeData);
                    std::copy_n(tile.bandTransform.begin(),4,part.bandTransform);
                    std::copy_n(tile.emRect.begin(),4,part.emRect);
                    const double a=x0+tile.bounds.x*w,c=y0+tile.bounds.y*h;
                    const double tileRight=a+tile.bounds.width*w,tileBottom=c+tile.bounds.height*h;
                    part.positionRect[0]=float(a);part.positionRect[1]=float(c);part.positionRect[2]=float(tileRight);part.positionRect[3]=float(tileBottom);
                    part.clip[0]=float(std::max(left,l));part.clip[1]=float(std::max(top,t));
                    part.clip[2]=float(std::min(right,r)-part.clip[0]);part.clip[3]=float(std::min(bottom,b)-part.clip[1]);
                    part.shapeData[3]|=0x40000000U;
                    part.strokeWidths[0]=tile.bounds.x;part.strokeWidths[1]=tile.bounds.y;
                    part.strokeWidths[2]=tile.bounds.x+tile.bounds.width;part.strokeWidths[3]=tile.bounds.y+tile.bounds.height;
                    ir.instances.push_back(part);
                    continue;
                  }
                  Instance part=quad;
                  part.positionRect[0]=float(l);part.positionRect[1]=float(t);part.positionRect[2]=float(r);part.positionRect[3]=float(b);
                  part.emRect[0]=float(std::lerp(double(source.emRect[0]),double(source.emRect[2]),u0));
                  part.emRect[1]=float(std::lerp(double(source.emRect[1]),double(source.emRect[3]),v0));
                  part.emRect[2]=float(std::lerp(double(source.emRect[0]),double(source.emRect[2]),u1));
                  part.emRect[3]=float(std::lerp(double(source.emRect[1]),double(source.emRect[3]),v1));
                  // Disjoint screen clips assign every pixel to exactly one tile,
                  // including the half-pixel dilation around adjacent tile quads.
                  part.clip[0]=float(std::max(left,l));part.clip[1]=float(std::max(top,t));
                  part.clip[2]=float(std::min(right,r)-part.clip[0]);part.clip[3]=float(std::min(bottom,b)-part.clip[1]);
                  part.shapeData[3]|=0x40000000U;
                  part.strokeWidths[0]=float(u0);part.strokeWidths[1]=float(v0);part.strokeWidths[2]=float(u1);part.strokeWidths[3]=float(v1);
                  ir.instances.push_back(part);
                }
                continue;
              }
            }
          }
          quad.positionRect[0]=float(x0);quad.positionRect[1]=float(y0);
          quad.positionRect[2]=float(x0+w);quad.positionRect[3]=float(y0+h);
          quad.paint[3]=float(shear);
          if(quad.shapeData[0]==0xFFFFFFF8U && (quad.shapeData[3]&0x80000000U)==0) {
            // Its dynamic buffers belong to the retained descriptor set. Re-lower
            // the outline so this frame never points at another buffer's offsets.
          } else { ir.instances.push_back(quad);continue; }
        }
        std::vector<detail::DQuadratic> curves;
        const auto project=[&](double x,double y) {
          const double u=(x-source.emRect[0])/(double(source.emRect[2])-source.emRect[0]);
          const double v=(y-source.emRect[1])/(double(source.emRect[3])-source.emRect[1]);
          return detail::DPoint{std::fma(u,w,x0)+shear*(1-v),std::fma(v,h,y0)};
        };
        bool evenOdd=false;
        if(source.shapeData[0]==0xFFFFFFF8U) {
          const bool resident=(source.shapeData[3]&0x80000000U)!=0;
          const auto* words=static_cast<const std::uint32_t*>(resident?persistentSlugBands.mapped:retained.dynamicBands.mapped);
          const auto* data=static_cast<const std::byte*>(resident?persistentSlugCurves.mapped:retained.dynamicCurves.mapped);
          const auto stride=resident?persistentCurveStride():sizeof(DynamicSlugCurve);
          const auto querySize = dynamicSlugQuerySize(source.shapeData[3]);
          const std::uint32_t base = source.shapeData[2],
                              headers = (source.shapeData[3] & 0x20000000U) != 0
                                            ? 2U * querySize
                                            : (source.shapeData[3] & 255U) +
                                                  ((source.shapeData[3] >> 8U) & 255U) + 2;
          std::uint32_t curveCount=0;
          for(std::uint32_t j=0;j<headers;++j) {
            const std::uint32_t headerBase =
                2U * querySize;
            const auto n=words[base+headerBase+2*j],offset=words[base+headerBase+1+2*j];
            for(std::uint32_t k=0;k<n;++k)curveCount=std::max(curveCount,detail::atlasBandCurveIndex(words,offset,k,(source.shapeData[3]&detail::packedAtlasBandFlag)!=0)+1);
          }
          for(std::uint32_t j=0;j<curveCount;++j) {
            DynamicSlugCurve q{};
            std::memcpy(&q,data+(std::size_t(source.shapeData[1])+j)*stride,stride);
            curves.push_back({project(q.p12[0],q.p12[1]),project(q.p12[2],q.p12[3]),project(q.p3[0],q.p3[1])});
          }
          evenOdd=(source.shapeData[3]&0x10000U)!=0;
        } else {
          const auto shape=viewOutlines.find((std::uint64_t(source.shapeData[1])<<32)|source.shapeData[0]);
          if(shape==viewOutlines.end())throw std::invalid_argument("View2D requires vector outlines");
          for(const auto& q:shape->second->curves)
            curves.push_back({project(q.x1,q.y1),project(q.x2,q.y2),project(q.x3,q.y3)});
          evenOdd=shape->second->evenOdd;
        }
        Rect expanded{clip.x-2,clip.y-2,clip.width+4,clip.height+4};
        auto visible=detail::clipViewport(curves,expanded);
        if(visible.empty())continue;
        Paint paint;
        paint.kind=static_cast<GradientKind>(int(source.paint[0]));
        paint.start={source.color0[0],source.color0[1],source.color0[2],source.color0[3]};
        paint.end={source.color1[0],source.color1[1],source.color1[2],source.color1[3]};
        paint.opacity=source.paint[1]*opacity;paint.shaderParameter=source.paint[2];
        paint.origin={source.gradient[0],source.gradient[1]};paint.target={source.gradient[2],source.gradient[3]};
        DynamicVectorPathCommand command{std::move(visible),{},clip,paint,evenOdd?FillRule::EvenOdd:FillRule::NonZero};
        command.bounds=quadraticBounds(command.curves);
        const auto before=ir.instances.size();
        appendDynamicVectorPath(ir.instances,ir.dynamicCurves,ir.dynamicBandWords,command);
        if(ir.instances.size()>before) {
          auto& result=ir.instances.back();
          result.shapeData[3]|=0x40000000U; // Original paint UV despite viewport clipping.
          result.strokeWidths[0]=float((double(result.positionRect[0])-x0)/w);
          result.strokeWidths[1]=float((double(result.positionRect[1])-y0)/h);
          result.strokeWidths[2]=float((double(result.positionRect[2])-x0)/w);
          result.strokeWidths[3]=float((double(result.positionRect[3])-y0)/h);
        }
      }
      emitBatch(first,additive);
    };
    if(retained.batches.empty())drawRange(0,retained.instanceCount,false);
    else for(const auto& batch:retained.batches)drawRange(batch.firstInstance,batch.instanceCount,batch.additiveCoverage);
  }

  void compileRenderIR(const DrawList& list, RenderIR& ir, bool forFrame) {
    ir.clear();
    frameDynamicShapeLocations.clear();
    auto& instances = ir.instances;
    // Retained geometry receives its final transform/clip when it is drawn. Early
    // clip culling and compact framebuffer-coordinate instances apply only to frames.
    auto* primitives = forFrame ? &ir.primitives : nullptr;
    auto& dynamicCurves = ir.dynamicCurves;
    auto& dynamicBandWords = ir.dynamicBandWords;
    auto& drawBatches = ir.batches;
    const std::size_t commandCount = list.commands().size() + list.overlayCommands().size();
    instances.reserve(commandCount);
    if (primitives) primitives->reserve(commandCount);
    drawBatches.reserve(commandCount);
    const auto appendCommands = [&](const auto& commands) {
      for (std::size_t commandIndex = 0; commandIndex < commands.size(); ++commandIndex) {
        const auto& display = commands[commandIndex];
        if (const auto* projected = std::get_if<ProjectedDrawListCommand>(&display)) {
          if(!projected->content||projected->opacity<=0||projected->clip.width<=0||projected->clip.height<=0)continue;
          if(!forFrame)throw std::invalid_argument("A projected DrawList cannot be retained or nested");
          RenderIR content;
          compileRenderIR(*projected->content,content,false);
          const auto instanceBase=static_cast<std::uint32_t>(instances.size());
          const auto curveBase=static_cast<std::uint32_t>(dynamicCurves.size());
          const auto bandBase=static_cast<std::uint32_t>(dynamicBandWords.size());
          dynamicCurves.insert(dynamicCurves.end(),content.dynamicCurves.begin(),content.dynamicCurves.end());
          dynamicBandWords.insert(dynamicBandWords.end(),content.dynamicBandWords.begin(),content.dynamicBandWords.end());
          std::set<std::uint32_t> rebased;
          for(auto& quad:content.instances){
            quad.paint[1]*=projected->opacity;
            if(quad.shapeData[0]==0xFFFFFFF8U&&(quad.shapeData[3]&0x80000000u)==0){
              const auto block=quad.shapeData[2];
              if(rebased.insert(block).second){
                const auto headers=(quad.shapeData[3]&255u)+((quad.shapeData[3]>>8u)&255u)+2u;
                for(std::uint32_t header=0;header<headers;++header)
                  dynamicBandWords[bandBase+block+64u+2u*header+1u]+=bandBase;
              }
              quad.shapeData[1]+=curveBase;quad.shapeData[2]+=bandBase;
            }
            instances.push_back(quad);
          }
          for(auto& batch:content.batches){
            if(batch.retainedId==0)batch.firstInstance+=instanceBase;
            else batch.opacity*=projected->opacity;
            batch.projected=true;batch.projective=projected->transform;batch.projectiveClip=projected->clip;
            drawBatches.push_back(std::move(batch));
          }
          // Locations from the child refer to a different curve/band stream.
          frameDynamicShapeLocations.clear();
          continue;
        }
        if (const auto* retainedCommand = std::get_if<RetainedTextCommand>(&display)) {
          if (retainedCommand->text == 0 || retainedCommand->text > retainedTexts.size())
            continue;
          const RetainedGeometry& retained = retainedTexts[retainedCommand->text - 1U];
          if (retained.instanceCount == 0)
            continue;
          if(retainedCommand->view) {
            if(!forFrame)throw std::invalid_argument("A View2D cannot be retained or nested");
            if(retainedViewFitsFloat(retained,*retainedCommand->view)) {
              const auto& view=*retainedCommand->view;
              const Vec2 position{float(view.origin.x-view.centerX*view.zoom),float(view.origin.y-view.centerY*view.zoom)};
              appendRetainedTileScissors(ir,retained,{retainedCommand->text,false,0,retained.instanceCount,position,float(view.zoom),retainedCommand->clip,retainedCommand->opacity});
              continue;
            }
            appendViewedRetained(ir,retained,*retainedCommand->view,retainedCommand->clip,retainedCommand->opacity);
            continue;
          }
          drawBatches.push_back({retainedCommand->text, false, 0, retained.instanceCount,
                                 retainedCommand->position, retainedCommand->scale,
                                 retainedCommand->clip, retainedCommand->opacity});
          continue;
        }
        if (const auto* retainedCommand = std::get_if<RetainedDrawListCommand>(&display)) {
          if (retainedCommand->drawList == 0 || retainedCommand->drawList > retainedDrawLists.size())
            continue;
          const RetainedGeometry& retained = retainedDrawLists[retainedCommand->drawList - 1U];
          if (retained.instanceCount == 0) continue;
          if(retainedCommand->view) {
            if(!forFrame)throw std::invalid_argument("A View2D cannot be retained or nested");
            if(retainedViewFitsFloat(retained,*retainedCommand->view)) {
              const auto& view=*retainedCommand->view;
              const Vec2 position{float(view.origin.x-view.centerX*view.zoom),float(view.origin.y-view.centerY*view.zoom)};
              for(const auto& source:retained.batches) {
                DrawBatch batch{retainedCommand->drawList,true,source.firstInstance,source.instanceCount,position,float(view.zoom),retainedCommand->clip,retainedCommand->opacity};
                batch.additiveCoverage=source.additiveCoverage;appendRetainedTileScissors(ir,retained,batch);
              }
              continue;
            }
            appendViewedRetained(ir,retained,*retainedCommand->view,retainedCommand->clip,retainedCommand->opacity);
            continue;
          }
          for(const auto& source:retained.batches){
            DrawBatch batch{retainedCommand->drawList,true,source.firstInstance,source.instanceCount,
                            retainedCommand->position,retainedCommand->scale,retainedCommand->clip,retainedCommand->opacity};
            batch.additiveCoverage=source.additiveCoverage;drawBatches.push_back(batch);
          }
          continue;
        }
        const std::size_t first = instances.size();
        const std::size_t primitiveFirst = primitives ? primitives->size() : 0;
        bool emittedPrimitive = false;
        ExternalImageId externalImage = 0;
        bool additiveCoverage=false;
        if (const auto* shapeCommand = std::get_if<DrawCommand>(&display))
          appendShape(instances, shapeCommand->shape, shapeCommand->destination,
                      shapeCommand->paint, shapeCommand->clip, shapeCommand->italicShear);
        else if (const auto* textCommand = std::get_if<TextCommand>(&display))
          appendText(instances, *textCommand);
        else if (const auto* roundedCommand = std::get_if<RoundedRectCommand>(&display)) {
          if (primitives && compactRectangleCompatible(*roundedCommand)) {
            appendCompactRectangle(*primitives, *roundedCommand);
            emittedPrimitive = primitives->size() != primitiveFirst;
          } else {
            appendRoundedRectSlug(instances, dynamicCurves, dynamicBandWords, *roundedCommand, forFrame);
          }
        }
        else if (const auto* lineCommand = std::get_if<LineCommand>(&display))
          appendLine(instances, dynamicCurves, dynamicBandWords, *lineCommand, forFrame);
        else if (const auto* dynamicPath = std::get_if<DynamicVectorPathCommand>(&display)) {
          appendDynamicVectorPath(instances, dynamicCurves, dynamicBandWords, *dynamicPath, forFrame);
          additiveCoverage=dynamicPath->additiveCoverage;
        }
        else if (const auto* cubicCommand = std::get_if<CubicBezierCommand>(&display))
          appendCubicBezier(instances, dynamicCurves, dynamicBandWords, *cubicCommand, forFrame);
        else if (const auto* arcCommand = std::get_if<ArcCommand>(&display))
          appendArc(instances, dynamicCurves, dynamicBandWords, *arcCommand, forFrame);
        else if (const auto* imageCommand = std::get_if<ExternalImageCommand>(&display)) {
          appendExternalImage(instances, *imageCommand);
          externalImage = imageCommand->image;
        }
        if (emittedPrimitive) {
          const auto count = static_cast<std::uint32_t>(primitives->size() - primitiveFirst);
          if (!drawBatches.empty() && drawBatches.back().retainedId == 0 &&
              drawBatches.back().primitive &&
              drawBatches.back().firstInstance + drawBatches.back().instanceCount == primitiveFirst) {
            drawBatches.back().instanceCount += count;
          } else {
            DrawBatch batch{0, false, static_cast<std::uint32_t>(primitiveFirst), count};
            batch.primitive = true;
            drawBatches.push_back(batch);
          }
          continue;
        }
        const float opacity = std::visit([](const auto& command) { return command.opacity; }, display);
        if (opacity < 1.0f)
          for (std::size_t i = first; i < instances.size(); ++i) instances[i].paint[1] *= opacity;
        const std::uint32_t count = static_cast<std::uint32_t>(instances.size() - first);
        if (count == 0)
          continue;
        if (!drawBatches.empty() && drawBatches.back().retainedId == 0 &&
            !drawBatches.back().primitive &&
            !drawBatches.back().projected &&
            drawBatches.back().additiveCoverage==additiveCoverage &&
            drawBatches.back().externalImage == externalImage &&
            drawBatches.back().firstInstance + drawBatches.back().instanceCount == first) {
          drawBatches.back().instanceCount += count;
        } else {
          DrawBatch batch{0, false, static_cast<std::uint32_t>(first), count};
          batch.externalImage = externalImage;
          batch.additiveCoverage=additiveCoverage;
          drawBatches.push_back(batch);
        }
      }
    };
    appendCommands(list.commands());
    appendCommands(list.overlayCommands());
  }

  void ensureCapacity(Buffer& buffer, VkDeviceSize required, VkBufferUsageFlags usage) {
    if (required <= buffer.size)
      return;
    const VkDeviceSize limit = (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      ? deviceProperties.limits.maxStorageBufferRange : std::numeric_limits<VkDeviceSize>::max();
    if (required > limit) throw std::length_error("Buffer exceeds the device storage range");
    VkDeviceSize newSize = std::max<VkDeviceSize>(buffer.size, 4);
    while (newSize < required) {
      if (newSize > limit / 2) { newSize = required; break; }
      newSize *= 2;
    }
    Buffer replacement;
    try {
      createBuffer(newSize, usage,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   replacement, true);
    } catch (...) {
      destroyBuffer(replacement);
      throw;
    }
    destroyBuffer(buffer);
    buffer = replacement;
  }

  void record(VkCommandBuffer command, std::uint32_t imageIndex, Frame& frame,
              std::uint64_t submissionSerial) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(frame)");
    if (frame.writeTimestamps) {
      vkCmdResetQueryPool(command, frame.timestamps, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestamps, 0);
    }
    if (frameRecorder != nullptr)
      frameRecorder(frameRecorderContext,
                    {command, extent.width, extent.height, submissionSerial});
    Color clearColor = config.clearColor;
    if (isSrgbFormat(swapchainFormat)) {
      clearColor.r = srgbToLinear(clearColor.r);
      clearColor.g = srgbToLinear(clearColor.g);
      clearColor.b = srgbToLinear(clearColor.b);
    }
    VkClearValue clear{{{clearColor.r, clearColor.g, clearColor.b, clearColor.a}}};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = renderPass;
    render.framebuffer = framebuffers[imageIndex];
    render.renderArea.extent = extent;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    if (!frameIR.batches.empty()) {
      VkViewport viewport{
          0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
          0.0f, 1.0f};
      vkCmdSetViewport(command, 0, 1, &viewport);
      VkPipeline currentPipeline = VK_NULL_HANDLE;
      VkDescriptorSet currentDescriptorSet = VK_NULL_HANDLE;
      for (const DrawBatch& batch : frameIR.batches) {
        const bool retained = batch.retainedId != 0;
        const RetainedGeometry* retainedGeometry = retained
          ? (batch.retainedDrawList ? &retainedDrawLists[batch.retainedId - 1U]
                                    : &retainedTexts[batch.retainedId - 1U])
          : nullptr;
        VkDescriptorSet desiredDescriptorSet = descriptorSet;
        if (retainedGeometry != nullptr &&
            retainedGeometry->dynamicDescriptorSet != VK_NULL_HANDLE) {
          desiredDescriptorSet = retainedGeometry->dynamicDescriptorSet;
        } else if (batch.externalImage != 0) {
          const auto* resource = externalImageResource(batch.externalImage);
          if (resource == nullptr) continue;
          desiredDescriptorSet = resource->descriptorSet;
        }
        if (desiredDescriptorSet != currentDescriptorSet) {
          vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                  0, 1, &desiredDescriptorSet, 0, nullptr);
          currentDescriptorSet = desiredDescriptorSet;
        }
        const VkPipeline desiredPipeline = batch.primitive ? primitivePipeline :
          batch.additiveCoverage ? additivePipeline : pipeline;
        if (desiredPipeline != currentPipeline) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
          currentPipeline = desiredPipeline;
        }
        const Buffer& buffer = batch.primitive
          ? frame.primitiveInstances
          : retained ? retainedArena : frame.instances;
        const VkDeviceSize vertexOffset = retainedGeometry ? retainedGeometry->offset : 0;
        VkRect2D scissor{{0, 0}, extent};
        if (retained || batch.projected) {
          const Rect screenClip=batch.projected?batch.projectiveClip:batch.clip;
          const float left = std::clamp(screenClip.x, 0.0f, static_cast<float>(extent.width));
          const float top = std::clamp(screenClip.y, 0.0f, static_cast<float>(extent.height));
          const float right =
              std::clamp(screenClip.x + screenClip.width, left, static_cast<float>(extent.width));
          const float bottom =
              std::clamp(screenClip.y + screenClip.height, top, static_cast<float>(extent.height));
          if (right <= left || bottom <= top)
            continue;
          scissor.offset = {static_cast<std::int32_t>(std::floor(left)),
                            static_cast<std::int32_t>(std::floor(top))};
          scissor.extent = {static_cast<std::uint32_t>(std::ceil(right) - std::floor(left)),
                            static_cast<std::uint32_t>(std::ceil(bottom) - std::floor(top))};
        }
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindVertexBuffers(command, 0, 1, &buffer.buffer, &vertexOffset);
        PushConstants push{{static_cast<float>(extent.width), static_cast<float>(extent.height),
                            retained ? batch.scale : 1.0f, retained ? batch.scale : 1.0f},
                            {retained ? batch.translation.x : 0.0f,
                             retained ? batch.translation.y : 0.0f, retained ? 1.0f : 0.0f,
                             isSrgbFormat(swapchainFormat) ? 1.0f : 0.0f},
                           {batch.clip.x, batch.clip.y, batch.clip.width, batch.clip.height},
                           {batch.opacity, 0.0f, 0.0f, 0.0f}};
        if(batch.projected){
          push.opacity[1]=1.0f;
          for(std::size_t row=0;row<3;++row)
            for(std::size_t column=0;column<3;++column)
              push.projectiveRows[row][column]=batch.projective.matrix[row*3+column];
          push.projectiveRows[0][3]=batch.projective.clipDepth.x;
          push.projectiveRows[1][3]=batch.projective.clipDepth.y;
          push.projectiveRows[2][3]=batch.projective.clipDepth.z;
          const float clip[4]{batch.projectiveClip.x,batch.projectiveClip.y,batch.projectiveClip.width,batch.projectiveClip.height};
          std::copy(std::begin(clip),std::end(clip),push.projectiveClip);
        }
        vkCmdPushConstants(command, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        if (config.enableIndexedVectorQuads && !batch.primitive) {
          vkCmdBindIndexBuffer(command, vectorQuadIndices.buffer, 0, VK_INDEX_TYPE_UINT16);
          vkCmdDrawIndexed(command, 6, batch.instanceCount, 0, 0, batch.firstInstance);
        } else {
          vkCmdDraw(command, 6, batch.instanceCount, 0, batch.firstInstance);
        }
      }
    }
    vkCmdEndRenderPass(command);
    if (captureFrame) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = swapchainImages[imageIndex];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {extent.width, extent.height, 1};
      vkCmdCopyImageToBuffer(command, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &region);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    if (frame.writeTimestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestamps, 1);
    check(vkEndCommandBuffer(command), "vkEndCommandBuffer(frame)");
  }

  void refreshCompletedSubmissions() noexcept {
    if (device == VK_NULL_HANDLE) return;
    for (std::size_t index = 0; index < framesInFlight; ++index) {
      const auto serial = frameSubmissionSerial[index];
      if (serial != 0 && vkGetFenceStatus(device, frames[index].fence) == VK_SUCCESS) {
        completedSubmissionSerial = std::max(completedSubmissionSerial, serial);
        frameSubmissionSerial[index] = 0;
      }
    }
  }

  void resolveGpuTimestamp(Frame& frame) {
    if (!frame.timestamps || !frame.timestampsWritten) return;
    std::uint64_t ticks[2]{};
    check(vkGetQueryPoolResults(device, frame.timestamps, 0, 2, sizeof(ticks), ticks,
                                sizeof(std::uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults(timestamp)");
    const std::uint64_t timestampMask = timestampValidBits >= 64
                                            ? ~std::uint64_t{0}
                                            : (std::uint64_t{1} << timestampValidBits) - 1;
    const std::uint64_t elapsedTicks = (ticks[1] - ticks[0]) & timestampMask;
    statistics.gpuMilliseconds = static_cast<float>(elapsedTicks) *
                                 deviceProperties.limits.timestampPeriod / 1'000'000.0f;
    statistics.gpuSampleSubmissionSerial = frame.timestampSubmissionSerial;
    frame.timestampsWritten = false;
  }

  void prepareFrame() {
    if (framePrepared)
      return;
    if (config.enableCpuPhaseTiming) {
      statistics.cpuPhases = {};
      statistics.cpuPhases.enabled = true;
    }
    const Vec2 framebuffer = platformSurface.framebufferSize();
    if (framebuffer.x > 0.0f && framebuffer.y > 0.0f &&
        (static_cast<std::uint32_t>(framebuffer.x) != extent.width ||
         static_cast<std::uint32_t>(framebuffer.y) != extent.height))
      recreateSwapchain();
    while (!framePrepared) {
      Frame& frame = frames[currentFrame];
      {
        OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.fenceWaitMilliseconds);
        check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences(frame)");
      }
      if (frameSubmissionSerial[currentFrame] != 0) {
        completedSubmissionSerial = std::max(completedSubmissionSerial,
                                             frameSubmissionSerial[currentFrame]);
        frameSubmissionSerial[currentFrame] = 0;
      }
      if (frame.timestamps && frame.timestampsWritten) {
        OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.queryReadMilliseconds);
        resolveGpuTimestamp(frame);
      }
      {
        OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.acquireMilliseconds);
        preparedAcquireResult = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &preparedImageIndex);
      }
      if (preparedAcquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        continue;
      }
      if (preparedAcquireResult != VK_SUCCESS && preparedAcquireResult != VK_SUBOPTIMAL_KHR)
        check(preparedAcquireResult, "vkAcquireNextImageKHR");
      framePrepared = true;
    }
  }

  void draw(const DrawList& list) {
    prepareFrame();
    Frame& frame = frames[currentFrame];
    const std::uint32_t imageIndex = preparedImageIndex;

    if (captureFrame) {
      if (swapchainFormat != VK_FORMAT_B8G8R8A8_SRGB && swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM &&
          swapchainFormat != VK_FORMAT_R8G8B8A8_SRGB && swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Readback requires an RGBA8/BGRA8 swapchain");
      ensureCapacity(readback, static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    const auto cpuBuildStart = std::chrono::steady_clock::now();
    persistentSlugUploadedBytes = 0;
    compileRenderIR(list, frameIR, true);
    const auto cpuBuildEnd = std::chrono::steady_clock::now();
    ensureCapacity(frame.instances, frameIR.instances.size() * sizeof(Instance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensureCapacity(frame.primitiveInstances, frameIR.primitives.size() * sizeof(PrimitiveInstance),
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensureCapacity(frame.dynamicCurves,
                   std::max<std::size_t>(1,frameIR.dynamicCurves.size())*sizeof(DynamicSlugCurve),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensureCapacity(frame.dynamicBands,
                   std::max<std::size_t>(1,frameIR.dynamicBandWords.size())*sizeof(std::uint32_t),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!frameIR.instances.empty())
      std::memcpy(frame.instances.mapped, frameIR.instances.data(),
                  frameIR.instances.size() * sizeof(Instance));
    if (!frameIR.primitives.empty())
      std::memcpy(frame.primitiveInstances.mapped, frameIR.primitives.data(),
                  frameIR.primitives.size() * sizeof(PrimitiveInstance));
    if(!frameIR.dynamicCurves.empty())
      std::memcpy(frame.dynamicCurves.mapped,frameIR.dynamicCurves.data(),
                  frameIR.dynamicCurves.size()*sizeof(DynamicSlugCurve));
    if(!frameIR.dynamicBandWords.empty())
      std::memcpy(frame.dynamicBands.mapped,frameIR.dynamicBandWords.data(),
                  frameIR.dynamicBandWords.size()*sizeof(std::uint32_t));
    writeDynamicDescriptorSet(descriptorSet,frame.dynamicCurves,frame.dynamicBands);
    for(auto& resource:externalImages)
      if(resource.live)
        writeDynamicDescriptorSet(resource.descriptorSet,frame.dynamicCurves,frame.dynamicBands);
    const auto cpuUploadEnd = std::chrono::steady_clock::now();

    frame.writeTimestamps = config.gpuTimingInterval > 0 && frame.timestamps != VK_NULL_HANDLE &&
                            submittedFrameCount % config.gpuTimingInterval == 0;
    const std::uint64_t submissionSerial = submittedFrameCount + 1;
    {
      OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.recordMilliseconds);
      check(vkResetFences(device, 1, &frame.fence), "vkResetFences");
      check(vkResetCommandBuffer(commandBuffers[currentFrame], 0), "vkResetCommandBuffer");
      record(commandBuffers[currentFrame], imageIndex, frame, submissionSerial);
    }
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffers[currentFrame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished[imageIndex];
    {
      OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.queueSubmitMilliseconds);
      check(vkQueueSubmit(graphicsQueue, 1, &submit, frame.fence), "vkQueueSubmit(frame)");
    }
    frameSubmissionSerial[currentFrame] = submissionSerial;
    frame.timestampsWritten = frame.writeTimestamps;
    frame.timestampSubmissionSerial = frame.writeTimestamps ? submissionSerial : 0;
    if (captureFrame) {
      check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(readback)");
      completedSubmissionSerial = std::max(completedSubmissionSerial, submissionSerial);
      captured.width = extent.width;
      captured.height = extent.height;
      captured.rgba.resize(static_cast<std::size_t>(extent.width) * extent.height * 4);
      std::memcpy(captured.rgba.data(), readback.mapped, captured.rgba.size());
      if (swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB || swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM)
        for (std::size_t i = 0; i < captured.rgba.size(); i += 4)
          std::swap(captured.rgba[i], captured.rgba[i + 2]);
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    VkResult presented;
    {
      OptionalCpuPhaseTimer timer(config.enableCpuPhaseTiming, statistics.cpuPhases.queuePresentMilliseconds);
      presented = vkQueuePresentKHR(presentQueue, &present);
    }
    const bool needsRecreate = presented == VK_ERROR_OUT_OF_DATE_KHR;
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR && !needsRecreate)
      check(presented, "vkQueuePresentKHR");
    const auto cpuSubmitEnd = std::chrono::steady_clock::now();

    std::uint32_t retainedQuadCount = 0;
    for (const DrawBatch& batch : frameIR.batches)
      if (batch.retainedId != 0)
        retainedQuadCount += batch.instanceCount;
    statistics.quads = static_cast<std::uint32_t>(
      frameIR.instances.size() + frameIR.primitives.size()) + retainedQuadCount;
    statistics.retainedQuads = retainedQuadCount;
    statistics.primitiveQuads = static_cast<std::uint32_t>(frameIR.primitives.size());
    statistics.drawCalls = static_cast<std::uint32_t>(frameIR.batches.size());
    statistics.uploadedBytes = frameIR.instances.size() * sizeof(Instance) +
                               frameIR.primitives.size() * sizeof(PrimitiveInstance) +
                               frameIR.dynamicCurves.size() * sizeof(DynamicSlugCurve) +
                               frameIR.dynamicBandWords.size() * sizeof(std::uint32_t) +
                               persistentSlugUploadedBytes;
    statistics.cpuBuildMilliseconds =
        std::chrono::duration<float, std::milli>(cpuBuildEnd - cpuBuildStart).count();
    statistics.cpuUploadMilliseconds =
        std::chrono::duration<float, std::milli>(cpuUploadEnd - cpuBuildEnd).count();
    statistics.cpuSubmitMilliseconds =
        std::chrono::duration<float, std::milli>(cpuSubmitEnd - cpuUploadEnd).count();
    statistics.dynamicPathCacheBytes = dynamicSlugPathCache.bytes();
    statistics.dynamicGeometryCacheBytes = dynamicGeometryCache.bytes();
    statistics.persistentGeometryAllocatedBytes =
      static_cast<std::size_t>(persistentSlugCurves.allocatedBytes + persistentSlugBands.allocatedBytes);
    statistics.persistentGeometryBufferBytes =
      static_cast<std::size_t>(persistentSlugCurves.size + persistentSlugBands.size);
    statistics.dynamicPathCacheHits = dynamicSlugCacheHits;
    statistics.dynamicPathCacheMisses = dynamicSlugCacheMisses;
    ++submittedFrameCount;
    framePrepared = false;
    currentFrame = (currentFrame + 1) % framesInFlight;
    if (needsRecreate)
      recreateSwapchain();
    if (config.enableCpuPhaseTiming)
      statistics.cpuPhases.postPresentMilliseconds = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - cpuSubmitEnd).count();
  }

  void destroy() noexcept {
    if (device)
      vkDeviceWaitIdle(device);
    if (device) {
      destroyBuffer(readback);
      for(auto& retained:retainedTexts) releaseRetainedDynamic(retained);
      for(auto& retained:retainedDrawLists) releaseRetainedDynamic(retained);
      retainedTexts.clear();
      retainedDrawLists.clear();
      retainedFreeRanges.clear();
      retainedArenaUsed = 0;
      destroyBuffer(retainedArena);
      for (auto& frame : frames) {
        destroyBuffer(frame.instances);
        destroyBuffer(frame.primitiveInstances);
        destroyBuffer(frame.dynamicCurves);
        destroyBuffer(frame.dynamicBands);
        if (frame.timestamps)
          vkDestroyQueryPool(device, frame.timestamps, nullptr);
        if (frame.imageAvailable)
          vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        if (frame.fence)
          vkDestroyFence(device, frame.fence, nullptr);
        frame = {};
      }
      destroySwapchainResources();
      for (auto& external : externalImages) {
        if (external.pool)
          vkDestroyDescriptorPool(
              device, external.pool, nullptr);
        destroyBuffer(external.ownedBuffer);
        external = {};
      }
      externalImages.clear();
      if (descriptorPool)
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      if (descriptorSetLayout)
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
      destroyBuffer(externalImageFallback);
      destroyBuffer(persistentSlugCurves);
      destroyBuffer(persistentSlugBands);
      destroyBuffer(vectorQuadIndices);
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
      destroyTexture(curveTexture);
      destroyTexture(bandTexture);
      if (commandPool)
        vkDestroyCommandPool(device, commandPool, nullptr);
      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (surface && instance)
      vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (debugMessenger && instance) {
      auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroyDebug)
        destroyDebug(instance, debugMessenger, nullptr);
    }
    debugMessenger = VK_NULL_HANDLE;
    if (instance)
      vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
  }
};

VulkanRenderer::VulkanRenderer(PlatformSurface& surface, const VectorAtlas& atlas,
                               const RendererConfig& config)
    : impl_(std::make_unique<Impl>(surface, atlas, config)),
      interop_(std::unique_ptr<VulkanInterop>(new VulkanInterop(*this))) {}
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
VulkanRenderer::VulkanRenderer(Window& window, const VectorAtlas& atlas,
                               const RendererConfig& config)
    : impl_(std::make_unique<Impl>(std::make_unique<GlfwPlatformSurface>(window), atlas, config)),
      interop_(std::unique_ptr<VulkanInterop>(new VulkanInterop(*this))) {}
#endif
VulkanRenderer::~VulkanRenderer() = default;
void VulkanRenderer::prepareFrame() {
  impl_->prepareFrame();
}
RetainedTextId VulkanRenderer::createRetainedText(std::string_view utf8, Rect layoutBounds,
                                                  TextStyle style) {
  return impl_->createRetainedText(utf8, layoutBounds, std::move(style));
}
std::size_t VulkanRenderer::updateRetainedText(RetainedTextId text, std::string_view utf8,
                                               Rect layoutBounds, TextStyle style) {
  return impl_->updateRetainedText(text, utf8, layoutBounds, std::move(style));
}
void VulkanRenderer::destroyRetainedText(RetainedTextId text) {
  impl_->destroyRetainedText(text);
}
RetainedDrawListId VulkanRenderer::createRetainedDrawList(const DrawList& list) {
  return impl_->createRetainedDrawList(list);
}
std::size_t VulkanRenderer::updateRetainedDrawList(RetainedDrawListId drawList,
                                                   const DrawList& list) {
  return impl_->updateRetainedDrawList(drawList, list);
}
void VulkanRenderer::destroyRetainedDrawList(RetainedDrawListId drawList) {
  impl_->destroyRetainedDrawList(drawList);
}
void VulkanRenderer::draw(const DrawList& list) {
  impl_->draw(list);
}
FramePixels VulkanRenderer::drawAndReadback(const DrawList& list) {
  if (!impl_->config.enableReadback) throw std::runtime_error("Readback was not enabled");
  impl_->captureFrame = true;
  try { impl_->draw(list); }
  catch (...) { impl_->captureFrame = false; throw; }
  impl_->captureFrame = false;
  return std::move(impl_->captured);
}
void VulkanRenderer::waitIdle() {
  if (impl_->device) {
    check(vkDeviceWaitIdle(impl_->device), "vkDeviceWaitIdle");
    if (impl_->config.enableCpuPhaseTiming)
      for (auto& frame : impl_->frames) impl_->resolveGpuTimestamp(frame);
    impl_->completedSubmissionSerial = impl_->submittedFrameCount;
    impl_->frameSubmissionSerial.fill(0);
  }
}
RendererStats VulkanRenderer::stats() const {
  auto result = impl_->statistics;
  result.validationEnabled = impl_->debugMessenger != VK_NULL_HANDLE;
  result.validationErrors = impl_->validationErrors.load(std::memory_order_relaxed);
  result.atlasStorageShapeCount = static_cast<std::uint32_t>(impl_->atlasStorageShapes.size());
  result.atlasRefinedShapeCount = impl_->atlasRefinedShapeCount;
  result.atlasPackedBandShapeCount = impl_->atlasPackedBandShapeCount;
  result.atlasRefinedShapesByCells = impl_->atlasRefinedShapesByCells;
  result.atlasStorageCurveBytes = impl_->atlasStorageCurveBytes;
  result.atlasStorageBandBytes = impl_->atlasStorageBandBytes;
  result.retainedInstanceStrideBytes = sizeof(Instance);
  result.retainedUploadStagingLimitBytes = retainedUploadStagingLimit;
  result.retainedUploadStagingPeakBytes = impl_->retainedUploadStagingPeakBytes;
  result.cpuPhases.enabled = impl_->config.enableCpuPhaseTiming;
  result.lastSubmittedSerial = impl_->submittedFrameCount;
  result.maxFramesInFlight = static_cast<std::uint32_t>(framesInFlight);
  result.gpuTimestampPeriodNanoseconds = impl_->deviceProperties.limits.timestampPeriod;
  result.gpuTimestampValidBits = impl_->timestampValidBits;
  result.apiVersion = impl_->deviceProperties.apiVersion;
  result.driverVersion = impl_->deviceProperties.driverVersion;
  result.vendorId = impl_->deviceProperties.vendorID;
  result.deviceId = impl_->deviceProperties.deviceID;
  return result;
}
const char* VulkanRenderer::deviceName() const {
  return impl_->deviceProperties.deviceName;
}
VulkanInterop& VulkanRenderer::vulkanInterop() noexcept { return *interop_; }

const char* VulkanRenderer::presentModeName() const {
  switch (impl_->swapchainPresentMode) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR:
    return "IMMEDIATE";
  case VK_PRESENT_MODE_MAILBOX_KHR:
    return "MAILBOX";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    return "FIFO_RELAXED";
  default:
    return "FIFO";
  }
}

VulkanDeviceContext VulkanInterop::deviceContext() const noexcept {
  const auto& impl = *renderer_->impl_;
  return {impl.physicalDevice, impl.device, impl.graphicsQueue,
          impl.queueFamilies.graphics.value_or(0)};
}

std::uint64_t VulkanInterop::completedSubmissionSerial() const noexcept {
  renderer_->impl_->refreshCompletedSubmissions();
  return renderer_->impl_->completedSubmissionSerial;
}

ExternalImageId VulkanInterop::registerRgba32fBuffer(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) {
  return renderer_->impl_->registerExternalImage(
      buffer, offset, range);
}

ExternalImageId VulkanInterop::createOwnedRgba32fImage(
    std::uint32_t width, std::uint32_t height) {
  return renderer_->impl_->createOwnedExternalImage(
      width, height);
}

bool VulkanInterop::updateOwnedRgba32fImage(
    ExternalImageId image,
    std::span<const float> argb_words) noexcept {
  return renderer_->impl_->updateOwnedExternalImage(
      image, argb_words);
}

void VulkanInterop::unregisterExternalImage(ExternalImageId image) noexcept {
  renderer_->impl_->unregisterExternalImage(image);
}

void VulkanInterop::setFrameRecorder(VulkanFrameRecorder recorder, void* context) noexcept {
  renderer_->impl_->frameRecorder = recorder;
  renderer_->impl_->frameRecorderContext = recorder != nullptr ? context : nullptr;
}

} // namespace slugia
