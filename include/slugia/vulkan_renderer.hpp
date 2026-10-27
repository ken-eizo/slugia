#pragma once

#include "slugia/draw_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace slugia {

class VectorAtlas;
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
class Window;
#endif
class PlatformSurface;
class VulkanInterop;

struct RendererConfig {
  // Opt-in diagnostics only; normal rendering does not allocate/copy/wait for pixels.
  bool enableReadback = false;
  // Request an alpha-composited native surface. The platform surface must also
  // support compositor alpha; otherwise construction fails rather than silently
  // presenting an opaque overlay.
  bool transparentSurface = false;
  Color clearColor = Color::fromRgb8(0x0b1020);
  bool validation = false;
  // false prefers refresh-synchronized MAILBOX for smooth low-latency interaction.
  bool vsync = false;
  // Select IMMEDIATE before MAILBOX when tearing is acceptable and absolute latency is primary.
  bool allowTearing = false;
  // Retain authored radii while forcing circular (non-superellipse) corners when disabled.
  bool enableContinuousCorners = true;
  // Experimental path tiling. Disabled until image-equivalence gates pass.
  bool enablePathTileCulling = false;
  // Number of quad instances preallocated per frame; buffers grow geometrically when needed.
  std::size_t initialVertexCapacity = 1U << 12U;
  // Initial device-local retained arena size. It grows geometrically and suballocates resources.
  std::size_t retainedArenaInitialBytes = 256U << 10U;
  // Bounded resolved glyph-run cache. Smaller values trade CPU work for lower resident memory.
  std::size_t glyphRunCacheCapacity = 512;
  // GPU timestamps add query commands. Zero disables them; otherwise sample every Nth submission.
  std::uint32_t gpuTimingInterval = 0;

  // Retained CPU payload budgets (keys + compiled/vector storage). Zero disables a cache.
  // Container/allocator overhead and the current frame's working memory are additional.
  std::size_t dynamicPathCacheBytes = 4U << 20U;
  std::size_t dynamicGeometryCacheBytes = 2U << 20U;
  // Size of EACH persistent curve/band GPU buffer, capped by the device storage limit.
  // Zero disables promotion; small valid descriptor fallback buffers are still required.
  std::size_t persistentGeometryBufferBytes = 8U << 20U;
  // Experimental packed-atlas to storage-buffer conversion. Keeps curve/band
  // order and uses the existing persistent-buffer budget; off by default.
  bool enableAtlasStoragePaths = false;
  // Experimental conservative sub-bands for the storage path; off by default.
  bool enableAtlasBandRefinement = false;
  // Experimental lossless float32 persistent-curve layout: 24 instead of 32
  // bytes. Transient curve buffers retain their existing layout. Off by default.
  bool enableCompactPersistentCurves = false;
  // Experimental lossless packing of eligible static atlas curve-ID lists.
  // Headers remain uint32; paths requiring IDs above 65535 stay unpacked.
  bool enablePackedAtlasBands = false;
  // Experimental reuse of the four unique vector-quad vertices; off by default.
  bool enableIndexedVectorQuads = false;
  // Power of two from 32 through 2048; only used by the opt-in refinement.
  // Requests above 512 halve down through 512, then use the original bands
  // if they do not fit. Original admitted paths and GPU budgets are retained.
  std::uint32_t atlasBandRefinementCells = 128;
  // Optional host wall intervals around renderer API work; off by default.
  // Intervals include OS descheduling and do not measure active CPU cycles.
  bool enableCpuPhaseTiming = false;

  // Conservative profile for low-memory / integrated-GPU hosts and battery-sensitive tools.
  [[nodiscard]] static RendererConfig lowSpec() noexcept {
    RendererConfig result;
    result.vsync = true;
    result.allowTearing = false;
    result.enableContinuousCorners = false;
    result.initialVertexCapacity = 1U << 10U;
    result.retainedArenaInitialBytes = 64U << 10U;
    result.glyphRunCacheCapacity = 128;
    result.gpuTimingInterval = 0;
    result.dynamicPathCacheBytes = 256U << 10U;
    result.dynamicGeometryCacheBytes = 128U << 10U;
    result.persistentGeometryBufferBytes = 64U << 10U;
    return result;
  }

  // Explicit integration of the pixel-equivalent research results measured
  // in October 2026. Raw defaults remain the compatibility configuration.
  // This preset does not claim best performance on every GPU or workload.
  [[nodiscard]] static RendererConfig vectorPerformance(bool lowMemory = false) noexcept {
    auto result = lowMemory ? lowSpec() : RendererConfig{};
    result.enableAtlasStoragePaths = true;
    result.enableAtlasBandRefinement = true;
    result.enableCompactPersistentCurves = true;
    result.enablePackedAtlasBands = true;
    result.atlasBandRefinementCells = 1024;
    return result;
  }
};

struct CpuFrameTimings {
  bool enabled = false;
  double fenceWaitMilliseconds = 0;
  double queryReadMilliseconds = 0;
  double acquireMilliseconds = 0;
  double recordMilliseconds = 0;
  double queueSubmitMilliseconds = 0;
  double queuePresentMilliseconds = 0;
  double postPresentMilliseconds = 0;
};

