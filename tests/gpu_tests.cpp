#include "slugia/slugia.hpp"
#include "slugia/vulkan_interop.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace slugia;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void samePixels(const FramePixels &a, const FramePixels &b,
                const char *message) {
  require(a.width == b.width && a.height == b.height && !a.rgba.empty() &&
              a.rgba.size() == b.rgba.size(),
          "readback dimensions");
  for (std::size_t i = 0; i < a.rgba.size(); ++i)
    if (std::abs(int(a.rgba[i]) - int(b.rgba[i])) > 2)
      throw std::runtime_error(message);
}
void emit(DrawList &list, Paint paint, int kind) {
  const StrokeStyle stroke{paint, 5.0f};
  switch (kind) {
  case 0:
    list.roundedRect({20, 20, 80, 65}, 12, paint);
    break;
  case 1:
    list.line({20, 40}, {100, 70}, stroke);
    break;
  case 2:
    list.cubicBezier({20, 80}, {35, 0}, {75, 120}, {100, 40}, stroke);
    break;
  case 3:
    list.arc({64, 64}, 30, 0.2f, 4.8f, stroke);
    break;
  case 4:
    list.roundedRect({20, 20, 80, 65}, 0, paint);
    break;
  default:
    auto curves = makeEllipseQuadratics({64, 64}, {40, 25});
    list.dynamicVectorPath(curves, quadraticBounds(curves), paint,
                           FillRule::NonZero, true);
  }
}
DrawList churnScene() {
  DrawList scene;
  for (int i = 0; i < 160; ++i) {
    const float x = 8.0f + float(i % 8) * 12;
    const float y = 8.0f + float((i / 8) % 8) * 12;
    const float width = 6.0f + float(i % 23) * 0.2f;
    scene.roundedRect({x, y, width, 9.0f}, 1.0f + float(i % 13) * 0.1f,
                      Paint::solid(Color::fromRgb8(0x65caff)));
  }
  return scene;
}
} // namespace

