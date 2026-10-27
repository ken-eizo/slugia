#include "slughorn/slughorn.hpp"
#include "slugia/slugia.hpp"
#include "slugia/vulkan_interop.hpp"
#include "slughorn/render.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main() try {
  using namespace slugia;

#if SLUGIA_ENABLE_3D
  {
    const Mesh3D box = makeBoxMesh3D({1.0f,2.0f,3.0f});
    require(box.vertices.size() == 8 && box.triangles.size() == 12,
            "Vector3D box generator topology");
    require(!box.edges.empty() && box.triangleNeighbors.size() == box.triangles.size(),
            "Vector3D topology is prepared once");
    const Mesh3D sphere = makeUvSphereMesh3D(1.0f, 16, 8);
    const Mesh3D cylinder = makeCylinderMesh3D(1.0f, 2.0f, 16);
    const Mesh3D cone = makeConeMesh3D(1.0f, 2.0f, 16);
    require(sphere.triangles.size() > 100 && cylinder.triangles.size() == 64 &&
                cone.triangles.size() == 32,
            "Vector3D curved primitive generators");
    const Mesh3D obj = parseObjMesh3D(
      "v -1 -1 0\n"
      "v 1 -1 0\n"
      "v 1 1 0\n"
      "v -1 1 0\n"
      "f -4 -3 -2 -1\n");
    require(obj.vertices.size() == 4 && obj.triangles.size() == 2 &&
                !obj.edges.empty(),
            "Vector3D OBJ fan triangulation and negative indices");
    DrawList projected;
    Mesh3DView view;
    view.origin = {100.0f, 100.0f};
    view.transform.rotation = {0.2f, 0.4f, 0.1f};
    view.transform.scale = {1.5f, 0.75f, 2.0f};
    Mesh3DStyle style;
    style.edge.width = 1.0f;
    drawMesh3D(projected, box, view, style);
    require(!projected.commands().empty(),
            "Vector3D lowers mesh geometry into ordinary Slug DrawList commands");
    require(std::all_of(projected.commands().begin(), projected.commands().end(),
              [](const DisplayCommand& command) {
                return std::holds_alternative<DynamicVectorPathCommand>(command);
              }),
            "Vector3D mesh fill and outlines use dynamic Slug quadratic coverage only");

    {
      std::vector<QuadraticBezier2D> line;
      appendLineQuadratic(line,{0,0},{10,3});
      require(line.size()==1 && line[0].p1.x==10.0f && line[0].p1.y==3.0f &&
                  line[0].p2.x==10.0f && line[0].p2.y==3.0f,
              "Slug line encoding duplicates the endpoint control point");
      const auto roundStroke=makeLineStrokeQuadratics(
        {0,0},{10,0},2.0f,LineCap::Round);
      const Rect roundBounds=quadraticBounds(roundStroke);
      require(!roundStroke.empty() && roundBounds.x < -0.99f &&
                  roundBounds.x+roundBounds.width > 10.99f &&
                  roundBounds.y < -0.99f &&
                  roundBounds.y+roundBounds.height > 0.99f,
              "dynamic Slug round line stroke preserves full cap geometry");
      const auto cubicStroke=makeCubicStrokeQuadratics(
        {0,0},{30,-40},{70,40},{100,0},3.0f,LineCap::Round);
      require(cubicStroke.size()>4 && quadraticBounds(cubicStroke).height>3.0f,
              "Bezier stroke lowers to bounded quadratic Slug contours");

      DrawList dynamicList;
      auto rail=makeRoundedRectQuadratics({10,20,120,4},2.0f);
      dynamicList.dynamicVectorPath(
        rail,quadraticBounds(rail),Paint::solid(Color::fromRgb8(0x6699ff)));
      require(dynamicList.commands().size()==1 &&
                  std::holds_alternative<DynamicVectorPathCommand>(
                    dynamicList.commands().front()),
              "runtime vector contours remain explicit dynamic Slug commands");
    }

    {
      VectorSolid3DStyle solidStyle;
      solidStyle.outline.width=1.25f;
      solidStyle.outline.paint=Paint::solid(Color::fromRgb8(0xff35d1));
      Camera3D camera;
      camera.position={0,0,-520};
      camera.perspective=1.0f;
      const Vec3 rotation{0.22f,0.47f,0.13f};
      DrawList primitives;
      drawSphere3D(primitives,{100,100},42.0f,rotation,camera,solidStyle);
      drawCylinder3D(primitives,{230,100},46.0f,64.0f,rotation,camera,solidStyle);
      drawCone3D(primitives,{360,100},44.0f,60.0f,rotation,camera,solidStyle);
      require(primitives.commands().size()>=10,
              "analytic solids lower fill, rims, silhouettes and outlines to vector contours");
      std::size_t outlineComponents=0;
      require(std::all_of(primitives.commands().begin(), primitives.commands().end(),
                [&](const DisplayCommand& command) {
                  if(!std::holds_alternative<DynamicVectorPathCommand>(command)) return false;
                  const auto& path=std::get<DynamicVectorPathCommand>(command);
                  if(std::abs(path.paint.start.r-solidStyle.outline.paint.start.r)<1.0e-6f &&
                     std::abs(path.paint.start.g-solidStyle.outline.paint.start.g)<1.0e-6f &&
                     std::abs(path.paint.start.b-solidStyle.outline.paint.start.b)<1.0e-6f)
                    ++outlineComponents;
                  return !path.curves.empty() && path.bounds.width>0.0f &&
                         path.bounds.height>0.0f &&
                         std::all_of(path.curves.begin(),path.curves.end(),[](const auto& c){
                           return std::isfinite(c.p0.x)&&std::isfinite(c.p0.y)&&
                                  std::isfinite(c.p1.x)&&std::isfinite(c.p1.y)&&
                                  std::isfinite(c.p2.x)&&std::isfinite(c.p2.y);
                         });
                }),
              "sphere cylinder and cone have finite quadratic contours only");
      require(outlineComponents>=8,
              "sphere outline plus cylinder side/rims and cone side/rim are all explicit Slug contours");

      Camera3D ortho=camera;
      ortho.perspective=0.0f;
      DrawList orthographic;
      const float halfPi=1.5707963267948966f;
      drawCylinder3D(orthographic,{180,220},40.0f,58.0f,
                     {halfPi-1.0e-5f,0.0f,0.0f},ortho,solidStyle);
      drawCone3D(orthographic,{320,220},38.0f,54.0f,
                 {halfPi-1.0e-5f,0.0f,0.0f},ortho,solidStyle);
      require(!orthographic.commands().empty() &&
              std::all_of(orthographic.commands().begin(),orthographic.commands().end(),
                [](const DisplayCommand& command){
                  if(!std::holds_alternative<DynamicVectorPathCommand>(command)) return false;
                  const auto& path=std::get<DynamicVectorPathCommand>(command);
                  return !path.curves.empty() &&
                    std::all_of(path.curves.begin(),path.curves.end(),[](const auto& c){
                      return std::isfinite(c.p0.x)&&std::isfinite(c.p0.y)&&
                             std::isfinite(c.p1.x)&&std::isfinite(c.p1.y)&&
                             std::isfinite(c.p2.x)&&std::isfinite(c.p2.y);
                    });
                }),
              "orthographic edge-on cylinder/cone remain finite vector geometry");
    }

    const Mesh3D perfSphere = makeUvSphereMesh3D(42.0f,48,24);
    const auto begin = std::chrono::steady_clock::now();
    std::size_t emitted=0;
    for(int i=0;i<200;++i){
      DrawList dl;
      drawMesh3D(dl,perfSphere,view,style);
      emitted+=dl.commands().size();
    }
    const auto elapsed=std::chrono::duration<double,std::micro>(
      std::chrono::steady_clock::now()-begin).count()/200.0;
    Mesh3D generalSphere=perfSphere; generalSphere.convex=false;
    const auto slowBegin=std::chrono::steady_clock::now();
    for(int i=0;i<10;++i){ DrawList dl; drawMesh3D(dl,generalSphere,view,style); }
    const auto slowElapsed=std::chrono::duration<double,std::micro>(
      std::chrono::steady_clock::now()-slowBegin).count()/10.0;
    std::cout << "Vector3D sphere lowering: convex " << elapsed
              << " us/frame vs general " << slowElapsed << " us/frame, "
              << emitted/200 << " commands\n";
  }