struct RendererStats {
  std::uint32_t quads = 0;
  std::uint32_t retainedQuads = 0;
  std::uint32_t primitiveQuads = 0;
  std::uint32_t drawCalls = 0;
  std::size_t uploadedBytes = 0;
  float cpuBuildMilliseconds = 0.0f;
  float cpuUploadMilliseconds = 0.0f;
  float gpuMilliseconds = 0.0f;
  bool gpuTimingAvailable = false;
  float cpuSubmitMilliseconds = 0.0f;
  // Retained cache payload and allocated persistent geometry buffers, not total process memory.
  std::size_t dynamicPathCacheBytes = 0;
  std::size_t dynamicGeometryCacheBytes = 0;
  std::size_t persistentGeometryAllocatedBytes = 0;
  std::size_t persistentGeometryBufferBytes = 0;
  std::uint64_t dynamicPathCacheHits = 0;
  std::uint64_t dynamicPathCacheMisses = 0;
  bool validationEnabled = false;
  std::uint32_t validationErrors = 0;
  std::uint32_t atlasStorageShapeCount = 0;
  std::uint32_t atlasRefinedShapeCount = 0;
  std::uint32_t atlasPackedBandShapeCount = 0;
  // Counts at 32,64,128,256,512,1024,2048 cells, including budget fallbacks.
  std::array<std::uint32_t, 7> atlasRefinedShapesByCells{};
  std::size_t atlasStorageCurveBytes = 0;
  std::size_t atlasStorageBandBytes = 0;
  std::size_t retainedInstanceStrideBytes = 0;
  std::size_t retainedUploadStagingLimitBytes = 0;
  std::size_t retainedUploadStagingPeakBytes = 0;
  CpuFrameTimings cpuPhases{};
  // Source submission of the last resolved GPU timestamp; zero means none.
  // It is generally different from lastSubmittedSerial (the current CPU frame).
  std::uint64_t gpuSampleSubmissionSerial = 0;
  std::uint64_t lastSubmittedSerial = 0;
  std::uint32_t maxFramesInFlight = 0;
  double gpuTimestampPeriodNanoseconds = 0;
  std::uint32_t gpuTimestampValidBits = 0;
  std::uint32_t apiVersion = 0, driverVersion = 0, vendorId = 0, deviceId = 0;
};

struct FramePixels {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> rgba;
};

class VulkanRenderer {
public:
  VulkanRenderer(PlatformSurface& surface, const VectorAtlas& atlas,
                 const RendererConfig& config = {});
#if defined(SLUGIA_ENABLE_GLFW) && SLUGIA_ENABLE_GLFW
  VulkanRenderer(Window& window, const VectorAtlas& atlas, const RendererConfig& config = {});
#endif
  ~VulkanRenderer();
  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  // Performs all potentially blocking fence/image acquisition work. Call this before polling
  // input, then build the DrawList and call draw() for the freshest possible interactive frame.
  void prepareFrame();
  // Resolves and uploads an immutable document once. The returned resource remains valid for
  // this renderer's lifetime and can be transformed cheaply with DrawList::retainedText().
  RetainedTextId createRetainedText(std::string_view utf8, Rect layoutBounds, TextStyle style);
  // Re-shapes text and sparsely updates the existing retained arena range when possible.
  std::size_t updateRetainedText(RetainedTextId text, std::string_view utf8,
                                 Rect layoutBounds, TextStyle style);
  // Releases retained text storage. IDs remain stable and are not recycled.
  void destroyRetainedText(RetainedTextId text);
  // Retains an arbitrary static DrawList in device-local memory. Retained commands may be
  // transformed, clipped and faded every frame without rebuilding their instances.
  RetainedDrawListId createRetainedDrawList(const DrawList& list);
  // Rebuilds CPU instances, but uploads only changed contiguous ranges when capacity permits.
  // Returns the number of bytes transferred to the GPU (zero when the retained data is identical).
  std::size_t updateRetainedDrawList(RetainedDrawListId drawList, const DrawList& list);
  // Releases the backing GPU allocation. IDs are never recycled, so stale commands stay inert.
  void destroyRetainedDrawList(RetainedDrawListId drawList);
  void draw(const DrawList& list);
  [[nodiscard]] FramePixels drawAndReadback(const DrawList& list);
  // Propagates device-wait failures. CPU profiling also resolves completed
  // timestamp slots here, without acquiring another image or submitting work.
  void waitIdle();
  [[nodiscard]] RendererStats stats() const;
  [[nodiscard]] const char* deviceName() const;
  [[nodiscard]] const char* presentModeName() const;
  // Explicit advanced bridge; include slugia/vulkan_interop.hpp to use it.
  [[nodiscard]] VulkanInterop& vulkanInterop() noexcept;

private:
  friend class VulkanInterop;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<VulkanInterop> interop_;
};

} // namespace slugia