int main(int argc, char **argv) try {
  VectorAtlas atlas;
  Path unit;
  unit.rect(0, 0, 1, 1);
  const auto unitShape = atlas.addPath(unit);
  atlas.build();
  Window window({128, 128, "slugia GPU regression tests", false, false});
  const bool performance = argc > 1 && (std::string_view(argv[1]) == "--research-profile" ||
                                       std::string_view(argv[1]) == "--research-profile-low-spec");
  auto config = performance ? RendererConfig::vectorPerformance(std::string_view(argv[1]) == "--research-profile-low-spec")
                            : RendererConfig::lowSpec();
  config.enableIndexedVectorQuads = argc > 1 && std::string_view(argv[1]) == "--indexed";
  config.enableCompactPersistentCurves = performance || (argc > 1 && std::string_view(argv[1]) == "--compact-curves");
  const bool packedBands = argc > 1 && std::string_view(argv[1]) == "--packed-bands";
  const bool cpuPhases = argc > 1 && std::string_view(argv[1]) == "--cpu-phases";
  config.enableCpuPhaseTiming = cpuPhases;
  if (cpuPhases) config.gpuTimingInterval = 1;
  if (packedBands) {
    config.enableCompactPersistentCurves = true;
    config.enablePackedAtlasBands = true;
    config.enableAtlasStoragePaths = true;
  }
  config.enableReadback = true;
  config.validation = true;
  config.clearColor = Color::fromRgb8(0x000000);
  config.dynamicPathCacheBytes = 4096;
  config.dynamicGeometryCacheBytes = 2048;
  const auto scene = churnScene();
  for (const auto cells : {0U, 31U, 33U, 513U, 2049U, 4096U}) {
    auto invalid = config;
    invalid.enableAtlasBandRefinement = true;
    invalid.atlasBandRefinementCells = cells;
    bool rejected = false;
    try {
      VulkanRenderer renderer(window, atlas, invalid);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected,
            "invalid atlas band counts must be rejected before GPU allocation");
  }
  FramePixels cachedPixels;
  {
    VulkanRenderer renderer(window, atlas, config);
    require(renderer.stats().validationEnabled,
            "GPU tests require the Khronos validation layer");
    std::cout << "GPU: " << renderer.deviceName() << '\n';
    if (cpuPhases) {
      if (!renderer.stats().gpuTimingAvailable) {
        std::cout << "CPU/GPU submission serial gate unsupported: timestamp queries unavailable\n";
        return 77;
      }
      renderer.draw(DrawList{});
      renderer.draw(DrawList{});
      const auto measured = renderer.stats();
      require(measured.cpuPhases.enabled && measured.lastSubmittedSerial == 2 &&
              measured.gpuSampleSubmissionSerial == 1,
              "CPU/GPU submission labels must refer to their actual frames");
      require(measured.cpuPhases.fenceWaitMilliseconds >= 0 &&
              measured.cpuPhases.acquireMilliseconds >= 0 &&
              measured.cpuPhases.queuePresentMilliseconds >= 0,
              "CPU API intervals must be nonnegative");
    }
    for (int kind = 0; kind < 6; ++kind) {
      DrawList global, local;
      const auto paint = Paint::solid(Color::fromRgb8(0xffffff));
      global.setOpacity(0.25f);
      emit(global, paint, kind);
      auto faded = paint;
      faded.opacity = 0.25f;
      emit(local, faded, kind);
      samePixels(
          renderer.drawAndReadback(global), renderer.drawAndReadback(local),
          "global opacity must match paint opacity for every vector command");
    }
    {
      auto &interop = renderer.vulkanInterop();
      const auto image = interop.createOwnedRgba32fImage(1, 1);
      const std::array<float, 4> white{1, 1, 1, 1};
      require(interop.updateOwnedRgba32fImage(image, white),
              "external image update");
      DrawList imageList, reference;
      imageList.setOpacity(0.25f);
      imageList.externalImage(image, {20, 20, 80, 65}, 1, 1);
      auto paint = Paint::solid(Color::fromRgb8(0xffffff));
      paint.opacity = 0.25f;
      reference.roundedRect({20, 20, 80, 65}, 0, paint);
      samePixels(renderer.drawAndReadback(imageList),
                 renderer.drawAndReadback(reference),
                 "external image opacity must be applied exactly once");
      renderer.waitIdle();
      interop.unregisterExternalImage(image);
    }
    cachedPixels = renderer.drawAndReadback(scene);
    const auto retained = renderer.createRetainedDrawList(scene);
    DrawList retainedScene;
    retainedScene.retainedDrawList(retained);
    samePixels(cachedPixels, renderer.drawAndReadback(retainedScene),
               "retaining dynamic geometry must preserve its pixels");
    // Dynamic curve buffers may be uploaded again even when instance bytes
    // are unchanged. Verify their image here, and zero-upload static data
    // below.
    renderer.updateRetainedDrawList(retained, scene);
    samePixels(cachedPixels, renderer.drawAndReadback(retainedScene),
               "an unchanged update must preserve the owned CPU shadow");
    {
      auto changed = churnScene();
      changed.roundedRect({40, 40, 20, 20}, 2,
                          Paint::solid(Color::fromRgb8(0xf04020)));
      require(renderer.updateRetainedDrawList(retained, changed) > 0,
              "a changed retained scene must upload its changed geometry");
      samePixels(
          renderer.drawAndReadback(changed),
          renderer.drawAndReadback(retainedScene),
          "retained updates must preserve pixels after transferring ownership");
      renderer.updateRetainedDrawList(retained, scene);
    }
    {
      DrawList source;
      source.shape(unitShape, {20, 20, 40, 50},
                   Paint::solid(Color::fromRgb8(0xffffff)));
      const auto id = renderer.createRetainedDrawList(source);
      require(renderer.updateRetainedDrawList(id, source) == 0,
              "an unchanged static scene must not upload instance data");
      DrawList display;
      display.retainedDrawList(id);
      samePixels(
          renderer.drawAndReadback(source), renderer.drawAndReadback(display),
          "an unchanged static update must preserve its owned CPU shadow");
      renderer.destroyRetainedDrawList(id);
    }
    for (int frame = 0; frame < 30; ++frame) {
      DrawList changing;
      changing.roundedRect({10, 10, 50.0f + float(frame) * 0.1f, 50}, 8,
                           Paint::solid(Color::fromRgb8(0xffffff)));
      renderer.draw(changing);
      const auto stats = renderer.stats();
      require(stats.dynamicPathCacheBytes <= config.dynamicPathCacheBytes &&
                  stats.dynamicGeometryCacheBytes <=
                      config.dynamicGeometryCacheBytes,
              "renderer cache payload must stay within the configured byte "
              "budgets");
      require(stats.persistentGeometryBufferBytes == 2U * config.persistentGeometryBufferBytes &&
                  stats.persistentGeometryAllocatedBytes >=
                      stats.persistentGeometryBufferBytes,
              "persistent geometry allocation must match the selected buffer budget");
    }
    samePixels(cachedPixels, renderer.drawAndReadback(retainedScene),
               "cache eviction must not invalidate retained geometry");
    renderer.destroyRetainedDrawList(retained);
    {
      DrawList source, reference;
      source.setClip({0, 0, 20, 20});
      source.roundedRect({150, 20, 50, 50}, 6,
                         Paint::solid(Color::fromRgb8(0xffffff)));
      reference.roundedRect({50, 20, 50, 50}, 6,
                            Paint::solid(Color::fromRgb8(0xffffff)));
      const auto resource = renderer.createRetainedDrawList(source);
      DrawList moved;
      moved.retainedDrawList(resource, {-100, 0});
      samePixels(renderer.drawAndReadback(reference),
                 renderer.drawAndReadback(moved),
                 "retained geometry must survive culling until its final "
                 "transform and clip");
      renderer.destroyRetainedDrawList(resource);
    }

    DrawList clipped;
    clipped.setClip({0, 0, 128, 128});
    auto offscreen = makeEllipseQuadratics({1000, 1000}, {20, 20});
    clipped.dynamicVectorPath(offscreen, quadraticBounds(offscreen),
                              Paint::solid(Color::fromRgb8(0xffffff)),
                              FillRule::NonZero, true);
    const auto misses = renderer.stats().dynamicPathCacheMisses;
    renderer.draw(clipped);
    require(renderer.stats().quads == 0 &&
                renderer.stats().dynamicPathCacheMisses == misses,
            "off-clip paths must skip compilation, caching and upload");

#if SLUGIA_ENABLE_3D
    auto box = makeBoxMesh3D({30, 25, 20});
    Mesh3DView view;
    view.origin = {64, 64};
    view.transform.rotation = {0.3f, 0.5f, 0.2f};
    Mesh3DStyle style;
    style.drawEdges = false;
    DrawList ordered, reversed;
    drawMesh3D(ordered, box, view, style);
    std::reverse(box.triangles.begin(), box.triangles.end());
    prepareMesh3D(box);
    box.convex = false;
    drawMesh3D(reversed, box, view, style);
    samePixels(renderer.drawAndReadback(ordered),
               renderer.drawAndReadback(reversed),
               "single-paint mesh coverage must be independent of face order");
    {
      DrawList source, projected;
      source.setClip({0, 0, 128, 128});
      source.roundedRect({20, 20, 80, 65}, 12,
                         Paint::solid(Color::fromRgb8(0x65caff)));
      projected.projected(source, {});
      samePixels(
          renderer.drawAndReadback(source), renderer.drawAndReadback(projected),
          "identity projection must preserve vector pixels and band offsets");
      DrawList canvas;
      canvas.setClip({-40, -35, 80, 70});
      canvas.roundedRect({-40, -35, 40, 70}, 0,
                         Paint::solid(Color::fromRgb8(0xff0000)));
      canvas.roundedRect({0, -35, 40, 70}, 0,
                         Paint::solid(Color::fromRgb8(0x0000ff)));
      Mesh3DView panel;
      panel.origin = {64, 64};
      panel.transform.rotation = {.2f, .6f, .1f};
      DrawList tilted;
      drawPlane3D(tilted, canvas, panel);
      const auto image = renderer.drawAndReadback(tilted);
      for (float x : {-20.0f, 20.0f}) {
        const auto p = projectPoint3D({x, 0, 0}, panel).position;
        const auto offset = (static_cast<std::size_t>(p.y) * image.width +
                             static_cast<std::size_t>(p.x)) *
                            4;
        require(x < 0 ? image.rgba[offset] > 240 && image.rgba[offset + 2] < 15
                      : image.rgba[offset + 2] > 240 && image.rgba[offset] < 15,
                "tilted vector planes must preserve perspective-correct colors "
                "and clipping");
      }
    }
    {
      Mesh3D a, b;
      a.vertices = {{-40, -35, -70}, {40, -35, 70}, {0, 35, 0}};
      a.triangles = {{0, 1, 2}};
      b.vertices = {{-40, -35, 70}, {40, -35, -70}, {0, 35, 0}};
      b.triangles = {{0, 1, 2}};
      prepareMesh3D(a);
      prepareMesh3D(b);
      std::array<SceneObject3D, 2> objects{};
      objects[0].mesh = &a;
      objects[1].mesh = &b;
      for (auto &object : objects) {
        object.style.drawEdges = false;
        object.style.cullBackfaces = false;
      }
      objects[0].style.fill = Paint::solid(Color::fromRgb8(0xff2211));
      objects[1].style.fill = Paint::solid(Color::fromRgb8(0x1144ff));
      Mesh3DView view;
      view.origin = {64, 64};
      DrawList scene, reordered;
      drawScene3D(scene, objects, view);
      const auto pixels = renderer.drawAndReadback(scene);
      require(pixels.rgba[(52 * pixels.width + 44) * 4] > 240 &&
                  pixels.rgba[(52 * pixels.width + 84) * 4 + 2] > 240,
              "intersecting 3D surfaces must resolve the nearer material per "
              "pixel");
      std::swap(objects[0], objects[1]);
      drawScene3D(reordered, objects, view);
      samePixels(pixels, renderer.drawAndReadback(reordered),
                 "3D scene depth must not depend on object submission order");
    }
    {
      Mesh3D panel;
      panel.vertices = {{-20, -20, 0}, {20, -20, 0}, {20, 20, 0}, {-20, 20, 0}};
      panel.triangles = {{0, 1, 2}, {0, 2, 3}};
      prepareMesh3D(panel);
      Curve3D line;
      line.basis = CurveBasis3D::Polyline;
      line.points = {{{-50, 0, 50}, {}, {}, 1}, {{50, 0, 50}, {}, {}, 1}};
      prepareCurve3D(line);
      std::array<SceneObject3D, 2> objects{};
      objects[0].mesh = &panel;
      objects[0].style.cullBackfaces = false;
      objects[0].style.drawEdges = false;
      objects[0].style.fill = Paint::solid(Color::fromRgb8(0xff0000));
      objects[1].curves = std::span<const Curve3D>(&line, 1);
      objects[1].style.edge.paint = Paint::solid(Color::fromRgb8(0xffffff));
      objects[1].style.edge.width = 3;
      Mesh3DView view;
      view.origin = {64, 64};
      DrawList scene;
      drawScene3D(scene, objects, view);
      const auto pixels = renderer.drawAndReadback(scene);
      require(pixels.rgba[(64 * pixels.width + 64) * 4] > 240 &&
                  pixels.rgba[(64 * pixels.width + 64) * 4 + 1] < 15 &&
                  pixels.rgba[(64 * pixels.width + 104) * 4 + 1] > 200,
              "native 3D curves must be occluded by nearer mesh surfaces");
    }
    {
      Mesh3D panel;
      panel.vertices = {{20, 20, 0}, {108, 20, 0}, {108, 108, 0}, {20, 108, 0}};
      panel.triangles = {{0, 1, 2}, {0, 2, 3}};
      panel.trianglePaints = {Paint::solid({1, 1, 1, 1}),
                              Paint::solid({1, 1, .99999f, 1})};
      prepareMesh3D(panel);
      SceneObject3D object;
      object.mesh = &panel;
      object.style.drawEdges = false;
      object.style.cullBackfaces = false;
      Mesh3DView view;
      view.camera.perspective = 0;
      DrawList scene;
      drawScene3D(scene, std::span<const SceneObject3D>(&object, 1), view);
      const auto pixels = renderer.drawAndReadback(scene);
      const auto offset = (64 * pixels.width + 64) * 4;
      require(pixels.rgba[offset] >= 250 && pixels.rgba[offset + 1] >= 250 &&
                  pixels.rgba[offset + 2] >= 250,
              "adjacent 3D faces must not produce a dark antialiasing seam");
      const auto retained = renderer.createRetainedDrawList(scene);
      DrawList retainedScene;
      retainedScene.retainedDrawList(retained);
      samePixels(pixels, renderer.drawAndReadback(retainedScene),
                 "retained 3D scenes must preserve coverage blend boundaries");
      renderer.destroyRetainedDrawList(retained);
    }
#endif
    renderer.waitIdle();
    if (cpuPhases)
      require(renderer.stats().gpuSampleSubmissionSerial == renderer.stats().lastSubmittedSerial,
              "waitIdle must resolve the final GPU timestamp without another draw");
    require(renderer.stats().validationErrors == 0,
            "Vulkan validation reported an error");
  }
  config.dynamicPathCacheBytes = 0;
  config.dynamicGeometryCacheBytes = 0;
  config.persistentGeometryBufferBytes = 0;
  if (performance) {
    config.enableAtlasStoragePaths = false;
    config.enableAtlasBandRefinement = false;
    config.enableCompactPersistentCurves = false;
    config.enablePackedAtlasBands = false;
  }
  {
    VulkanRenderer renderer(window, atlas, config);
    {
      constexpr std::size_t count = 30000;
      const std::array<std::size_t, 4> visibleIndices{0, 15000, 26214,
                                                      count - 1};
      const auto largeScene = [&](int version, DrawList &reference) {
        DrawList result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
          const auto found =
              std::find(visibleIndices.begin(), visibleIndices.end(), i);
          const bool visible = found != visibleIndices.end();
          const auto slot = std::size_t(found - visibleIndices.begin());
          const Rect rect = visible ? Rect{8.0f + float(slot) * 28, 32, 20, 24}
                                    : Rect{1000.0f + float(i % 100) * 2,
                                           1000.0f + float(i / 100) * 2, 1, 1};
          const auto color = version == 2              ? 0x40a0ff
                             : version == 1 && visible ? 0xff7040
                                                       : 0xffffff;
          const auto paint = Paint::solid(Color::fromRgb8(color));
          result.shape(unitShape, rect, paint);
          if (visible)
            reference.shape(unitShape, rect, paint);
        }
        return result;
      };
      DrawList reference;
      auto source = largeScene(0, reference);
      const auto id = renderer.createRetainedDrawList(source);
      DrawList display;
      display.retainedDrawList(id);
      const auto initial = renderer.drawAndReadback(display);
      require(renderer.stats().retainedQuads == count,
              "chunked uploads must preserve every retained instance");
      samePixels(
          renderer.drawAndReadback(reference), initial,
          "bulk upload must preserve instances across the chunk boundary");
      for (int version = 1; version <= 2; ++version) {
        reference.clear();
        source = largeScene(version, reference);
        renderer.draw(display);
        const auto bytes = renderer.updateRetainedDrawList(id, source);
        const auto expected = (version == 1 ? visibleIndices.size() : count) *
                              renderer.stats().retainedInstanceStrideBytes;
        require(
            bytes == expected,
            "sparse and dense updates must report their actual transfer size");
        samePixels(
            renderer.drawAndReadback(reference),
            renderer.drawAndReadback(display),
            "bounded sparse and dense uploads must preserve the final image");
      }
      require(
          count * renderer.stats().retainedInstanceStrideBytes > (4U << 20U) &&
              renderer.stats().retainedUploadStagingPeakBytes <= (4U << 20U),
          "large retained transfers must stay within the 4 MiB staging cap");
      renderer.destroyRetainedDrawList(id);
    }
    {
      const auto curves = makeEllipseQuadratics({40, 40}, {20, 16});
      DrawList source;
      source.dynamicVectorPath(curves, quadraticBounds(curves),
                               Paint::solid(Color::fromRgb8(0xffffff)),
                               FillRule::NonZero, false);
      const auto id = renderer.createRetainedDrawList(source);
      require(renderer.updateRetainedDrawList(id, source) == 0,
              "identical dynamic buffers must not be uploaded again");
      DrawList display;
      display.retainedDrawList(id);
      samePixels(renderer.drawAndReadback(source),
                 renderer.drawAndReadback(display),
                 "skipping identical dynamic uploads must preserve pixels");
      renderer.draw(display);
      renderer.updateRetainedDrawList(id, DrawList{});
      samePixels(renderer.drawAndReadback(DrawList{}),
                 renderer.drawAndReadback(display),
                 "clearing in-flight retained data must leave an empty image");
      renderer.destroyRetainedDrawList(id);
    }
    samePixels(
        cachedPixels, renderer.drawAndReadback(scene),
        "cache eviction and promotion must not change the rendered image");
    const auto stats = renderer.stats();
    require(
        stats.dynamicPathCacheBytes == 0 &&
            stats.dynamicGeometryCacheBytes == 0 &&
            stats.persistentGeometryBufferBytes == 36,
        "disabled caches must retain only valid descriptor fallback buffers");
    renderer.waitIdle();
    require(renderer.stats().validationErrors == 0,
            "Vulkan validation reported an error");
  }
  std::cout << "GPU opacity, cache churn, retained geometry, clipping and mesh "
               "coverage passed\n";
  return 0;
} catch (const std::exception &error) {
  std::cerr << "GPU regression failure: " << error.what() << '\n';
  return 1;
}
