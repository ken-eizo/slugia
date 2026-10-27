#include "geist_font.generated.hpp"
#include "slugia/slugia.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

using namespace slugia;
namespace {
TextStyle label(float size = 14, Color color = Color::fromRgb8(0xdbe7ff)) {
  TextStyle text;
  text.size = size;
  text.fontName = "Geist";
  text.paint = Paint::solid(color);
  return text;
}
void capture(const FramePixels &pixels, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << pixels.width << ' ' << pixels.height << "\n255\n";
  for (std::size_t i = 0; i < pixels.rgba.size(); i += 4)
    out.write(reinterpret_cast<const char *>(pixels.rgba.data() + i), 3);
  if (!out)
    throw std::runtime_error("Could not write the 3D capture");
}
AffineTransform3D normalizeAsset(const SceneAsset3D &asset) {
  Vec3 minimum{1e30f, 1e30f, 1e30f}, maximum{-1e30f, -1e30f, -1e30f};
  bool found = false;
  const auto include = [&](Vec3 p, const AffineTransform3D &transform) {
    p = transformPoint3D(transform, p);
    found = true;
    minimum = {std::min(minimum.x, p.x), std::min(minimum.y, p.y),
               std::min(minimum.z, p.z)};
    maximum = {std::max(maximum.x, p.x), std::max(maximum.y, p.y),
               std::max(maximum.z, p.z)};
  };
  for (const auto &object : asset.objects) {
    for (Vec3 p : object.mesh.vertices)
      include(p, object.transform);
    for (const auto &curve : object.curves)
      for (const auto &p : curve.points)
        include(p.position, object.transform);
  }
  if (!found)
    return {};
  const float scale =
      400.0f / std::max({maximum.x - minimum.x, maximum.y - minimum.y,
                         maximum.z - minimum.z, 1e-5f});
  Transform3D transform;
  transform.scale = {scale, scale, scale};
  transform.translation = {-(minimum.x + maximum.x) * .5f * scale,
                           -(minimum.y + maximum.y) * .5f * scale,
                           -(minimum.z + maximum.z) * .5f * scale};
  return affineTransform3D(transform);
}
} // namespace
int main(int argc, char **argv) try {
  bool smoke = false, baseline = false, lowMemory = false;
  std::string outputPath, assetPath;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--smoke")
      smoke = true;
    else if (arg == "--baseline") baseline = true;
    else if (arg == "--low-spec") lowMemory = true;
    else if (arg == "--capture" && i + 1 < argc)
      outputPath = argv[++i];
    else if (arg == "--asset" && i + 1 < argc)
      assetPath = argv[++i];
    else
      throw std::invalid_argument(
          "Usage: slugia3D [--smoke] [--baseline] [--low-spec] [--capture output.ppm] [--asset "
          "model.obj|model.slug3d]");
  }
  VectorAtlas atlas;
  if (!atlas.loadFontMemory(example::assets::geistFont,
                            {"Geist", 400, false, "Regular"}))
    throw std::runtime_error("Cannot load the embedded font");
  Path unit;
  unit.rect(0, 0, 1, 1);
  atlas.addPath(unit);
  atlas.build();
  Window window({1440, 900, "slugia3D - Vector Scene", true, !smoke});
  auto config = baseline ? (lowMemory ? RendererConfig::lowSpec() : RendererConfig{})
                         : RendererConfig::vectorPerformance(lowMemory);
  config.clearColor = Color::fromRgb8(0x080e1b);
  config.validation = smoke;
  config.enableReadback = !outputPath.empty();
  config.gpuTimingInterval = smoke ? 1 : 60;
  VulkanRenderer renderer(window, atlas, config);
  const std::array<Mesh3D, 4> meshes{
      makeBoxMesh3D({62, 62, 62}), makeUvSphereMesh3D(70, 32, 16),
      makeCylinderMesh3D(57, 82, 40), makeConeMesh3D(64, 86, 40)};
  Curve3D bezier;
  bezier.points = {{{-290, 20, -60}, {-290, 20, -60}, {-130, -220, 180}, 1},
                   {{270, 20, 80}, {80, 210, -140}, {270, 20, 80}, 1}};
  prepareCurve3D(bezier);
  std::optional<SceneAsset3D> asset;
  AffineTransform3D normalization;
  std::uint64_t assetVersion = 0;
  std::string status = "Drop an OBJ or Slug3D model into this window";
  const auto load = [&](const std::string &path) {
    auto model = loadScene3D(path);
    normalization = normalizeAsset(model);
    asset = std::move(model);
    ++assetVersion;
    status = "Loaded: " + path;
  };
  if (!assetPath.empty())
    load(assetPath);
  window.setDropCallback([&](const auto &paths) {
    if (paths.empty())
      return;
    try {
      load(paths.front());
    } catch (const std::exception &e) {
      status = e.what();
    }
  });
  float pitch = -18, yaw = 24, roll = 0, perspective = 1, zoom = 1, depth = 0,
        panelX = -12, panelY = -24;
  bool shaded = true, edges = true, glass = false, orbit = false;
  std::uint64_t selected = 0, activeSlider = 0;
  int clicks = 0, frames = 0;
  DrawList draw;
  Scene3DStats sceneStats{};
  double buildMs = 0;
  DrawList sceneCommands;
  RetainedDrawListId retainedScene = 0;
  std::optional<std::array<float, 10>> previousView;
  std::array<std::uint64_t, 5> previousPolicy{};
  bool retainedDirty = true;
  while (!window.shouldClose()) {
    renderer.prepareFrame();
    window.pollEvents();
    draw.clear();
    const auto size = window.framebufferSize();
    draw.setClip({0, 0, size.x, size.y});
    const auto &input = window.input();
    if (input.key(Key::Escape).pressed)
      window.requestClose();
    const float controlsX = size.x - 300;
    const Rect stage{24, 100, std::max(320.0f, size.x - 352),
                     std::max(300.0f, size.y - 175)};
    if (smoke && !asset && (frames == 1 || frames == 2)) {
      Mesh3DView testPlane;
      testPlane.origin = {stage.x + stage.width * .5f, stage.y + 145};
      testPlane.camera.position = {0, 0, -700};
      testPlane.camera.focalLength = 700;
      constexpr float toRadians = 3.14159265358979323846f / 180;
      testPlane.transform.rotation = {panelX * toRadians, panelY * toRadians,
                                      0};
      InputWriter writer(window.input());
      writer.cursor(projectPoint3D({0, 35, 0}, testPlane).position);
      writer.mouseButton(MouseButton::Left, frames == 1 ? InputAction::Press
                                                        : InputAction::Release);
    }
    const auto mouse = input.cursorPosition();
    draw.textStatic("slugia3D", {28, 22, 500, 42}, label(30));
    draw.textStatic("Vector geometry / scene depth / native curves / 3D UI",
                    {30, 67, 900, 22}, label(14, Color::fromRgb8(0x8b9fbd)));
    draw.roundedRect(stage, 18, Paint::solid(Color::fromRgb8(0x0d1628)));
    const auto button = [&](std::string_view text, Rect rect, bool active) {
      const bool hover = rect.contains(mouse);
      draw.roundedRect(rect, 7,
                       Paint::solid(Color::fromRgb8(active  ? 0x335de1
                                                    : hover ? 0x263b5e
                                                            : 0x17263d)));
      draw.textStatic(text, {rect.x + 12, rect.y + 8, rect.width - 24, 22},
                      label());
      return hover && input.mouse(MouseButton::Left).pressed;
    };
    const auto slider = [&](std::uint64_t id, std::string_view name, float y,
                            float &value, float minimum, float maximum) {
      draw.textStatic(name, {controlsX, y, 190, 20}, label(13));
      std::array<char, 32> formatted{};
      std::snprintf(formatted.data(), formatted.size(), "%.2f", double(value));
      draw.text(formatted.data(), {controlsX + 194, y, 80, 20}, label(12));
      const Rect hit{controlsX, y + 22, 268, 22};
      if (hit.contains(mouse) && input.mouse(MouseButton::Left).pressed)
        activeSlider = id;
      if (activeSlider == id && input.mouse(MouseButton::Left).down)
        value =
            minimum + std::clamp((mouse.x - hit.x) / hit.width, 0.0f, 1.0f) *
                          (maximum - minimum);
      draw.roundedRect({hit.x, hit.y + 8, hit.width, 5}, 2.5f,
                       Paint::solid(Color::fromRgb8(0x2b3d59)));
      const float position =
          hit.x + (value - minimum) / (maximum - minimum) * hit.width;
      draw.roundedRect({position - 5, hit.y + 3, 10, 15}, 5,
                       Paint::solid(Color::fromRgb8(0x8aafff)));
    };
    if (button(shaded ? "Lighting on" : "Lighting off",
               {controlsX, 105, 130, 34}, shaded))
      shaded = !shaded;
    if (button(edges ? "Edges on" : "Edges off",
               {controlsX + 138, 105, 130, 34}, edges))
      edges = !edges;
    if (button(glass ? "Glass on" : "Glass off", {controlsX, 148, 130, 34},
               glass))
      glass = !glass;
    if (button("Reset", {controlsX + 138, 148, 130, 34}, false)) {
      pitch = -18;
      yaw = 24;
      roll = 0;
      perspective = 1;
      zoom = 1;
      depth = 0;
      panelX = -12;
      panelY = -24;
    }
    slider(1, "Object X", 205, pitch, -180, 180);
    slider(2, "Object Y", 262, yaw, -180, 180);
    slider(3, "Object Z", 319, roll, -180, 180);
    slider(4, "Perspective", 376, perspective, 0, 2);
    slider(5, "Zoom", 433, zoom, .3f, 2.5f);
    slider(6, "Depth / near clipping", 490, depth, -760, 500);
    slider(7, "UI plane X", 570, panelX, -75, 75);
    slider(8, "UI plane Y", 627, panelY, -75, 75);
    if (input.mouse(MouseButton::Left).released) {
      activeSlider = 0;
      orbit = false;
    }
    if (stage.contains(mouse) && input.scroll().delta.y != 0)
      zoom =
          std::clamp(zoom * std::exp(input.scroll().delta.y * .08f), .3f, 2.5f);
    constexpr float radians = 3.14159265358979323846f / 180;
    Mesh3DView view;
    view.origin = {stage.x + stage.width * .5f, stage.y + stage.height * .57f};
    view.camera.position = {0, 0, -700};
    view.camera.focalLength = 700;
    view.camera.perspective = perspective;
    view.camera.nearPlane = .5f;
    view.transform.rotation = {pitch * radians, yaw * radians, roll * radians};
    view.transform.scale = {zoom, zoom, zoom};
    view.transform.translation = {0, 0, depth};
    std::vector<SceneObject3D> objects;
    if (asset) {
      for (std::size_t i = 0; i < asset->objects.size(); ++i) {
        const auto &source = asset->objects[i];
        SceneObject3D object;
        object.mesh = source.mesh.triangles.empty() ? nullptr : &source.mesh;
        object.parent = composeTransform3D(normalization, source.transform);
        object.style = source.style;
        object.id = i + 1;
        if (!source.useMeshFallback)
          object.curves = source.curves;
        objects.push_back(object);
      }
    } else {
      const std::array<Vec3, 4> positions{
          {{-190, 30, 20}, {-45, 40, 15}, {110, 20, -25}, {250, 40, 40}}};
      const std::array<std::uint32_t, 4> colors{0x638fff, 0x52d0b1, 0xea76ac,
                                                0xf3b064};
      for (std::size_t i = 0; i < meshes.size(); ++i) {
        SceneObject3D object;
        object.mesh = &meshes[i];
        object.transform.translation = positions[i];
        object.transform.rotation = {.12f, float(i) * .2f, 0};
        object.style.fill = Paint::solid(Color::fromRgb8(colors[i]),
                                         glass && i == 0 ? .45f : 1.0f);
        object.id = i + 1;
        objects.push_back(object);
      }
      SceneObject3D curve;
      curve.curves = std::span<const Curve3D>(&bezier, 1);
      curve.style.edge.paint = Paint::solid(Color::fromRgb8(0xffe092));
      curve.style.edge.width = 2.5f;
      objects.push_back(curve);
    }
    for (auto &object : objects) {
      object.style.shaded = shaded;
      object.style.drawEdges = edges;
      if (object.mesh) {
        object.style.edge.paint = Paint::solid(
            Color::fromRgb8(object.id == selected ? 0xffffff : 0xc4d5f8));
        object.style.edge.width = object.id == selected ? 2 : 1;
      }
    }
    Mesh3DView panel;
    panel.origin = {stage.x + stage.width * .5f, stage.y + 145};
    panel.camera = view.camera;
    panel.transform.rotation = {panelX * radians, panelY * radians, 0};
    const auto local = unprojectPlane3D(mouse, panel);
    const Rect panelButton{-85, 22, 170, 36};
    const bool panelHover = !asset && local && panelButton.contains(*local);
    if (panelHover && input.mouse(MouseButton::Left).pressed)
      ++clicks;
    if (input.mouse(MouseButton::Left).pressed && stage.contains(mouse) &&
        !panelHover) {
      const auto hit = pickScene3D(mouse, objects, view);
      selected = hit ? hit->objectId : 0;
      orbit = true;
    }
    if (orbit && input.mouse(MouseButton::Left).down) {
      yaw += input.cursorDelta().x * .3f;
      pitch -= input.cursorDelta().y * .3f;
    }
    view.transform.rotation = {pitch * radians, yaw * radians, roll * radians};
    for (auto &object : objects)
      if (object.mesh) {
        object.style.edge.paint = Paint::solid(
            Color::fromRgb8(object.id == selected ? 0xffffff : 0xc4d5f8));
        object.style.edge.width = object.id == selected ? 2 : 1;
      }
    draw.setClip(stage);
    const std::array<float, 10> key{
        pitch, yaw,     roll,    perspective, zoom,
        depth, stage.x, stage.y, stage.width, stage.height};
    const std::array<std::uint64_t, 5> policy{
        std::uint64_t(shaded), std::uint64_t(edges), std::uint64_t(glass),
        selected, assetVersion};
    const bool changed =
        !previousView || *previousView != key || previousPolicy != policy;
    if (changed) {
      const auto begin = std::chrono::steady_clock::now();
      sceneCommands.clear();
      sceneCommands.setClip(stage);
      try {
        sceneStats = drawScene3D(sceneCommands, objects, view);
      } catch (const std::exception &error) {
        if (smoke)
          throw;
        sceneCommands.clear();
        sceneStats = {};
        status = error.what();
      }
      buildMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin)
                    .count();
      previousView = key;
      previousPolicy = policy;
      retainedDirty = true;
      draw.append(sceneCommands);
    } else {
      if (retainedDirty) {
        if (retainedScene)
          renderer.updateRetainedDrawList(retainedScene, sceneCommands);
        else
          retainedScene = renderer.createRetainedDrawList(sceneCommands);
        retainedDirty = false;
      }
      draw.retainedDrawList(retainedScene);
    }
    if (!asset) {
      DrawList canvas;
      canvas.setClip({-190, -85, 380, 180});
      canvas.roundedRect({-180, -75, 360, 150}, 16,
                         Paint::solid(Color::fromRgb8(0x203761)));
      canvas.textStatic("A real vector UI plane", {-151, -54, 310, 30},
                        label(21));
      canvas.textStatic("Text and curves remain vector", {-151, -18, 310, 25},
                        label(13, Color::fromRgb8(0xb8c9e3)));
      canvas.roundedRect(
          panelButton, 8,
          Paint::solid(Color::fromRgb8(panelHover ? 0x6d9aff : 0x426ce4)));
      canvas.text("Click count: " + std::to_string(clicks), {-63, 30, 140, 22},
                  label(14));
      drawPlane3D(draw, canvas, panel);
    }
    draw.setClip({0, 0, size.x, size.y});
    draw.text("Selected object: " + std::to_string(selected),
              {controlsX, 714, 280, 22}, label(13));
    draw.text(
        "Triangles: " + std::to_string(sceneStats.triangles) +
            " / fragments: " + std::to_string(sceneStats.visibleFragments),
        {controlsX, 744, 285, 42}, label(12));
    draw.text("Scene build: " + std::to_string(buildMs).substr(0, 5) + " ms",
              {controlsX, 795, 280, 22}, label(12));
    draw.textStatic("Drag: orbit   |   Wheel: zoom   |   Click: select",
                    {30, size.y - 61, size.x - 60, 22}, label(13));
    draw.text(status, {30, size.y - 35, size.x - 60, 22},
              label(12, Color::fromRgb8(0x8b9fbd)));
    if (smoke && !outputPath.empty() && frames == 2)
      capture(renderer.drawAndReadback(draw), outputPath);
    else
      renderer.draw(draw);
    if (smoke && ++frames >= 3)
      window.requestClose();
  }
  renderer.waitIdle();
  if (smoke && !renderer.stats().validationEnabled)
    throw std::runtime_error("Smoke tests require the Vulkan validation layer");
  if (smoke && !asset && clicks != 1)
    throw std::runtime_error("Projected UI button smoke test failed");
  if (renderer.stats().validationErrors)
    throw std::runtime_error("Vulkan validation reported errors");
  std::cout << "slugia3D: " << renderer.deviceName()
            << ", triangles=" << sceneStats.triangles
            << ", visible=" << sceneStats.visibleFragments
            << ", curve segments=" << sceneStats.curveSegments
            << ", UI clicks=" << clicks
            << ", validation errors=" << renderer.stats().validationErrors
            << '\n';
  return 0;
} catch (const std::exception &e) {
  std::cerr << "slugia3D: " << e.what() << '\n';
  return 1;
}