#endif
  require(hashId("stable") == hashId("stable"), "stable widget ids");
  require(hashId("stable") != hashId("different"), "distinct widget ids");
  const auto padded = inset({10, 20, 100, 60}, Insets::symmetric(4, 8));
  require(padded.x == 18 && padded.y == 24 && padded.width == 84 && padded.height == 52,
          "Insets define deterministic padding geometry");
  const float columnSpecs[] = {24.0f, -1.0f, -2.0f};
  const auto columns = gridColumns({0, 0, 100, 20}, columnSpecs, 2.0f);
  require(columns.size() == 3 && columns[0].width == 24.0f &&
              std::abs(columns[1].width * 2.0f - columns[2].width) < 0.0001f,
          "fixed and flex columns share one geometry source");
  LinearLayout rows({0, 0, 80, 100}, Axis::Vertical, 4.0f);
  require(rows.take(20).height == 20 && rows.take(30).y == 24 && rows.remaining().y == 58,
          "linear layout advances by extent and gap");
  TextEditState editor;
  editor.reset("12.5", true);
  require(editor.insert("7") && editor.value == "7", "typing replaces a selected value");
  editor.insert("\xE3\x81\x82");
  require(editor.backspace() && editor.value == "7", "UTF-8 backspace erases one codepoint");
  editor.beginTransaction();
  editor.insert("a");
  editor.insert("b");
  editor.insert("c");
  editor.endTransaction();
  require(editor.value == "7abc", "one UI frame may contain multiple text mutations");
  require(editor.undo() && editor.value == "7", "transaction undo restores the whole frame edit");
  require(editor.redo() && editor.value == "7abc", "transaction redo restores the whole frame edit");
  editor.selectAll();
  editor.beginTransaction();
  editor.insert("\xE6\x97\xA5\xE6\x9C\xAC");
  editor.endTransaction();
  require(editor.undo() && editor.value == "7abc", "UTF-8 replacement participates in bounded history");
  require(std::abs(ease(Easing::Linear, 0.25f) - 0.25f) < 0.0001f, "linear easing");
  require(std::abs(ease(Easing::SmoothStep, 0.5f) - 0.5f) < 0.0001f, "smoothstep easing");
  auto decoded = decodeUtf8("A\xE3\x81\x82\xF0\x9F\x98\x80");
  require(decoded.size() == 3 && decoded[0] == 'A' && decoded[1] == 0x3042 && decoded[2] == 0x1f600,
          "UTF-8 decoding");
  const auto overlong = decodeUtf8(std::string_view{"\xC0\xAF", 2});
  require(overlong.size() == 2 && overlong[0] == 0xfffdU && overlong[1] == 0xfffdU,
          "overlong UTF-8 rejection");
  const auto surrogate = decodeUtf8(std::string_view{"\xED\xA0\x80", 3});
  require(!surrogate.empty() && surrogate[0] == 0xfffdU, "UTF-8 surrogate rejection");
  const auto truncated = decodeUtf8(std::string_view{"\xE3\x81", 2});
  require(truncated.size() == 2 && truncated[0] == 0xfffdU && truncated[1] == 0xfffdU,
          "truncated UTF-8 rejection");

  InputState input;
  InputWriter inputWriter(input);
  inputWriter.beginFrame();
  inputWriter.cursor({10.0f, 20.0f});
  inputWriter.cursor({13.0f, 24.0f});
  inputWriter.cursor({12.0f, 30.0f});
  inputWriter.finishFrame(1.0);
  require(input.rawMouseDeltas().size() == 3 && input.rawMouseDeltas()[1].x == 3.0f &&
              input.rawMouseDeltas()[1].y == 4.0f && input.rawMouseDelta().x == 12.0f &&
              input.rawMouseDelta().y == 30.0f,
          "per-event raw mouse deltas preserve the frame trajectory");
  inputWriter.beginFrame();
  require(input.rawMouseDeltas().empty() && input.rawMouseDelta().x == 0.0f &&
              input.rawMouseDelta().y == 0.0f,
          "raw mouse delta samples reset at frame start");
  inputWriter.key(16, InputAction::Press);
  inputWriter.key(16, InputAction::Press);
  require(input.key(16).pressed && input.key(16).down,
          "duplicate modifier samples preserve the press edge");
  inputWriter.beginFrame();
  inputWriter.key(16, InputAction::Release);
  inputWriter.key(16, InputAction::Release);
  require(input.key(16).released && !input.key(16).down,
          "duplicate modifier samples preserve the release edge");

  inputWriter.composition(U"\u304b\u306a", 1, 1);
  require(input.composition().active && input.composition().text == U"\u304b\u306a" &&
              input.composition().selectionStart == 1 && input.composition().selectionLength == 1,
          "IME composition state is exposed through the platform-independent input API");
  inputWriter.beginFrame();
  require(input.composition().active && !input.composition().changedThisFrame,
          "IME composition persists across frames without creating synthetic changes");
  inputWriter.commitComposition(U"\u78ba\u5b9a");
  require(!input.composition().active && input.composition().committedThisFrame &&
              input.textInput() == U"\u78ba\u5b9a",
          "IME commit feeds committed codepoints into normal text input");
  inputWriter.beginFrame();
  inputWriter.composition(U"\u5019\u88dc", 0, 2);
  inputWriter.focusLost();
  require(!input.composition().active && input.composition().changedThisFrame,
          "focus loss cancels active IME composition");

  const Paint sharedShader =

      Paint::shader(Color::fromRgb8(0x10e0b0), Color::fromRgb8(0x8040ff), 0.75f);
  require(sharedShader.kind == GradientKind::Shader &&
              std::abs(sharedShader.shaderParameter - 0.75f) < 0.0001f,
          "procedural paint factory");
  const Paint conic = Paint::hsvConic(0.8f);
  require(conic.kind == GradientKind::HsvConic && std::abs(conic.opacity - 0.8f) < 0.0001f &&
              std::abs(conic.origin.x - 0.5f) < 0.0001f &&
              std::abs(conic.origin.y - 0.5f) < 0.0001f,
          "HSV conic paint factory");
  DrawList paintList;
  paintList.fill(1, {0, 0, 100, 40}, sharedShader);
  StrokeStyle paintedStroke;
  paintedStroke.paint = sharedShader;
  paintList.beginOverlay();
  paintList.stroke(2, {0, 50, 100, 20}, paintedStroke);
  paintList.endOverlay();
  require(paintList.commands().size() == 1 && paintList.overlayCommands().size() == 1,
          "overlay command ordering");
  require(std::get<DrawCommand>(paintList.commands().front()).paint.kind == GradientKind::Shader &&
              std::get<DrawCommand>(paintList.overlayCommands().front()).paint.kind ==
                  GradientKind::Shader,
          "fill and line share Paint");

  static_assert(std::is_trivially_copyable_v<VulkanDeviceContext>);
  static_assert(std::is_trivially_copyable_v<VulkanFrameContext>);
  DrawList externalImageList;
  externalImageList.externalImage(3, {4, 8, 64, 32}, 16, 8);
  require(externalImageList.commands().size() == 1 &&
              std::holds_alternative<ExternalImageCommand>(externalImageList.commands().front()),
          "external image draw command is part of the public display IR");
  const auto& externalImage =
      std::get<ExternalImageCommand>(externalImageList.commands().front());
  require(externalImage.image == 3 && externalImage.width == 16 && externalImage.height == 8 &&
              externalImage.destination.x == 4 && externalImage.destination.height == 32,
          "external image draw command preserves image identity and dimensions");

  constexpr std::string_view persistentText = "persistent text without a frame copy";
  DrawList staticTextList;
  staticTextList.textStatic(persistentText, {0, 0, 300, 30}, {});
  require(std::get<TextCommand>(staticTextList.commands().front()).text() == persistentText,
          "borrowed static text command");
  std::array<TextRun, 2> persistentRuns{{
    {"mixed ", TextStyle{.fontName = "Geist", .weight = 300}},
    {"text", TextStyle{.fontName = "Geist", .weight = 900,
                       .underline = true,
                       .paint = Paint::solid(Color::fromRgb8(0xff4080))}},
  }};
  DrawList richTextList;
  richTextList.textRunsStatic(persistentRuns, {0, 0, 300, 30}, {}, 2.0f);
  const auto& richText = std::get<TextCommand>(richTextList.commands().front());
  require(richText.runs.size() == 2 && richText.runs[1].style.weight == 900 &&
          richText.runs[1].style.underline && richText.runScale == 2.0f,
          "borrowed rich text runs preserve per-range style");

  DrawList cornerList;
  cornerList.roundedRect({20, 30, 360, 72}, 12.0f, sharedShader, 100.0f);
  require(cornerList.commands().size() == 1, "analytic rounded rectangle command");
  const auto& corner = std::get<RoundedRectCommand>(cornerList.commands().front());
  require(std::abs(corner.destination.width - 360.0f) < 0.0001f &&
              std::abs(corner.destination.height - 72.0f) < 0.0001f &&
              std::abs(corner.radiiPx.topLeft - 12.0f) < 0.0001f &&
              std::abs(corner.radiiPx.bottomRight - 12.0f) < 0.0001f,
          "rounded rectangle keeps absolute pixel radius after sizing");
  DrawList individualCornerList;
  individualCornerList.roundedRect({0, 0, 200, 80}, {4, 12, 20, 28}, sharedShader,
                                   {0, 25, 50, 100},
                                   {Paint::solid(Color::fromRgb8(0xff4080)), 3.0f,
                                    StrokeAlign::Inside});
  const auto& individualCorner = std::get<RoundedRectCommand>(individualCornerList.commands().front());
  require(individualCorner.radiiPx.topLeft == 4 && individualCorner.radiiPx.bottomLeft == 28 &&
          individualCorner.continuousCorners.topRightPercent == 25 &&
          individualCorner.continuousCorners.bottomLeftPercent == 100 &&
          individualCorner.border.width == 3.0f &&
          individualCorner.border.align == StrokeAlign::Inside &&
          individualCorner.border.paint.start.r == Color::fromRgb8(0xff4080).r,
          "individual corner radius and smoothing overrides");

  // The immediate UI rectangle helper must emit the skin atlas shape instead of recursively
  // calling itself. This protects the normal example's first Windows refresh callback.
  UiSkin immediateSkin;
  immediateSkin.rectangle = 17;
  UiContext immediateUi(immediateSkin);
  InputState immediateInput;
  DrawList immediateList;
  GridModel immediateGrid{{"Column"}, {{"Cell"}}, 0, 0};
  immediateUi.beginFrame(immediateInput, immediateList);
  immediateUi.gridView(hashId("immediate-grid-regression"), {0, 0, 160, 80}, immediateGrid);
  immediateUi.endFrame();
  require(!immediateList.commands().empty(), "immediate UI grid emits draw commands");
  const bool emittedSkinRectangle = std::any_of(
      immediateList.commands().begin(), immediateList.commands().end(), [](const auto& command) {
        return std::holds_alternative<DrawCommand>(command) &&
               std::get<DrawCommand>(command).shape == 17;
      });
  require(emittedSkinRectangle,
          "immediate UI rectangle helper uses the configured rectangle atlas shape");

  {
    UiSkin sliderSkin;
    sliderSkin.circle = 23;
    UiContext sliderUi(sliderSkin);
    InputState sliderInput;
    DrawList sliderList;
    float sliderValue = 0.42f;
    sliderUi.beginFrame(sliderInput, sliderList);
    sliderUi.slider(hashId("slug-rail-regression"), "Scale",
                    {20, 20, 290, 28}, sliderValue, 0.0f, 1.0f);
    sliderUi.endFrame();
    const auto roundedRails = std::count_if(
      sliderList.commands().begin(), sliderList.commands().end(),
      [](const DisplayCommand& command) {
        return std::holds_alternative<RoundedRectCommand>(command);
      });
    require(roundedRails >= 3,
            "slider background, rail and active rail stay semantic rounded rectangles "
            "before renderer-side Slug lowering");
  }

  BorderStyle sideBorder;
  sideBorder.width = 9.0f;
  sideBorder.individualWidths = BorderWidths{1.0f, 2.0f, 3.0f, 4.0f};
  const auto resolvedSides = sideBorder.resolvedWidths();
  require(resolvedSides.top == 1.0f && resolvedSides.right == 2.0f &&
          resolvedSides.bottom == 3.0f && resolvedSides.left == 4.0f,
          "individual border widths override the uniform compatibility field");

  DrawList cubicList;
  StrokeStyle cubicStyle;
  cubicStyle.width = 3.25f;
  cubicStyle.cap = LineCap::Round;
  cubicStyle.join = LineJoin::Round;
  cubicStyle.paint = sharedShader;
  cubicList.cubicBezier({10, 20}, {80, 20}, {120, 140}, {220, 140}, cubicStyle);
  require(cubicList.commands().size() == 1, "screen-space cubic stroke command");
  const auto& cubic = std::get<CubicBezierCommand>(cubicList.commands().front());
  require(cubic.from.x == 10 && cubic.control1.x == 80 && cubic.control2.y == 140 &&
              cubic.to.x == 220 && std::abs(cubic.style.width - 3.25f) < 0.0001f,
          "cubic command preserves authored control points and width");

  VectorAtlas atlas;
  const auto lowSpec = RendererConfig::lowSpec();
  require(lowSpec.vsync && !lowSpec.enableContinuousCorners &&
              lowSpec.initialVertexCapacity == (1U << 10U) &&
              lowSpec.retainedArenaInitialBytes == (64U << 10U) &&
              lowSpec.glyphRunCacheCapacity == 128,
          "low-spec renderer profile reduces resident working sets");
  RenderDevice logicalDevice(atlas);
  const auto performance = RendererConfig::vectorPerformance();
  const auto performanceLow = RendererConfig::vectorPerformance(true);
  require(performance.enableAtlasStoragePaths && performance.enableAtlasBandRefinement &&
              performance.enableCompactPersistentCurves && performance.enablePackedAtlasBands &&
              performance.atlasBandRefinementCells == 1024 &&
              !performance.enablePathTileCulling && !performance.enableIndexedVectorQuads &&
              !performance.enableReadback && !performance.enableCpuPhaseTiming &&
              performance.gpuTimingInterval == 0 &&
              performanceLow.persistentGeometryBufferBytes == lowSpec.persistentGeometryBufferBytes &&
              performanceLow.dynamicPathCacheBytes == lowSpec.dynamicPathCacheBytes,
          "performance preset selects the verified layout within existing resource budgets");
  require(&logicalDevice.atlas() == &atlas, "RenderDevice preserves atlas identity");
  static_assert(std::is_move_constructible_v<RenderSurface>);
