// Frozen real Figma ColorPicker-v5 AOT fixture. The renderer must NOT pass
// a white saturation/value square or a flat red hue slider. A passing GPU
// test now means the actual exported multistop and three-layer paints render.
// Exact visual fidelity still requires comparing to a native Figma PNG.
#include "slugia/slugia.hpp"
#include "colorpicker_v5.generated.hpp"

#include <cstddef>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace slugia;
static void expect(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
int main(int argc, char** argv) try {
  VectorAtlas atlas;
  regression::ColorPickerV5Generated figma(atlas);
  atlas.build();

  Window window({800, 460, "Slugia Figma import fidelity test", false, false});
  const bool research=argc>1 && std::string_view(argv[1])=="--research-profile";
  RendererConfig config=research
    ? RendererConfig::vectorPerformance():RendererConfig::lowSpec();
  config.enableReadback=true;
  config.preferSrgbAlphaCompositing=true;
  VulkanRenderer renderer(window,atlas,config);
  slugui::Runtime ui;
  DrawList draw;
  ui.render(figma.component,slugui::FrameInput{},draw,{0,0,800,460});
  // The authored Transparency slider is a rounded white Frame, a checker
  // Shape with 8 even-odd SVG paths, and a translucent red gradient. They
  // must be evaluated in one masked fragment, not re-alpha-ed three times.
  bool isolatedTransparencyGroup=false;
  for(const auto& command:draw.commands())
    if(const auto* rounded=std::get_if<RoundedRectCommand>(&command))
      if(std::abs(rounded->destination.x-490.0f)<0.01f &&
         std::abs(rounded->destination.y-23.0f)<0.01f &&
         rounded->compositeLayers) {
        const auto& layers=*rounded->compositeLayers;
        isolatedTransparencyGroup=
          layers.size()==3u && layers[0].shape==0u &&
          layers[1].shape!=0u && layers[2].shape==0u &&
          layers[2].paint.kind==GradientKind::Linear;
      }
  expect(isolatedTransparencyGroup,
    "Figma checker shape + alpha overlay must share one exact rounded Clip AA");
  const auto pixels=renderer.drawAndReadback(draw);
  if (argc == 3 && std::string_view(argv[1]) == "--capture-ppm") {
    // P6 writes only authored pixels, without the larger test viewport.
    const int captureWidth=787, captureHeight=434;
    std::ofstream output(argv[2], std::ios::binary);
    if (!output) throw std::runtime_error("could not open PPM capture output");
    output << "P6\n" << captureWidth << " " << captureHeight << "\n255\n";
    for (int y=0;y<captureHeight;++y)
      for (int x=0;x<captureWidth;++x) {
        const std::size_t at=4U*(std::size_t(y)*pixels.width+std::size_t(x));
        output.write(reinterpret_cast<const char*>(pixels.rgba.data()+at),3);
      }
    if (!output) throw std::runtime_error("could not finish PPM capture");
    std::cout << "Captured the Figma v5 ColorPicker fixture to " << argv[2] << "\n";
  }
  if(argc==4 && std::string_view(argv[1])=="--capture-zoom") {
    const double zoom=std::stod(argv[2]);
    expect(std::isfinite(zoom)&&zoom>=0.15&&zoom<=4.0,
           "ColorPicker zoom must be finite and in [0.15,4.0]");
    // The complete ColorPicker contains projected child groups, which
    // cannot be nested inside a retained view. Isolate the already lowered
    // three-layer checker/gradient Frame; retain its real composited command.
    DrawList isolated;
    auto& isolatedCommands=
      const_cast<std::vector<DisplayCommand>&>(isolated.commands());
    for(const auto& original:draw.commands())
      if(const auto* rounded=std::get_if<RoundedRectCommand>(&original))
        if(std::abs(rounded->destination.x-490.0f)<0.01f &&
           std::abs(rounded->destination.y-23.0f)<0.01f &&
           rounded->compositeLayers)
          isolatedCommands.emplace_back(original);
    expect(isolatedCommands.size()==1u,
      "Zoom capture requires the authored fused checker Frame");
    const auto retained=renderer.createRetainedDrawList(isolated);
    DrawList zoomed;
    zoomed.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x1d1d1d)));
    zoomed.retainedDrawListView(retained,
      View2D{0.0,0.0,zoom,{float(220.0-490.0*zoom),float(20.0-23.0*zoom)}});
    const auto outputPixels=renderer.drawAndReadback(zoomed);
    renderer.destroyRetainedDrawList(retained);
    std::ofstream file(argv[3],std::ios::binary);
    if(!file)throw std::runtime_error("cannot open ColorPicker zoom capture");
    file<<"P6\n"<<outputPixels.width<<" "<<outputPixels.height<<"\n255\n";
    for(std::size_t i=0;i<outputPixels.rgba.size();i+=4)
      file.write(reinterpret_cast<const char*>(outputPixels.rgba.data()+i),3);
    if(!file)throw std::runtime_error("zoom capture write failed");
    std::cout<<"ColorPicker zoom "<<zoom<<" captured "<<argv[3]<<"\n";
  }
  expect(pixels.width>=787 && pixels.height>=434,
         "Figma import image must contain the entire ColorPicker");
  const auto pixel=[&](int x,int y,int channel) {
    return static_cast<int>(pixels.rgba[4u*(std::size_t(y)*pixels.width+
                                             std::size_t(x))+std::size_t(channel)]);
  };
  // Distinct output colors prove the entire Figma gradient stack is live,
  // and hue's 13 exported stops are sampled from the GPU stop table.
  expect(pixel(35,35,0)>220 && pixel(35,35,1)>220 && pixel(35,35,2)>220,
         "Color Picker upper-left must be nearly white");
  expect(pixel(395,35,0)>210 && pixel(395,35,1)<115 && pixel(395,35,2)<115,
         "Color Picker upper-right must be red, not solid white");
  expect(pixel(35,394,0)<110 && pixel(35,394,1)<110,
         "Color Picker bottom must be dark, not solid white");
  expect(pixel(216,217,0)>100 && pixel(216,217,1)<175,
         "Color Picker center must include both gradients");
  expect(pixel(450,110,2)>180 && pixel(450,110,0)<180,
         "Hue bar quarter stop must be violet/blue, not red");
  expect(pixel(450,217,1)>160 && pixel(450,217,0)<75,
         "Hue bar middle must be green/cyan, not red");
  expect(pixel(450,310,1)>180 && pixel(450,310,0)>100,
         "Hue bar lower middle must be yellow/green");
  expect(pixel(5,5,0)<70 && pixel(5,5,1)<70 && pixel(5,5,2)<70,
         "the outer frame must keep its dark fill");
  // Real Figma checker intersections are 10.33px even-odd SVG squares,
  // rather than a screenshot-sized flat red rectangle. These subpixel
  // positions were independently area-integrated from the authored SVG
  // paths at 16x resolution (before gradient composition). Keep a strict
  // per-channel bound so AA regressions cannot be hidden by broad hue tests.
  const auto checkerNear=[&](int x,int y,int r,int g,int b,int threshold) {
    return std::abs(pixel(x,y,0)-r)<=threshold &&
           std::abs(pixel(x,y,1)-g)<=threshold &&
           std::abs(pixel(x,y,2)-b)<=threshold;
  };
  expect(checkerNear(499,358,237,202,202,5) &&
         checkerNear(509,350,244,204,204,5) &&
         checkerNear(501,197,236,96,96,5) &&
         checkerNear(499,399,239,232,232,6),
         "even-odd checker + clipped red alpha gradient must match"
         " 16x reference area filtering at fine path intersections");

  // Compare against source-over in encoded sRGB, not linear-light blending.
  // Pixel centers are far from antialiased edges. Figma's three paints are
  // white + horizontally fading red + vertically fading black.
  expect(pixel(35,217,0)>=120 && pixel(35,217,0)<=135 &&
         pixel(35,217,1)>=116 && pixel(35,217,1)<=132,
         "Figma mid-left must be ~#7f7b7b (not bright linear-light #BCBCBC)");
  expect(pixel(35,394,0)<=20 && pixel(35,394,1)<=20,
         "Figma bottom-left must be nearly black (not gamma-lightened gray)");
  expect(pixel(395,217,0)>=118 && pixel(395,217,0)<=139 &&
         pixel(395,217,1)<20,
         "Figma mid-right must be dark red in sRGB, not gamma-bright red");

  // Separate the three stroke alignment geometry cases. Semi-transparent
  // white over black must give the *same* ~50% sRGB value on either side of
  // the Figma contour, with exact coverage relative to its frame rectangle.
  {
    DrawList borders;
    borders.roundedRect({0,0,800,460},0,Paint::solid(Color::fromRgb8(0x000000)));
    BorderStyle stroke;
    stroke.paint = Paint::solid(Color::fromRgb8(0xffffff,128.0f/255.0f));
    stroke.width = 6.0f;
    const auto paintBorder = [&](float left, StrokeAlign align) {
      stroke.align = align;
      borders.roundedRect({left,140,70,60}, 0,
        Paint::solid(Color::fromRgb8(0x000000)), 0, stroke);
    };
    paintBorder(80, StrokeAlign::Inside);
    paintBorder(180, StrokeAlign::Center);
    paintBorder(280, StrokeAlign::Outside);
    const auto image = renderer.drawAndReadback(borders);
    const auto brightness = [&](int x, int y) {
      const auto offset=4u*(std::size_t(y)*image.width+std::size_t(x));
      return int(image.rgba[offset]);
    };
    const auto halfWhite = [&](int x, int y) {
      const auto v=brightness(x,y);
      return v>=120 && v<=140;
    };
    expect(brightness(79,170)<18 && halfWhite(81,170) &&
           brightness(87,170)<18,
           "INSIDE 6px stroke is confined to [80,86), not outside the frame");
    expect(halfWhite(178,170) && halfWhite(182,170) &&
           brightness(184,170)<18,
           "CENTER 6px stroke must extend 3px either side of frame contour");
    expect(halfWhite(276,170) && brightness(281,170)<18,
           "OUTSIDE 6px stroke must not encroach on the frame interior");
  }

  // Stroke must never move the local UV coordinate system of the fill.
  // Previously, a *fully opaque* inside stroke rendered the inner rectangle
  // AFTER the border, using inner bounds as the gradient mapping basis.
  // Near the border, a gray ramp therefore jumped toward black/white.
  // Figma computes its gradient over the whole authored rectangle, even
  // if an opaque 10px stroke obscures that portion.
  {
    DrawList gradientStroke;
    gradientStroke.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    BorderStyle border;
    border.width = 10.0f;
    border.align = StrokeAlign::Inside;
    border.paint = Paint::solid(Color::fromRgb8(0xffffff));
    gradientStroke.roundedRect({100,240,100,50}, 0,
      Paint::gradient(GradientKind::Linear,
        Color::fromRgb8(0x000000), Color::fromRgb8(0xffffff),
        {0.0f,0.0f}, {1.0f,0.0f}), 0, border);
    const auto image = renderer.drawAndReadback(gradientStroke);
    const auto r = [&](int x) {
      return int(image.rgba[4u*(std::size_t(265)*image.width+
                                std::size_t(x))]);
    };
    // Center of x=112 corresponds to 12.5% of the original 100px rectangle
    // and therefore sRGB gray 32; remapping to the inset width would give 8.
    expect(r(105)>245 && r(112)>=26 && r(112)<=39 &&
           r(187)>=215 && r(187)<=230 && r(195)>245,
           "Opaque inside strokes may not rebase gradients or overwrite AA edges");
  }

  // Oversized Figma corner radius (50px on a 36px-high pill) with
  // asymmetric stroke widths. Radius normalization MUST occur before the
  // 10px inside offset: the corner is effectively 18px, not 50px.
  // Without this, the inner ellipse shifts ~2px at the upper-left arc.
  {
    DrawList pill;
    pill.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    BorderStyle border;
    border.paint=Paint::solid(Color::fromRgb8(0xffffff));
    border.align=StrokeAlign::Inside;
    border.individualWidths=BorderWidths{0.0f,0.0f,0.0f,10.0f};
    pill.roundedRect({200,240,100,36},50,
      Paint::solid(Color::fromRgb8(0xff0000)),0,border);
    const auto image=renderer.drawAndReadback(pill);
    const auto at=[&](int x,int y,int channel) {
      return int(image.rgba[4u*(std::size_t(y)*image.width+
                                std::size_t(x))+std::size_t(channel)]);
    };
    expect(at(208,245,0)>225 && at(208,245,1)>225,
           "Left-side inside border should cover its curved outer fringe");
    expect(at(213,245,0)>225 && at(213,245,1)<20 &&
           at(213,245,2)<20,
           "Pill inner corner is shifted when oversized radii normalize after stroke inset");
  }

  // Pixel-exact screenshot coordinates from the real Figma v5 file.
  // The hue slider is x=[436,465), width=29, inside stroke=3px. The
  // preview Frame is x=[544,638), stroke=center 2px, with clipped children.
  {
    const auto within = [](int value,int expected,int tolerance=3) {
      return std::abs(value-expected)<=tolerance;
    };
    expect(within(pixel(436,217,0),38) && within(pixel(438,217,0),38) &&
           pixel(435,217,0)==29 && pixel(439,217,0)<5 &&
           within(pixel(462,217,0),38) && within(pixel(464,217,0),38) &&
           pixel(465,217,0)==29,
           "Hue slider 3px inside ring shifted away from Figma boundaries");
    expect(within(pixel(543,55,0),86) &&
           within(pixel(544,55,1),64) &&
           pixel(545,55,1)<5 &&
           within(pixel(637,55,1),64) &&
           within(pixel(638,55,0),86) &&
           pixel(639,55,0)==29,
           "Preview Frame 2px centered ring shifted or clipped by children");
  }

  // Fill alpha and paint-level opacity are independent multiplicative
  // controls and may not be prematurely premultiplied or applied twice.
  {
    DrawList alpha;
    const Rect area{30,30,80,80};
    alpha.roundedRect(area,0,Paint::solid(Color::fromRgb8(0xffffff)));
    alpha.roundedRect(area,0,Paint::solid(Color::fromRgb8(0x000000,128.0f/255.0f)));
    alpha.roundedRect(area,0,Paint::solid(Color::fromRgb8(0x000000),0.5f));
    const auto image=renderer.drawAndReadback(alpha);
    const auto at=4u*(std::size_t(70)*image.width+std::size_t(70));
    for (int channel=0;channel<3;++channel)
      expect(int(image.rgba[at+std::size_t(channel)])>=58 &&
             int(image.rgba[at+std::size_t(channel)])<=70,
             "two 50% black overlays on white must yield 25% sRGB gray");
  }
  // Coverage dominance is the invariant that prevents a 0.938px INSIDE
  // border from leaking a subpixel outside its parent fill. At every pixel,
  // ring coverage must be <= coverage of the original filled shape.
  // This catches subtly mismatched AA contours even when coarse 6px straight
  // edge tests all pass.
  {
    constexpr std::array<float,3> widths{0.938f, 1.0f, 3.0f};
    constexpr std::array<float,3> fractions{0.125f, 0.37f, 0.83f};
    int worstExcess = 0, worstX = -1, worstY = -1;
    int worstDoubleAA = 0, worstDoubleX = -1, worstDoubleY = -1;
    for (const float width : widths) {
      for (const float shift : fractions) {
        const Rect shape{300.0f + shift, 170.0f + shift, 101.0f, 36.0f};
        DrawList fillOnly, strokeOnly, together;
        fillOnly.roundedRect({0,0,800,460},0,
          Paint::solid(Color::fromRgb8(0x000000)));
        strokeOnly.roundedRect({0,0,800,460},0,
          Paint::solid(Color::fromRgb8(0x000000)));
        together.roundedRect({0,0,800,460},0,
          Paint::solid(Color::fromRgb8(0x000000)));
        fillOnly.roundedRect(shape, 50.0f,
          Paint::solid(Color::fromRgb8(0xffffff)));
        BorderStyle border;
        border.align = StrokeAlign::Inside;
        border.width = width;
        border.paint = Paint::solid(Color::fromRgb8(0xffffff));
        strokeOnly.roundedRect(shape, 50.0f,
          Paint::solid(Color{}, 0.0f), 0.0f, border);
        together.roundedRect(shape, 50.0f,
          Paint::solid(Color::fromRgb8(0xffffff)), 0.0f, border);
        const auto fillPixels = renderer.drawAndReadback(fillOnly);
        const auto ringPixels = renderer.drawAndReadback(strokeOnly);
        const auto togetherPixels = renderer.drawAndReadback(together);
        for (int y=167;y<210;++y)
          for (int x=297;x<405;++x) {
            const std::size_t i=4u*(std::size_t(y)*fillPixels.width+std::size_t(x));
            const int excess=int(ringPixels.rgba[i])-int(fillPixels.rgba[i]);
            if (excess>worstExcess) {worstExcess=excess;worstX=x;worstY=y;}
            const int doubleAA=int(togetherPixels.rgba[i])-int(fillPixels.rgba[i]);
            if (doubleAA>worstDoubleAA) {worstDoubleAA=doubleAA;worstDoubleX=x;worstDoubleY=y;}
          }
      }
    }
    if(worstExcess > 3)
      std::cerr << "INSIDE stroke AA leaks " << worstExcess
                << "/255 at (" << worstX << "," << worstY << ")\n";
    expect(worstExcess <= 3,
      "INSIDE border AA coverage may not exceed the original fill coverage");
    expect(worstDoubleAA <= 3,
      "INSIDE fill+stroke must be composited under one common AA silhouette "
      "(no double-painted subpixel outside the authored contour)");
  }

  // .938px outside/center strokes with radius=50 on a 36px-high pill.
  // The combined stroke + fully opaque white fill MUST have precisely the
  // silhouette of the OUTER contour, including fractional translations and
  // oversized-radius normalization. For inside, the silhouette is unchanged.
  {
    for (StrokeAlign align: {StrokeAlign::Inside,StrokeAlign::Center,
                              StrokeAlign::Outside}) {
      constexpr float strokeWidth=0.938f;
      constexpr float visualRadius=18.0f;
      const float outside=align==StrokeAlign::Outside
        ? 1.0f : align==StrokeAlign::Center ? 0.5f : 0.0f;
      const Rect authored{300.37f,170.125f,101.0f,36.0f};
      const float spread=strokeWidth*outside;
      DrawList fused, outerOnly;
      fused.roundedRect({0,0,800,460},0,
        Paint::solid(Color::fromRgb8(0x000000)));
      outerOnly.roundedRect({0,0,800,460},0,
        Paint::solid(Color::fromRgb8(0x000000)));
      BorderStyle border;
      border.paint=Paint::solid(Color::fromRgb8(0xffffff));
      border.width=strokeWidth;
      border.align=align;
      fused.roundedRect(authored,50.0f,
        Paint::solid(Color::fromRgb8(0xffffff)),0,border);
      outerOnly.roundedRect(
        {authored.x-spread,authored.y-spread,
         authored.width+2.0f*spread,authored.height+2.0f*spread},
        visualRadius+spread,
        Paint::solid(Color::fromRgb8(0xffffff)));
      const auto a=renderer.drawAndReadback(fused);
      const auto b=renderer.drawAndReadback(outerOnly);
      int worst=0;
      for (int y=166;y<212;++y)
        for (int x=297;x<405;++x) {
          const std::size_t i=4u*(std::size_t(y)*a.width+std::size_t(x));
          worst=std::max(worst,std::abs(int(a.rgba[i])-int(b.rgba[i])));
        }
      expect(worst<=4,
        "OUTSIDE/CENTER/INSIDE subpixel strokes must share the exact "
        "outer contour with the matching fill (no radius/edge drift)");
    }
  }

  // Figma mirrors its 0.938px cursor, so a projected DrawList must rebase
  // ALL contour indices in the shared band SSBO. Pixel colors must match
  // the nonprojected geometry reflected around the screen's x=500 line.
  {
    DrawList local, plain, mirrored;
    const Rect shape{130.37f,100.125f,101.0f,36.0f};
    BorderStyle border;
    border.width=0.938f;
    border.align=StrokeAlign::Inside;
    border.paint=Paint::solid(Color::fromRgb8(0xffffff,0.4f));
    const auto paint=Paint::solid(Color::fromRgb8(0xff0000));
    local.roundedRect(shape,50.0f,paint,0.0f,border);
    plain.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    mirrored.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    plain.append(local);
    ProjectiveTransform2D flip;
    flip.matrix={-1,0,500, 0,1,0, 0,0,1};
    mirrored.projected(local,flip);
    const auto a=renderer.drawAndReadback(plain);
    const auto b=renderer.drawAndReadback(mirrored);
    int worst=0;
    for(int y=96;y<142;++y)
      for(int x=126;x<236;++x) {
        const std::size_t at=4u*(std::size_t(y)*a.width+std::size_t(x));
        const std::size_t mirroredAt=
          4u*(std::size_t(y)*b.width+std::size_t(499-x));
        for(int channel=0;channel<3;++channel)
          worst=std::max(worst,
            std::abs(int(a.rgba[at+channel])-int(b.rgba[mirroredAt+channel])));
      }
    if(worst>7)
      std::cerr << "Projected fused 0.938px stroke difference " << worst << "\n";
    expect(worst<=7,
      "Figma mirrored 0.938px inside stroke must preserve exact single-pass "
      "AA after projected DrawList band descriptor rebasing");
  }

  // Figma three-paint ColorPicker: one opaque base and two semitransparent
  // overlaid fills all share the SAME AA outline. Repeating three draw calls
  // would produce opacity 1-(1-B)^3 instead of B at a 50% edge.
  {
    DrawList baseline, layered;
    baseline.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    layered.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    const Rect rect{300.37f,170.125f,101.0f,36.0f};
    const auto white=Paint::solid(Color::fromRgb8(0xffffff));
    const auto translucentWhite=Paint::solid(Color::fromRgb8(0xffffff,0.4f));
    const auto fadeWhite=Paint::gradient(GradientKind::Linear,
       Color::fromRgb8(0xffffff,0.6f),
       Color::fromRgb8(0xffffff,0.0f),
       {0,0},{1,0});
    BorderStyle border;
    border.align=StrokeAlign::Inside;
    border.width=0.938f;
    border.paint=Paint::solid(Color::fromRgb8(0xffffff,0.149f));
    baseline.roundedRect(rect,50.0f,white);
    layered.roundedRectLayers(rect,CornerRadii::all(50.0f),
                              {white,translucentWhite,fadeWhite},
                              CornerSmoothing{},border);
    expect(layered.commands().size()==2,
      "ColorPicker's three fills and stroke must lower to ONE draw command");
    const auto expected=renderer.drawAndReadback(baseline);
    const auto actual=renderer.drawAndReadback(layered);
    int worst=0;
    for(int y=166;y<210;++y)
      for(int x=297;x<405;++x){
        const std::size_t i=4u*(std::size_t(y)*expected.width+std::size_t(x));
        worst=std::max(worst,
          std::abs(int(actual.rgba[i])-int(expected.rgba[i])));
      }
    if(worst>3)
      std::cerr << "Figma 3-layer inside frame AA overdraw=" << worst << "\n";
    expect(worst<=3,
      "Three-layer Figma fill+inside stroke must share EXACTLY ONE AA mask");
  }

  // Clip contents must use the very SAME Slug outline and coverage as its
  // parent fill, including corner smoothing, fractional subpixel placement,
  // and normalized radii. A separate approximate superellipse SDF is NOT a
  // valid substitute for the path the rasterizer actually draws.
  {
    int greatestDifference = 0;
    float worstShift=0,worstSmooth=0;
    int worstX=0,worstY=0,worstDraw=0,worstMask=0;
    for(const float shift:{0.0f,0.375f,0.83f}) {
      const Rect r{120.0f+shift,100.0f+shift,174.0f,112.0f};
      for(const float smoothing:{0.0f,45.0f}) {
        DrawList rendered, masked;
        const Paint black=Paint::solid(Color::fromRgb8(0x000000));
        const Paint white=Paint::solid(Color::fromRgb8(0xffffff));
        rendered.roundedRect({0,0,800,460},0,black);
        masked.roundedRect({0,0,800,460},0,black);
        rendered.roundedRect(r,CornerRadii::all(58.0f),white,
                             CornerSmoothing::all(smoothing));
        auto clip = std::make_shared<std::vector<RoundedClip>>();
        clip->push_back(RoundedClip{
          r,CornerRadii::all(58.0f),CornerSmoothing::all(smoothing),
          {1,0,0, 0,1,0}});
        masked.setRoundedClips(clip);
        masked.roundedRect({r.x-4,r.y-4,r.width+8,r.height+8},0,white);
        masked.setRoundedClips({});
        const auto image=renderer.drawAndReadback(rendered);
        const auto clipped=renderer.drawAndReadback(masked);
        for(int y=95;y<217;++y)
          for(int x=115;x<300;++x) {
            const std::size_t index=4u*(std::size_t(y)*image.width+std::size_t(x));
            int delta=std::abs(int(image.rgba[index])-int(clipped.rgba[index]));
            if(delta>greatestDifference) {
              greatestDifference=delta; worstShift=shift;worstSmooth=smoothing;
              worstX=x;worstY=y;worstDraw=int(image.rgba[index]);
              worstMask=int(clipped.rgba[index]);
            }
          }
      }
    }
    if (greatestDifference>3)
      std::cerr << "Rounded Clip/Fill AA contour mismatch: "
                << greatestDifference << "/255 at " <<worstX<<","<<worstY
                <<" offset="<<worstShift<<" smoothing="<<worstSmooth
                <<" original="<<worstDraw<<" masked="<<worstMask<<"\n";
    expect(greatestDifference<=3,
      "Clip contents mask and identical Slug rounded rectangle must have "
      "the SAME coverage, smoothing and fractional outer pixels");
  }

  // Exercise Clip contents through the ACTUAL SlugUI layout/runtime path,
  // including its nested CPU clipRect and GPU roundedClipChain. It must not
  // discard the half-pixel white fringe that Slug would draw for the same
  // authored shape at a .375px offset.
  {
    namespace sui=slugui;
    const Rect r{120.375f,100.25f,174.0f,112.0f};
    const auto white=Paint::solid(Color::fromRgb8(0xffffff));
    auto root=sui::absolute(hashId("mask-aa-root"));
    auto frame=sui::roundedRectangle(hashId("mask-aa-frame"),
      Paint::solid(Color::fromRgb8(0x000000,0.0f)),
      CornerRadii::all(58.0f));
    frame.layout.x=sui::Length::physical(r.x);
    frame.layout.y=sui::Length::physical(r.y);
    frame.layout.width=sui::Length::physical(r.width);
    frame.layout.height=sui::Length::physical(r.height);
    frame.clipsChildren=true;
    auto fullBleed=sui::roundedRectangle(hashId("mask-aa-child"),white);
    fullBleed.layout.x=sui::Length::physical(-4.0f);
    fullBleed.layout.y=sui::Length::physical(-4.0f);
    fullBleed.layout.width=sui::Length::physical(r.width+8.0f);
    fullBleed.layout.height=sui::Length::physical(r.height+8.0f);
    // Keep this a genuine clipped child, not an optimized same-silhouette
    // paint stack. Its tiny border is fully outside the Frame's clip.
    auto& childVisual=std::get<sui::RoundedRectangleVisual>(fullBleed.visual);
    childVisual.stroke.width=0.01f;
    childVisual.stroke.paint.normal=white;
    frame.add(std::move(fullBleed));
    root.add(std::move(frame));
    sui::Component component(std::move(root));
    sui::Runtime runtime;
    DrawList uiCommands,expected;
    runtime.render(component,sui::FrameInput{},uiCommands,{0,0,800,460});
    expect(uiCommands.commands().size()>=2,
      "Runtime Clip contents test must create independent parent/child geometry");
    expected.roundedRect(r,58.0f,white);
    const auto actual=renderer.drawAndReadback(uiCommands);
    const auto ideal=renderer.drawAndReadback(expected);
    int worst=0;
    for(int y=95;y<217;++y)
      for(int x=115;x<300;++x){
        const auto at=4u*(std::size_t(y)*actual.width+std::size_t(x));
        worst=std::max(worst,std::abs(int(actual.rgba[at])-int(ideal.rgba[at])));
      }
    if(worst>3)
      std::cerr<<"Runtime Figma Clip contents AA mismatch="<<worst<<"/255\n";
    expect(worst<=3,
      "SlugUI Frame Clip contents must preserve exact subpixel mask coverage");
  }

  // A semi-transparent white Figma child covering its clipped opaque red
  // Frame must first composite INSIDE the common red silhouette. Drawing
  // red*coverage and white*coverage in separate framebuffer passes overpaints
  // their common fractional edge: the red channel becomes brighter than the
  // geometrically correct merged #ff8080 edge.
  {
    const Rect r{120.375f,100.25f,174.0f,112.0f};
    const auto black=Paint::solid(Color::fromRgb8(0x000000));
    const auto red=Paint::solid(Color::fromRgb8(0xff0000));
    const auto overlay=Paint::solid(Color::fromRgb8(0xffffff,0.5f));
    const auto desired=Paint::solid(Color::fromRgb8(0xff8080));
    DrawList stacked, correct;
    stacked.roundedRect({0,0,800,460},0,black);
    correct.roundedRect({0,0,800,460},0,black);
    stacked.roundedRect(r,58.0f,red);
    auto clip=std::make_shared<std::vector<RoundedClip>>();
    clip->push_back({r,CornerRadii::all(58.0f),{},{1,0,0,0,1,0}});
    stacked.setRoundedClips(clip);
    stacked.roundedRect({r.x-4,r.y-4,r.width+8,r.height+8},0,overlay);
    stacked.setRoundedClips({});
    // The bitmap and hit-test geometry are unchanged, but adjacent,
    // full-bleed paints must share ONE frame-AA command.
    expect(stacked.commands().size()==2,
      "The fully covered clipped white overlay should fold into its red frame");
    const auto& merged=std::get<RoundedRectCommand>(stacked.commands().back());
    expect(merged.fillLayers && merged.fillLayers->size()==2,
      "The fused red/white frame must preserve both original paint layers");
    correct.roundedRect(r,58.0f,desired);
    const auto actual=renderer.drawAndReadback(stacked);
    const auto expected=renderer.drawAndReadback(correct);
    int worst=0,atX=-1,atY=-1;
    for(int y=95;y<217;++y)
      for(int x=115;x<300;++x) {
        const std::size_t at=4u*(std::size_t(y)*actual.width+std::size_t(x));
        for(int ch=0;ch<3;++ch) {
          const int delta=std::abs(int(actual.rgba[at+ch])-int(expected.rgba[at+ch]));
          if(delta>worst){worst=delta;atX=x;atY=y;}
        }
      }
    if(worst>3)
      std::cerr<<"Fractionally covered red+white clip overdraw="<<worst
               <<" /255 at "<<atX<<","<<atY<<"\n";
    expect(worst<=3,
      "An opaque clipped red Frame with a full-frame semi-transparent white"
      " overlay needs a single common AA silhouette");

    // A partially covered child must NOT be fused. It has its own genuine
    // boundary and must preserve its original layering / painter order.
    DrawList partial;
    partial.roundedRect(r,58.0f,red);
    partial.setRoundedClips(clip);
    partial.roundedRect({r.x+20,r.y+20,r.width-40,r.height-40},0,overlay);
    expect(partial.commands().size()==2,
      "Partially overlapping children must remain independent draw commands");

    // Multiple full-bleed layers share one silhouette and preserve order.
    DrawList threeLayers;
    threeLayers.roundedRect(r,58.0f,red);
    threeLayers.setRoundedClips(clip);
    threeLayers.roundedRect(r,0,overlay);
    threeLayers.roundedRect(r,0,Paint::solid(Color::fromRgb8(0x000000,0.2f)));
    expect(threeLayers.commands().size()==1,
      "Consecutive full-frame masks must not repeat edge antialiasing");
    const auto& combined=std::get<RoundedRectCommand>(threeLayers.commands()[0]);
    expect(combined.fillLayers && combined.fillLayers->size()==3,
      "Full-bleed paint stacks must keep all layers in authored order");
  }

  // A projected/rotated Figma clip must evaluate the mask in its local
  // (pre-projection) coordinate system, not gl_FragCoord screen coordinates.
  // Its quadratic curve and band indices must also be rebased when the
  // projected DrawList is merged into the enclosing frame's buffers.
  {
    const Rect r{120.375f,100.25f,174.0f,112.0f};
    const auto black=Paint::solid(Color::fromRgb8(0x000000));
    const auto white=Paint::solid(Color::fromRgb8(0xffffff));
    DrawList local, normal, mirror;
    auto clip=std::make_shared<std::vector<RoundedClip>>();
    clip->push_back({r,CornerRadii::all(58.0f),
                     CornerSmoothing::all(35.0f),{1,0,0,0,1,0},
                     true});
    local.setRoundedClips(clip);
    local.roundedRect({r.x-4,r.y-4,r.width+8,r.height+8},0,white);
    normal.roundedRect({0,0,800,460},0,black);
    mirror.roundedRect({0,0,800,460},0,black);
    normal.append(local);
    ProjectiveTransform2D reflect;
    reflect.matrix={-1,0,500,0,1,0,0,0,1};
    mirror.projected(local,reflect);
    const auto a=renderer.drawAndReadback(normal);
    const auto b=renderer.drawAndReadback(mirror);
    int worst=0;
    for(int y=95;y<218;++y)
      for(int x=115;x<300;++x) {
        const auto ia=4u*(std::size_t(y)*a.width+std::size_t(x));
        const auto ib=4u*(std::size_t(y)*b.width+std::size_t(499-x));
        worst=std::max(worst,std::abs(int(a.rgba[ia])-int(b.rgba[ib])));
      }
    if(worst>6)
      std::cerr<<"Projected fractional Clip contents mismatch="<<worst<<"/255\n";
    expect(worst<=6,
      "Projected fractional Clip contents must preserve local Slug vector AA");
  }

  // The intersection of two rectangular Clip contents masks is exactly
  // one rectangle. In particular, min(coverageA,coverageB) OVERCOVERS a
  // shared subpixel corner: two 0.625-covered perpendicular edges have an
  // intersection of 0.390625, not 0.625. The GPU compiler must coalesce
  // compatible masks, preserving both fractional edge positions and their
  // source-over opacity. No extra AA samples or render target are needed.
  {
    const auto black=Paint::solid(Color::fromRgb8(0x000000));
    const auto white=Paint::solid(Color::fromRgb8(0xffffff));
    const Rect first{200.375f,100.0f,110.0f,130.0f};
    const Rect second{180.0f,150.25f,160.0f,110.0f};
    const Rect intersection{200.375f,150.25f,110.0f,79.75f};
    const auto makeMask=[](Rect r) {
      return RoundedClip{r,CornerRadii{},CornerSmoothing{}, {1,0,0,0,1,0}};
    };
    DrawList doubleClip, singleClip;
    doubleClip.roundedRect({0,0,800,460},0,black);
    singleClip.roundedRect({0,0,800,460},0,black);
    auto masks=std::make_shared<std::vector<RoundedClip>>();
    masks->push_back(makeMask(first));
    masks->push_back(makeMask(second));
    doubleClip.setRoundedClips(masks);
    doubleClip.roundedRect({150,70,250,220},0,white);
    auto one=std::make_shared<std::vector<RoundedClip>>();
    one->push_back(makeMask(intersection));
    singleClip.setRoundedClips(one);
    singleClip.roundedRect({150,70,250,220},0,white);
    const auto actual=renderer.drawAndReadback(doubleClip);
    const auto expected=renderer.drawAndReadback(singleClip);
    int worst=0;
    for(int y=145;y<235;++y)
      for(int x=195;x<315;++x){
        const auto at=4u*(std::size_t(y)*actual.width+std::size_t(x));
        for(int c=0;c<3;++c)
          worst=std::max(worst,std::abs(int(actual.rgba[at+c])-
                                      int(expected.rgba[at+c])));
      }
    if(worst>3)
      std::cerr<<"Intersecting subpixel rectangular clips differ by "
               <<worst<<"/255 from exact intersection\n";
    expect(worst<=3,
      "Coincident-space rectangular Clip contents must form their exact "
      "geometric intersection before fragment AA");
  }

  // Two rectangular masks with a 0.375-pixel gap have NO intersection.
  // Their individual antialiasing fringes can overlap, but must not create
  // ghost geometry through min(coverageA,coverageB).
  {
    DrawList disjoint;
    disjoint.roundedRect({0,0,800,460},0,
      Paint::solid(Color::fromRgb8(0x000000)));
    auto masks=std::make_shared<std::vector<RoundedClip>>();
    masks->push_back({{150,100,50.25f,90},{},{},{1,0,0,0,1,0}});
    masks->push_back({{200.625f,100,50,90},{},{},{1,0,0,0,1,0}});
    disjoint.setRoundedClips(masks);
    disjoint.roundedRect({120,90,180,120},0,
      Paint::solid(Color::fromRgb8(0xffffff)));
    const auto image=renderer.drawAndReadback(disjoint);
    int greatest=0;
    for(int y=95;y<195;++y)
      for(int x=147;x<254;++x)
        greatest=std::max(greatest,int(image.rgba[
          4u*(std::size_t(y)*image.width+std::size_t(x))]));
    expect(greatest==0,
      "Disjoint fractional rectangular clips must not produce ghost coverage");
  }

  // Invariance of a clipped white vector slab under fractional view zoom.
  // Compare each GPU pixel on the left/right mask boundary to the analytic
  // AREA of its intersection with the physical framebuffer pixel, not to
  // the result at 100% zoom. This catches hard clip/AA coordinate mismatches
  // that only appear at noninteger zoom.
  {
    const Rect maskRect{120.375f,60.25f,29.125f,240.375f};
    auto masks=std::make_shared<std::vector<RoundedClip>>();
    masks->push_back({maskRect,CornerRadii{},CornerSmoothing{},
                      {1,0,0,0,1,0}});
    DrawList source;
    source.setRoundedClips(masks);
    source.roundedRect({110,45,55,270},0,
                       Paint::solid(Color::fromRgb8(0xffffff)));
    const auto retained=renderer.createRetainedDrawList(source);
    double globalMax=0.0;
    for(const double z : {0.5,0.75,0.9,1.0,1.125,1.5,2.0}) {
      constexpr double tx=30.35,ty=18.15;
      DrawList frame;
      frame.roundedRect({0,0,800,460},0,
                         Paint::solid(Color::fromRgb8(0x000000)));
      frame.retainedDrawListView(retained,View2D{0.0,0.0,z,{tx,ty}});
      const auto image=renderer.drawAndReadback(frame);
      const int y=static_cast<int>((maskRect.y+maskRect.height*0.45)*z+ty);
      const double left=double(maskRect.x)*z+tx;
      const double right=double(maskRect.x+maskRect.width)*z+tx;
      double worst=0.0;
      for(int x=std::max(0,int(std::floor(left))-2);
              x<=int(std::ceil(right))+2 && x<800; ++x) {
        const double area=std::clamp(std::min(x+1.0,right)-std::max(double(x),left),0.0,1.0);
        const double expected=255.0*area;
        const auto at=4u*(std::size_t(y)*image.width+std::size_t(x));
        worst=std::max(worst,std::abs(double(image.rgba[at])-expected));
      }
      std::cout<<"ZOOM_CLIP scale="<<z<<" worst_edge_error="<<worst<<"/255\n";
      globalMax=std::max(globalMax,worst);
    }
    renderer.destroyRetainedDrawList(retained);
    expect(globalMax<8.0,
      "Fractional zoom of rectangular mask must preserve exact pixel-area AA");
  }

  // The AOT checker + alpha gradient must survive every orthographic
  // zoom/pan, including a fractional screen transform. Previously, the
  // masked child Slug path used SCREEN localPosition with AUTHOR-space
  // child bounds and silently disappeared when the camera moved.
  // The 10.33px Figma checker is sampled at two interior tile centers,
  // far from any AA edges, and compared with the native-resolution GPU
  // sample. A missing or displaced child fails at ALL zooms.
  {
    DrawList isolated;
    auto& commands=const_cast<std::vector<DisplayCommand>&>(isolated.commands());
    for(const auto& item:draw.commands())
      if(const auto* parent=std::get_if<RoundedRectCommand>(&item))
        if(std::abs(parent->destination.x-490.0f)<0.01f &&
           std::abs(parent->destination.y-23.0f)<0.01f &&
           parent->compositeLayers)
          commands.push_back(item);
    expect(commands.size()==1,
      "Zoom regression must select the real fused checker/gradient Frame");
    const auto retained=renderer.createRetainedDrawList(isolated);
    constexpr int originalGrayX=503,originalWhiteX=495,originalY=180;
    const auto nativeGray=pixel(originalGrayX,originalY,1);
    const auto nativeWhite=pixel(originalWhiteX,originalY,1);
    expect(nativeWhite-nativeGray>=13,
      "The original ColorPicker checker fixture must contain distinct tiles");
    for(const double zoom:{0.5,0.75,0.9,1.0,1.125,1.5,2.0}) {
      constexpr double left=490.0,top=23.0;
      DrawList frame;
      frame.roundedRect({0,0,800,460},0,
                        Paint::solid(Color::fromRgb8(0x1d1d1d)));
      frame.retainedDrawListView(retained,
        View2D{0,0,zoom,{float(220.0-left*zoom),
                        float(20.0-top*zoom)}});
      const auto image=renderer.drawAndReadback(frame);
      const auto screenX=[&](int x){
        return int(std::lround(220.0+(x+0.5-left)*zoom-0.5));
      };
      const int y=int(std::lround(20.0+
        (originalY+0.5-top)*zoom-0.5));
      const auto getG=[&](int x) {
        return int(image.rgba[4u*(std::size_t(y)*image.width+
                                  std::size_t(screenX(x)))+1u]);
      };
      const int gray=getG(originalGrayX),white=getG(originalWhiteX);
      if(std::abs(gray-nativeGray)>4||std::abs(white-nativeWhite)>4)
        std::cerr<<"Zoom masked checker mismatch at "<<zoom
                 <<"x: gray="<<gray<<" white="<<white
                 <<" native="<<nativeGray<<","<<nativeWhite<<"\n";
      expect(std::abs(gray-nativeGray)<=4 &&
             std::abs(white-nativeWhite)<=4 &&
             white-gray>=13,
        "Figma clipped even-odd checker mask and alpha gradient must retain"
        " their authored coordinates under every fractional view zoom");
    }
    renderer.destroyRetainedDrawList(retained);

    // The same mask stack also uses preprojection local coordinates when
    // mapped through a 90-degree ProjectiveTransform2D. It must not be
    // subjected to the retained-view inverse mapping a second time.
    ProjectiveTransform2D rotation;
    rotation.matrix={0,-1,700, 1,0,-300, 0,0,1};
    DrawList projected;
    projected.roundedRect({0,0,800,460},0,
                          Paint::solid(Color::fromRgb8(0x1d1d1d)));
    projected.projected(isolated,rotation);
    const auto rotated=renderer.drawAndReadback(projected);
    const auto rotG=[&](int x,int y){
      return int(rotated.rgba[4u*(std::size_t(y)*rotated.width+
                                 std::size_t(x))+1u]);
    };
    expect(std::abs(rotG(519,203)-nativeGray)<=5 &&
           std::abs(rotG(519,195)-nativeWhite)<=5 &&
           rotG(519,195)-rotG(519,203)>=12,
      "Projected even-odd masked paint stack must preserve its authored"
      " coordinate frame through a 90-degree rotation");
  }

  std::cout<<"Figma ColorPicker v5 GPU interpolation / alpha / stroke checks passed.\n";
  return 0;
} catch(const std::exception& error) {
  std::cerr<<"Figma ColorPicker pixel regression failure: "<<error.what()<<'\n';
  return 1;
}