#if defined(_WIN32)
  static_assert(std::is_base_of_v<PlatformSurface, Win32PlatformSurface>);
#endif
  const ShapeId rectangle = atlas.addPath(Path{}.rect(0, 0, 100, 40));
  const ShapeId rounded = atlas.addPath(Path{}.roundedRect(0, 0, 100, 40, 8, 100));
  Path nestedWinding;
  nestedWinding.rect(0, 0, 100, 100).rect(25, 25, 50, 50);
  const ShapeId nestedNonZero = atlas.addPath(nestedWinding, FillRule::NonZero);
  const ShapeId nestedEvenOdd = atlas.addPath(nestedWinding, FillRule::EvenOdd);
  StrokeStyle dashed;
  dashed.width = 3;
  dashed.cap = LineCap::Round;
  dashed.dashLengths = {8, 4};
  dashed.startCap = LineCap::Square;
  dashed.endCap = LineCap::Butt;
  dashed.dashStartCap = LineCap::Butt;
  dashed.dashEndCap = LineCap::Square;
  dashed.dashCaps.resize(2);
  dashed.dashCaps[1] = {LineCap::Round, LineCap::Butt};
  const ShapeId stroke =
      atlas.addStroke(Path{}.moveTo(0, 0).cubicTo(30, 50, 70, -50, 100, 0), dashed);
  StrokeStyle tapered;
  tapered.width = 6;
  tapered.startTaper = 0.0f;
  tapered.endTaper = 1.0f;
  const ShapeId taperStroke =
      atlas.addStroke(Path{}.moveTo(0, 0).quadraticTo(50, 30, 100, 0), tapered);
  StrokeStyle svgStroke;
  svgStroke.width = 2;
  svgStroke.cap = LineCap::Round;
  svgStroke.join = LineJoin::Round;
  const ShapeId relativeSvg =
      atlas.addStroke(Path{}.svgPath("m15 14 5-5-5-5 M20 9H9.5a5.5 5.5 0 0 0 0 11H13",
                                    24.0f),
                      svgStroke);
  require(rectangle && rounded && nestedNonZero && nestedEvenOdd && stroke && taperStroke && relativeSvg,
          "shape registration");
  const auto svgMetric = atlas.metrics(relativeSvg);
  require(svgMetric && svgMetric->bearingY <= 25.1f &&
              svgMetric->bearingY - svgMetric->height >= -1.1f,
          "relative SVG path stays inside reflected viewBox");
  atlas.build();
  require(atlas.built(), "atlas build");
  const auto metric = atlas.metrics(rounded);
  require(metric && metric->width > 0 && metric->height > 0, "shape metrics");
  const auto& curveTexture = atlas.native().getCurveTextureData();
  const auto& bandTexture = atlas.native().getBandTextureData();
  require(!curveTexture.empty(), "curve texture");
  require(!bandTexture.empty(), "band texture");
  require(bandTexture.format == slughorn::Atlas::TextureData::Format::RG16UI &&
              bandTexture.bytes.size() ==
                static_cast<std::size_t>(bandTexture.width) * bandTexture.height * 4U,
          "band texture uses compact RG16UI storage");
  const auto nonZeroSampler = slughorn::render::decode(atlas.native(), slughorn::Key(nestedNonZero));
  const auto evenOddSampler = slughorn::render::decode(atlas.native(), slughorn::Key(nestedEvenOdd));
  const float nonZeroCenter = static_cast<float>(nonZeroSampler.renderSampleBanded(50.0f, 50.0f, 4.0f, 4.0f).fill);
  const float evenOddCenter = static_cast<float>(evenOddSampler.renderSampleBanded(50.0f, 50.0f, 4.0f, 4.0f).fill);
  require(nonZeroCenter > 0.99f && evenOddCenter < 0.01f,
          "Slug fill-rule metadata preserves exact nonzero/evenodd coverage");

  const std::string fontPath = findDefaultSystemFont();
  if (!fontPath.empty()) {
    VectorAtlas fontAtlas;
    require(fontAtlas.loadFont(
              fontPath, FontFace{.family = "TestFace", .weight = 400}, {'A'}) &&
            fontAtlas.loadFont(
              fontPath, FontFace{.family = "TestFace", .weight = 700}, {'A'}),
            "explicit font face registration");
    require(fontAtlas.glyph('A', "TestFace", 400) !=
            fontAtlas.glyph('A', "TestFace", 700),
            "font weight selects distinct glyph namespaces");
    require(fontAtlas.hasFontFace("TestFace", false) &&
            !fontAtlas.hasFontFace("TestFace", true),
            "font face style availability");

    VectorAtlas shapedAtlas;
    const std::vector<std::uint32_t> shapingCodepoints{
      static_cast<std::uint32_t>('A'), static_cast<std::uint32_t>('f'),
      static_cast<std::uint32_t>('i'), static_cast<std::uint32_t>('o'),
      static_cast<std::uint32_t>('c'), static_cast<std::uint32_t>('e'), 0x0301u};
    require(shapedAtlas.loadFont(
              fontPath, FontFace{.family = "ShapeFace", .weight = 400}, shapingCodepoints),
            "shaping face registration");
    require(shapedAtlas.prepareText("office", "ShapeFace", 400, false),
            "HarfBuzz prepares GSUB/GPOS glyphs before atlas build");
    require(shapedAtlas.prepareText("A\xCC\x81", "ShapeFace", 400, false),
            "HarfBuzz prepares combining-mark positioning");
    shapedAtlas.build();
    const auto shapedOffice = shapedAtlas.shapeText("office", "ShapeFace", 400, false);
    require(shapedOffice && !shapedOffice->glyphs.empty() && shapedOffice->xAdvance > 0.0f,
            "prepared text resolves to a positioned glyph run");
    require(std::all_of(
              shapedOffice->glyphs.begin(), shapedOffice->glyphs.end(),
              [](const ShapedGlyph& glyph) { return glyph.shape != 0; }),
            "every shaped glyph maps to immutable Slug atlas geometry");
    const auto shapedAccent = shapedAtlas.shapeText("A\xCC\x81", "ShapeFace", 400, false);
    require(shapedAccent && !shapedAccent->glyphs.empty(),
            "combining sequence remains shapeable after atlas finalization");
  }

  {
    namespace sui = slugui;
    sui::Component component(sui::absolute(hashId("property-root")));
    auto& properties = component.properties();
    const auto source = properties.define<float>("source", 3.0f);
    const auto doubled = properties.define<float>("doubled", 0.0f);
    const auto borderWidths = properties.define<BorderWidths>(
      "border-widths", BorderWidths{1.0f, 2.0f, 3.0f, 4.0f});
    int bindingEvaluations = 0;
    properties.bind<float>(doubled, {source.id}, [source, &bindingEvaluations](const sui::PropertyStore& values) {
      ++bindingEvaluations;
      return values.get(source) * 2.0f;
    });
    require(properties.evaluateBindings() && properties.get(doubled) == 6.0f &&
            bindingEvaluations == 1,
            "SlugUI typed binding initial evaluation");
    require(!properties.evaluateBindings() && bindingEvaluations == 1,
            "SlugUI skips unchanged bindings");
    require(properties.set(source, 5.0f) && properties.evaluateBindings() &&
            properties.get(doubled) == 10.0f && bindingEvaluations == 2,
            "SlugUI dependency revision evaluation");
    require(properties.find("doubled") == doubled.id &&
            properties.type(doubled.id) == sui::PropertyType::Scalar,
            "SlugUI construction-time property metadata");
    require(properties.type(borderWidths.id) == sui::PropertyType::BorderWidths &&
            properties.get(borderWidths).left == 4.0f,
            "SlugUI typed per-side border width property");
  }

  {
    namespace sui = slugui;
    auto icon = sui::shape(hashId("dual-shape"), 1,
                           Paint::solid(Color::fromRgb8(0x80d8ff)));
    icon.layout.width = sui::Length::physical(32.0f);
    icon.layout.height = sui::Length::physical(32.0f);
    auto& visual = std::get<sui::ShapeVisual>(icon.visual);
    visual.strokeShape = 2;
    visual.strokePaint.normal = Paint::solid(Color::fromRgb8(0xff4080));
    visual.fillPlacement = {0.125f, 0.25f, 0.5f, 0.5f};
    visual.strokePlacement = {0.0625f, 0.1875f, 0.625f, 0.625f};
    sui::Component component(std::move(icon));
    sui::Runtime runtime;
    DrawList list;
    runtime.render(component, sui::FrameInput{}, list, {0, 0, 32, 32});
    require(list.commands().size() == 2,
            "SlugUI vector fill and expanded stroke lower into one ordered batch");
    require(std::get<DrawCommand>(list.commands()[0]).shape == 1 &&
            std::get<DrawCommand>(list.commands()[1]).shape == 2,
            "SlugUI vector stroke draws after its fill");
    const auto fillDestination = std::get<DrawCommand>(list.commands()[0]).destination;
    const auto strokeDestination = std::get<DrawCommand>(list.commands()[1]).destination;
    require(fillDestination.x == 4.0f && fillDestination.y == 8.0f &&
            fillDestination.width == 16.0f && fillDestination.height == 16.0f,
            "SlugUI vector fill preserves its source-coordinate placement");
    require(strokeDestination.x == 2.0f && strokeDestination.y == 6.0f &&
            strokeDestination.width == 20.0f && strokeDestination.height == 20.0f,
            "SlugUI vector stroke preserves its source-coordinate placement");
  }

  {
    namespace sui = slugui;
    auto root = sui::column(hashId("layout-root"));
    root.layout.padding = sui::Insets::all(sui::Length::logical(5.0f));
    root.layout.spacing = sui::Length::logical(2.0f);

    auto fixed = sui::roundedRectangle(hashId("layout-fixed"), sharedShader);
    fixed.layout.width = sui::Length::percent(50.0f);
    fixed.layout.height = sui::Length::logical(10.0f);
    root.add(std::move(fixed));

    auto growing = sui::roundedRectangle(hashId("layout-growing"), sharedShader);
    growing.layout.grow = 1.0f;
    root.add(std::move(growing));

    sui::Component component(std::move(root));
    sui::Runtime runtime;
    runtime.layout(component, {0, 0, 100, 100}, 2.0f);
    const auto* fixedBox = runtime.find(hashId("layout-fixed"));
    const auto* growingBox = runtime.find(hashId("layout-growing"));
    require(fixedBox && growingBox, "SlugUI layout produces stable element boxes");
    require(fixedBox->bounds.x == 10.0f && fixedBox->bounds.y == 10.0f &&
            fixedBox->bounds.width == 40.0f && fixedBox->bounds.height == 20.0f,
            "SlugUI resolves logical pixels and percent once");
    require(growingBox->bounds.x == 10.0f && growingBox->bounds.y == 34.0f &&
            growingBox->bounds.width == 80.0f && growingBox->bounds.height == 56.0f,
            "SlugUI column grow and stretch layout");
  }

  {
    namespace sui = slugui;
    auto root = sui::row(hashId("figma-wrap-root"));
    root.layout.wrap = true;
    root.layout.spacing = sui::Length::physical(10.0f);
    root.layout.counterSpacing = sui::Length::physical(20.0f);
    for (int index = 0; index < 3; ++index) {
      auto child = sui::roundedRectangle(
          hashId(std::string("figma-wrap-child-") + std::to_string(index)), sharedShader);
      child.layout.width = sui::Length::physical(40.0f);
      child.layout.height = sui::Length::physical(10.0f);
      root.add(std::move(child));
    }
    sui::Component component(std::move(root));
    sui::Runtime runtime;
    runtime.layout(component, {0, 0, 100, 100}, 1.0f);
    const auto* first = runtime.find(hashId("figma-wrap-child-0"));
    const auto* second = runtime.find(hashId("figma-wrap-child-1"));
    const auto* third = runtime.find(hashId("figma-wrap-child-2"));
    require(first && second && third, "SlugUI wrap produces all child boxes");
    require(first->bounds.x == 0.0f && second->bounds.x == 50.0f &&
            first->bounds.y == 0.0f && second->bounds.y == 0.0f,
            "SlugUI wrap preserves primary-axis item spacing");
    require(third->bounds.x == 0.0f && third->bounds.y == 30.0f,
            "SlugUI wrap starts a new track using counter-axis spacing");
  }

  {
    namespace sui = slugui;
    auto root = sui::row(hashId("figma-absolute-flow-root"));
    root.layout.spacing = sui::Length::physical(10.0f);

    auto first = sui::roundedRectangle(hashId("figma-flow-first"), sharedShader);
    first.layout.width = sui::Length::physical(50.0f);
    first.layout.height = sui::Length::physical(20.0f);
    root.add(std::move(first));

    auto floating = sui::roundedRectangle(hashId("figma-flow-floating"), sharedShader);
    floating.layout.absolutePositioned = true;
    floating.layout.x = sui::Length::physical(120.0f);
    floating.layout.y = sui::Length::physical(10.0f);
    floating.layout.width = sui::Length::physical(30.0f);
    floating.layout.height = sui::Length::physical(20.0f);
    root.add(std::move(floating));

    auto second = sui::roundedRectangle(hashId("figma-flow-second"), sharedShader);
    second.layout.width = sui::Length::physical(40.0f);
    second.layout.height = sui::Length::physical(20.0f);
    root.add(std::move(second));

    sui::Component component(std::move(root));
    sui::Runtime runtime;
    runtime.layout(component, {0, 0, 200, 80}, 1.0f);
    const auto* firstBox = runtime.find(hashId("figma-flow-first"));
    const auto* floatingBox = runtime.find(hashId("figma-flow-floating"));
    const auto* secondBox = runtime.find(hashId("figma-flow-second"));
    require(firstBox && floatingBox && secondBox,
            "SlugUI absolute auto-layout children all produce boxes");
    require(firstBox->bounds.x == 0.0f && secondBox->bounds.x == 60.0f,
            "absolute child does not consume Row flow or spacing");
    require(floatingBox->bounds.x == 120.0f && floatingBox->bounds.y == 10.0f &&
            floatingBox->bounds.width == 30.0f && floatingBox->bounds.height == 20.0f,
            "absolute child keeps Figma x/y inside a Row parent");
  }

  {
    namespace sui = slugui;
    auto root = sui::row(hashId("figma-focus-parent"));
    root.layout.preferredWidth = sui::Length::physical(57.0f);
    root.layout.preferredHeight = sui::Length::physical(32.0f);
    root.layout.padding.top = sui::Length::physical(7.0f);
    root.layout.padding.bottom = sui::Length::physical(7.0f);
    root.layout.crossAlignment = sui::Alignment::Center;

    auto radio = sui::roundedRectangle(hashId("figma-focus-radio"), sharedShader);
    radio.layout.width = sui::Length::physical(14.0f);
    radio.layout.height = sui::Length::physical(14.0f);
    root.add(std::move(radio));

    auto ring = sui::roundedRectangle(hashId("figma-focus-ring"), sharedShader);
    ring.layout.absolutePositioned = true;
    ring.layout.x = sui::Length::physical(-4.0f);
    ring.layout.y = sui::Length::physical(5.0f);
    ring.layout.width = sui::Length::physical(22.0f);
    ring.layout.height = sui::Length::physical(22.0f);
    ring.layout.horizontalConstraint = sui::Constraint::Stretch;
    ring.layout.verticalConstraint = sui::Constraint::Stretch;
    root.add(std::move(ring));

    sui::Component component(std::move(root));
    sui::Runtime runtime;
    runtime.layout(component, {0, 0, 57, 32}, 1.0f);
    const auto* ringBox = runtime.find(hashId("figma-focus-ring"));
    require(ringBox && ringBox->bounds.x == -4.0f && ringBox->bounds.y == 5.0f &&
            ringBox->bounds.width == 22.0f && ringBox->bounds.height == 22.0f,
            "Figma absolute child coordinates ignore auto-layout padding");

    runtime.layout(component, {0, 0, 67, 42}, 1.0f);
    ringBox = runtime.find(hashId("figma-focus-ring"));
    require(ringBox && ringBox->bounds.x == -4.0f && ringBox->bounds.y == 5.0f &&
            ringBox->bounds.width == 32.0f && ringBox->bounds.height == 32.0f,
            "Figma STRETCH constraints preserve authored edge anchors");
  }

  {
    namespace sui = slugui;
    auto image = sui::roundedRectangle(
      hashId("figma-image-fit"), Paint::solid(Color::fromRgb8(0x406080)));
    image.layout.width = sui::Length::physical(320.0f);
    image.layout.height = sui::Length::physical(240.0f);
    auto& visual = std::get<sui::RoundedRectangleVisual>(image.visual);
    visual.image.enabled = true;
    visual.image.sourceWidth = 1920.0f;
    visual.image.sourceHeight = 1080.0f;
    visual.image.scaleMode = sui::ImageScaleMode::Fit;

    sui::Component component(std::move(image));
    sui::Runtime runtime;
    DrawList list;
    runtime.render(component, sui::FrameInput{}, list, {0, 0, 320, 240});
    require(list.commands().size() == 1 &&
            std::holds_alternative<RoundedRectCommand>(list.commands().front()),
            "Figma image placeholder lowers to a rounded-rect draw");
    const auto& imageCommand = std::get<RoundedRectCommand>(list.commands().front());
    require(std::abs(imageCommand.destination.x) < 0.001f &&
            std::abs(imageCommand.destination.y - 30.0f) < 0.001f &&
            std::abs(imageCommand.destination.width - 320.0f) < 0.001f &&
            std::abs(imageCommand.destination.height - 180.0f) < 0.001f,
            "Figma FIT image placeholder preserves source aspect and centers it");
  }

  {
    namespace sui = slugui;
    auto root = sui::row(hashId("figma-reverse-z-root"));
    root.layout.reverseChildPaintOrder = true;
    root.layout.spacing = sui::Length::physical(-10.0f);

    auto first = sui::roundedRectangle(hashId("figma-reverse-z-first"), sharedShader);
    first.layout.width = sui::Length::physical(30.0f);
    first.layout.height = sui::Length::physical(20.0f);
    root.add(std::move(first));

    auto second = sui::roundedRectangle(hashId("figma-reverse-z-second"), sharedShader);
    second.layout.width = sui::Length::physical(30.0f);
    second.layout.height = sui::Length::physical(20.0f);
    root.add(std::move(second));

    sui::Component component(std::move(root));
    sui::Runtime runtime;
    DrawList list;
    runtime.render(component, sui::FrameInput{}, list, {0, 0, 80, 20});
    const auto* firstBox = runtime.find(hashId("figma-reverse-z-first"));
    const auto* secondBox = runtime.find(hashId("figma-reverse-z-second"));
    require(firstBox && secondBox && firstBox->bounds.x == 0.0f && secondBox->bounds.x == 20.0f,
            "reverse paint order does not change Figma auto-layout flow positions");
    require(list.commands().size() == 2 &&
            std::get<RoundedRectCommand>(list.commands()[0]).destination.x == 20.0f &&
            std::get<RoundedRectCommand>(list.commands()[1]).destination.x == 0.0f,
            "Figma itemReverseZIndex reverses canvas stacking without reversing layout");
  }

  {
    namespace sui = slugui;
    auto root = sui::absolute(hashId("interactive-root"));
    auto button = sui::roundedRectangle(
      hashId("interactive-button"), Paint::solid(Color::fromRgb8(0x203050)),
      CornerRadii::all(6.0f), CornerSmoothing::all(100.0f));
    button.layout.x = sui::Length::physical(10.0f);
    button.layout.y = sui::Length::physical(10.0f);
    button.layout.height = sui::Length::physical(40.0f);
    button.interaction = {true, true, 1};
    auto& buttonVisual = std::get<sui::RoundedRectangleVisual>(button.visual);
    buttonVisual.paint.hovered =
      sui::ValueSource<Paint>{Paint::solid(Color::fromRgb8(0x30a060))};
    buttonVisual.paint.pressed =
      sui::ValueSource<Paint>{Paint::solid(Color::fromRgb8(0x3060d0))};
    buttonVisual.stroke.paint.normal =
      Paint::gradient(GradientKind::Linear, Color::fromRgb8(0xff4080),
                      Color::fromRgb8(0x40e0ff));
    buttonVisual.stroke.width = 2.5f;
    buttonVisual.stroke.align = StrokeAlign::Inside;
    root.add(std::move(button));

    auto popup = sui::roundedRectangle(
      hashId("overlay-popup"), Paint::solid(Color::fromRgb8(0xf09030)));
    popup.layout.x = sui::Length::physical(4.0f);
    popup.layout.y = sui::Length::physical(4.0f);
    popup.layout.width = sui::Length::physical(60.0f);
    popup.layout.height = sui::Length::physical(24.0f);
    popup.overlay = true;
    root.add(std::move(popup));

    sui::Component component(std::move(root));
    const auto buttonWidth =
      component.properties().define<sui::Length>("button-width", sui::Length::physical(100.0f));
    component.root().children[0].layout.width = buttonWidth;
    int pressed = 0;
    component.on(1, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Pressed) ++pressed;
      if (event.type == sui::EventType::Activated) {
        component.properties().set(buttonWidth, sui::Length::physical(120.0f));
      }
    });

    sui::Runtime runtime;
    DrawList uiList;
    sui::FrameInput input;
    input.cursor = {20.0f, 20.0f};
    input.primary = {.pressed = true, .released = false, .down = true};
    const auto stats = runtime.render(component, input, uiList, {0, 0, 200, 100});
    const auto* buttonBox = runtime.find(hashId("interactive-button"));
    require(pressed == 1 && stats.callbacks == 3 && stats.layoutPasses == 2,
            "SlugUI press-edge activation updates the current frame");
    require(buttonBox && buttonBox->bounds.width == 120.0f,
            "SlugUI callback property change relayouts without a frame delay");
    require(uiList.commands().size() == 1 && uiList.overlayCommands().size() == 1,
            "SlugUI overlay lowers after regular commands");
    const auto pressedColor =
      std::get<RoundedRectCommand>(uiList.commands().front()).paint.start;
    const auto resolvedBorder =
      std::get<RoundedRectCommand>(uiList.commands().front()).border;
    require(std::abs(pressedColor.r - Color::fromRgb8(0x3060d0).r) < 0.0001f &&
            std::abs(pressedColor.g - Color::fromRgb8(0x3060d0).g) < 0.0001f &&
            std::abs(pressedColor.b - Color::fromRgb8(0x3060d0).b) < 0.0001f,
            "SlugUI pressed paint resolves on the input frame");
    require(resolvedBorder.width == 2.5f &&
            resolvedBorder.align == StrokeAlign::Inside &&
            resolvedBorder.paint.kind == GradientKind::Linear,
            "SlugUI stroke paint and width lower into the current frame");
  }

  {
    namespace sui = slugui;
    sui::PropertyStore properties;
    const auto width = properties.define<sui::Length>(
      "cached-width", sui::Length::physical(80.0f));
    const auto paint = properties.define<Paint>(
      "cached-paint", Paint::solid(Color::fromRgb8(0x204080)));
    auto root = sui::absolute(hashId("cached-root"));
    auto child = sui::roundedRectangle(
      hashId("cached-child"), Paint::solid(Color::fromRgb8(0x000000)));
    child.layout.width = width;
    child.layout.height = sui::Length::physical(32.0f);
    std::get<sui::RoundedRectangleVisual>(child.visual).paint.normal = paint;
    root.add(std::move(child));
    sui::Component component(std::move(root), std::move(properties));
    sui::Runtime runtime;
    DrawList first;
    const auto firstStats = runtime.render(
      component, sui::FrameInput{}, first, {0, 0, 200, 100});
    require(firstStats.layoutPasses == 1, "SlugUI performs initial layout once");

    DrawList stable;
    const auto stableStats = runtime.render(
      component, sui::FrameInput{}, stable, {0, 0, 200, 100});
    require(stableStats.layoutPasses == 0,
            "SlugUI skips layout on unchanged frames");

    component.properties().set(paint, Paint::solid(Color::fromRgb8(0x80a020)));
    DrawList paintOnly;
    const auto paintStats = runtime.render(
      component, sui::FrameInput{}, paintOnly, {0, 0, 200, 100});
    require(paintStats.layoutPasses == 0 && !paintOnly.commands().empty(),
            "SlugUI paint-only changes bypass layout");
    component.properties().set(width, sui::Length::physical(120.0f));
    DrawList resized;
    const auto resizedStats = runtime.render(
      component, sui::FrameInput{}, resized, {0, 0, 200, 100});
    const auto* resizedBox = runtime.find(hashId("cached-child"));
    require(resizedStats.layoutPasses == 1 && resizedBox &&
            resizedBox->bounds.width == 120.0f,
            "SlugUI relayouts when a tracked geometry property changes");
  }

  {
    namespace sui = slugui;
    sui::PropertyStore defaults;
    const auto width = defaults.define<sui::Length>(
      "instance-width", sui::Length::physical(80.0f));

    auto root = sui::absolute(hashId("shared-definition-root"));
    auto child = sui::roundedRectangle(hashId("shared-definition-child"), sharedShader);
    child.layout.width = width;
    child.layout.height = sui::Length::physical(24.0f);
    child.interaction = {true, true, 77};
    root.add(std::move(child));

    auto definition = std::make_shared<const sui::ComponentDefinition>(
      std::move(root), std::move(defaults));
    sui::ComponentInstance first(definition, 101);
    sui::ComponentInstance second(definition, 102);
    require(first.instanceId() != second.instanceId() &&
            &first.root() == &second.root(),
            "SlugUI instances share one immutable definition tree");

    require(first.properties().set(width, sui::Length::physical(120.0f)),
            "SlugUI instance accepts a local property override");
    require(second.properties().get(width).value == 80.0f,
            "SlugUI instance properties remain isolated");

    sui::Runtime firstRuntime;
    sui::Runtime secondRuntime;
    firstRuntime.layout(first, {0, 0, 200, 100});
    secondRuntime.layout(second, {0, 0, 200, 100});
    const auto childId = hashId("shared-definition-child");
    const auto* firstBox = firstRuntime.find(sui::RuntimeNodeId{101, childId});
    const auto* secondBox = secondRuntime.find(sui::RuntimeNodeId{102, childId});
    require(firstBox && secondBox && firstBox->node.instance == 101 &&
            secondBox->node.instance == 102 &&
            firstBox->bounds.width == 120.0f && secondBox->bounds.width == 80.0f,
            "SlugUI runtime identity qualifies shared definition nodes by instance");

    sui::RuntimeNodeId activated = {};
    first.on(77, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) activated = event.targetNode;
    });
    sui::FrameInput input;
    input.cursor = {1.0f, 1.0f};
    input.primary = {.pressed = true, .released = false, .down = true};
    DrawList instanceDraws;
    firstRuntime.render(first, input, instanceDraws, {0, 0, 200, 100});
    require(activated == sui::RuntimeNodeId{101, childId},
            "SlugUI instance callbacks expose instance-qualified runtime identity");
  }

  {
    namespace sui = slugui;
    auto buttonRoot = sui::roundedRectangle(
      hashId("scene-shared-button"), Paint::solid(Color::fromRgb8(0x305080)));
    buttonRoot.interaction = {true, true, 91};
    auto definition = std::make_shared<const sui::ComponentDefinition>(
      std::move(buttonRoot));
    sui::ComponentInstance bottom(definition, 401);
    sui::ComponentInstance top(definition, 402);

    int bottomActivated = 0;
    int topActivated = 0;
    int topReleased = 0;
    bottom.on(91, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) ++bottomActivated;
    });
    top.on(91, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) ++topActivated;
      if (event.type == sui::EventType::Released) ++topReleased;
    });

    sui::Scene scene;
    scene.add(bottom, {0, 0, 100, 40});
    scene.add(top, {0, 0, 100, 40});
    sui::SceneRuntime sceneRuntime;

    sui::FrameInput press;
    press.cursor = {10.0f, 10.0f};
    press.primary = {.pressed = true, .released = false, .down = true};
    DrawList pressDraws;
    const auto sceneStats = sceneRuntime.render(scene, press, pressDraws);
    require(sceneStats.instances == 2 && bottomActivated == 0 && topActivated == 1,
            "SlugUI Scene dispatches overlapping input only to the top painter");
    require(sceneRuntime.activeNode().instance == 402 &&
            sceneRuntime.focusedNode().instance == 402,
            "SlugUI Scene preserves instance-qualified active/focus state");

    sui::FrameInput release;
    release.cursor = {300.0f, 300.0f};
    release.primary = {.pressed = false, .released = true, .down = false};
    DrawList releaseDraws;
    sceneRuntime.render(scene, release, releaseDraws);
    require(topReleased == 1 && !sceneRuntime.activeNode(),
            "SlugUI Scene keeps pointer capture through release outside the instance");

    const auto stableGeneration = scene.generation();
    require(!scene.setViewport(402, {0, 0, 100, 40}) &&
            scene.generation() == stableGeneration,
            "SlugUI Scene keeps its spatial generation stable for unchanged placement");
    require(scene.setViewport(402, {20, 0, 100, 40}) &&
            scene.generation() == stableGeneration + 1,
            "SlugUI Scene invalidates its spatial index only when placement changes");
  }

  {
    namespace sui = slugui;
    auto overlayRoot = sui::absolute(hashId("scene-overlay-root"));
    auto popup = sui::roundedRectangle(
      hashId("scene-overlay-popup"), Paint::solid(Color::fromRgb8(0xd08030)));
    popup.layout.width = sui::Length::physical(100.0f);
    popup.layout.height = sui::Length::physical(40.0f);
    popup.overlay = true;
    popup.interaction = {true, true, 92};
    overlayRoot.add(std::move(popup));
    auto overlayDefinition = std::make_shared<const sui::ComponentDefinition>(
      std::move(overlayRoot));
    sui::ComponentInstance overlayInstance(overlayDefinition, 411);

    auto regularRoot = sui::roundedRectangle(
      hashId("scene-regular-root"), Paint::solid(Color::fromRgb8(0x306090)));
    regularRoot.interaction = {true, true, 93};
    auto regularDefinition = std::make_shared<const sui::ComponentDefinition>(
      std::move(regularRoot));
    sui::ComponentInstance regularInstance(regularDefinition, 412);

    int overlayActivated = 0;
    int regularActivated = 0;
    overlayInstance.on(92, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) ++overlayActivated;
    });
    regularInstance.on(93, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) ++regularActivated;
    });

    sui::Scene scene;
    scene.add(overlayInstance, {0, 0, 100, 40});
    scene.add(regularInstance, {0, 0, 100, 40});
    sui::SceneRuntime sceneRuntime;
    sui::FrameInput press;
    press.cursor = {10.0f, 10.0f};
    press.primary = {.pressed = true, .released = false, .down = true};
    DrawList draws;
    sceneRuntime.render(scene, press, draws);
    require(overlayActivated == 1 && regularActivated == 0,
            "SlugUI Scene resolves overlay hit order globally across component instances");
  }

  {
    namespace sui = slugui;

    sui::PropertyStore childDefaults;
    const auto childWidth = childDefaults.define<sui::Length>(
      "nested-width", sui::Length::physical(24.0f));
    auto childRoot = sui::roundedRectangle(
      hashId("nested-child-root"), Paint::solid(Color::fromRgb8(0x5070a0)));
    childRoot.layout.width = childWidth;
    childRoot.layout.height = sui::Length::physical(30.0f);
    childRoot.interaction = {true, true, 201};
    auto childDefinition = std::make_shared<const sui::ComponentDefinition>(
      std::move(childRoot), std::move(childDefaults));

    sui::PropertyStore parentDefaults;
    const auto parentWidth = parentDefaults.define<sui::Length>(
      "child-width-input", sui::Length::physical(60.0f));
    auto parentRoot = sui::absolute(hashId("nested-parent-root"));
    auto childHost = sui::instance(
      hashId("nested-host"), 7, childDefinition, "nested-host");
    childHost.layout.x = sui::Length::physical(10.0f);
    childHost.layout.y = sui::Length::physical(5.0f);
    childHost.layout.width = sui::Length::physical(100.0f);
    childHost.layout.height = sui::Length::physical(40.0f);
    auto* childVisual = std::get_if<sui::InstanceVisual>(&childHost.visual);
    require(childVisual != nullptr, "SlugUI instance() must create InstanceVisual");
    childVisual->links.push_back({parentWidth.id, childWidth.id});
    parentRoot.add(std::move(childHost));

    auto parentDefinition = std::make_shared<const sui::ComponentDefinition>(
      std::move(parentRoot), std::move(parentDefaults));
    sui::ComponentInstance parent(parentDefinition, 501);

    sui::Scene scene;
    scene.add(parent, {0, 0, 200, 100});
    sui::SceneRuntime sceneRuntime;
    sui::FrameInput idle;
    idle.cursor = {150.0f, 80.0f};
    DrawList firstDraws;
    const auto firstStats = sceneRuntime.render(scene, idle, firstDraws);

    const auto nestedId = sui::composeInstanceId(501, 7);
    require(firstStats.instances == 2 && sceneRuntime.instance(nestedId) != nullptr,
            "SlugUI Scene retains nested component instances without flattening");
    const auto childRootId = hashId("nested-child-root");
    const auto* firstChild = sceneRuntime.find({nestedId, childRootId});
    require(firstChild && firstChild->bounds.x == 10.0f && firstChild->bounds.y == 5.0f &&
            firstChild->bounds.width == 60.0f && firstChild->bounds.height == 30.0f,
            "SlugUI nested instance resolves parent-linked properties inside host bounds");

    require(parent.properties().set(parentWidth, sui::Length::physical(80.0f)),
            "nested parent property update must change");
    DrawList changedDraws;
    sceneRuntime.render(scene, idle, changedDraws);
    const auto* changedChild = sceneRuntime.find({nestedId, childRootId});
    require(changedChild && changedChild->bounds.width == 80.0f,
            "SlugUI nested property links propagate without rebuilding the definition tree");

    int nestedActivated = 0;
    sui::RuntimeNodeId nestedActivatedNode = {};
    auto* retainedNested = sceneRuntime.instance(nestedId);
    require(retainedNested != nullptr, "SlugUI nested instance must be addressable after expansion");
    retainedNested->on(201, [&](const sui::UiEvent& event) {
      if (event.type == sui::EventType::Activated) {
        ++nestedActivated;
        nestedActivatedNode = event.targetNode;
      }
    });

    sui::FrameInput press;
    press.cursor = {20.0f, 15.0f};
    press.primary = {.pressed = true, .released = false, .down = true};
    DrawList pressDraws;
    sceneRuntime.render(scene, press, pressDraws);
    require(nestedActivated == 1 &&
            nestedActivatedNode == sui::RuntimeNodeId{nestedId, childRootId},
            "SlugUI nested interaction dispatches through retained child instance identity");
  }

  std::cout << "slugia core tests passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Test failure: " << error.what() << '\n';
  return 1;
}
