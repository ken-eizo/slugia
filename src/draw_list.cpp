#include "slugia/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace slugia {

namespace {
bool sameClipEdge(float lhs,float rhs) noexcept {
  return std::abs(lhs-rhs)<=0.0001f;
}
bool sameClipRect(Rect lhs,Rect rhs) noexcept {
  return sameClipEdge(lhs.x,rhs.x)&&sameClipEdge(lhs.y,rhs.y)&&
         sameClipEdge(lhs.width,rhs.width)&&sameClipEdge(lhs.height,rhs.height);
}
bool sameClipRadii(CornerRadii lhs,CornerRadii rhs) noexcept {
  return sameClipEdge(lhs.topLeft,rhs.topLeft)&&
         sameClipEdge(lhs.topRight,rhs.topRight)&&
         sameClipEdge(lhs.bottomRight,rhs.bottomRight)&&
         sameClipEdge(lhs.bottomLeft,rhs.bottomLeft);
}
bool sameClipSmoothing(CornerSmoothing lhs,CornerSmoothing rhs) noexcept {
  return sameClipEdge(lhs.topLeftPercent,rhs.topLeftPercent)&&
         sameClipEdge(lhs.topRightPercent,rhs.topRightPercent)&&
         sameClipEdge(lhs.bottomRightPercent,rhs.bottomRightPercent)&&
         sameClipEdge(lhs.bottomLeftPercent,rhs.bottomLeftPercent);
}
bool singlePassPaint(const Paint& paint) noexcept {
  return paint.kind==GradientKind::Solid||paint.kind==GradientKind::Linear;
}
// A Figma rounded Frame may contain an atlas vector checker and a translucent
// gradient. Independent AA-blended passes incorrectly double-cover their
// shared edge; strict mask matching permits single-pass group compositing.
// Assert exact mask identity, including all ancestor clips, stroke-independent
// corner normalization, and a conservative CPU clipRect that doesn't truncate
// the 0.5px Slug coverage fringe. A different child clip is NEVER folded.
bool matchesRoundedParentMask(const RoundedRectCommand& base,
                              Rect childClip,RoundedClipChain childMasks) {
  const auto& parent=base.destination;
  if(childClip.x>parent.x-0.5f||
     childClip.y>parent.y-0.5f||
     childClip.x+childClip.width<parent.x+parent.width+0.5f||
     childClip.y+childClip.height<parent.y+parent.height+0.5f)return false;
  const std::size_t n=base.roundedClips?base.roundedClips->size():0u;
  if(!childMasks||childMasks->size()!=n+1u)return false;
  for(std::size_t i=0;i<n;++i){
    const auto& a=base.roundedClips->at(i);
    const auto& b=childMasks->at(i);
    if(!sameClipRect(a.bounds,b.bounds)||
       !sameClipRadii(a.radii,b.radii)||
       !sameClipSmoothing(a.smoothing,b.smoothing)||
       a.inverseWorld!=b.inverseWorld||
       a.projectiveLocal!=b.projectiveLocal)return false;
  }
  const auto& mask=childMasks->back();
  if(!sameClipRect(mask.bounds,parent)||
     mask.inverseWorld!=std::array<float,6>{1,0,0,0,1,0}||
     !sameClipSmoothing(mask.smoothing,base.continuousCorners))
    return false;
  // Match runtime's CSS/Figma corner-sum normalization, including 50px
  // authored radii on 36px-high buttons.
  CornerRadii expected=base.radiiPx;
  float factor=1.0f;
  const auto cap=[&](float dimension,float sum){
    if(sum>dimension&&sum>1.0e-8f)
      factor=std::min(factor,dimension/sum);
  };
  cap(parent.width,expected.topLeft+expected.topRight);
  cap(parent.width,expected.bottomLeft+expected.bottomRight);
  cap(parent.height,expected.topLeft+expected.bottomLeft);
  cap(parent.height,expected.topRight+expected.bottomRight);
  factor=std::clamp(factor,0.0f,1.0f);
  expected.topLeft*=factor;expected.topRight*=factor;
  expected.bottomRight*=factor;expected.bottomLeft*=factor;
  CornerRadii normalizedMask=mask.radii;
  float maskFactor=1.0f;
  const auto maskCap=[&](float dimension,float sum) {
    if(sum>dimension&&sum>1.0e-8f)
      maskFactor=std::min(maskFactor,dimension/sum);
  };
  maskCap(parent.width,normalizedMask.topLeft+normalizedMask.topRight);
  maskCap(parent.width,normalizedMask.bottomLeft+normalizedMask.bottomRight);
  maskCap(parent.height,normalizedMask.topLeft+normalizedMask.bottomLeft);
  maskCap(parent.height,normalizedMask.topRight+normalizedMask.bottomRight);
  maskFactor=std::clamp(maskFactor,0.0f,1.0f);
  normalizedMask.topLeft*=maskFactor;normalizedMask.topRight*=maskFactor;
  normalizedMask.bottomRight*=maskFactor;normalizedMask.bottomLeft*=maskFactor;
  if(!sameClipRadii(expected,normalizedMask))return false;
  return true;
}

bool fuseFullBleedMaskedPaint(RoundedRectCommand& base,
                              const RoundedRectCommand& overlay) {
  if(base.opacity!=1.0f||overlay.opacity!=1.0f||
     base.border.resolvedWidths().maximum()>0.0f||
     overlay.border.resolvedWidths().maximum()>0.0f||
     !singlePassPaint(base.paint)||
     !singlePassPaint(overlay.paint))return false;
  if(base.fillLayers)
    for(const auto& layer:*base.fillLayers)
      if(!singlePassPaint(layer))return false;
  if(overlay.fillLayers)
    for(const auto& layer:*overlay.fillLayers)
      if(!singlePassPaint(layer))return false;
  if(overlay.radiiPx.topLeft!=0.0f||
     overlay.radiiPx.topRight!=0.0f||
     overlay.radiiPx.bottomRight!=0.0f||
     overlay.radiiPx.bottomLeft!=0.0f)return false;
  const auto& parent=base.destination;
  const auto& child=overlay.destination;
  if(child.x>parent.x+0.0001f||child.y>parent.y+0.0001f||
     child.x+child.width<parent.x+parent.width-0.0001f||
     child.y+child.height<parent.y+parent.height-0.0001f)return false;
  if(!matchesRoundedParentMask(base,overlay.clip,overlay.roundedClips))
    return false;



  std::vector<Paint> layers=base.fillLayers
    ? *base.fillLayers:std::vector<Paint>{base.paint};
  std::vector<MaskedFillLayer> masked=base.compositeLayers
    ? *base.compositeLayers:std::vector<MaskedFillLayer>{};
  const auto addLayer=[&](Paint paint){
    // Linear gradient's original UV belongs to the child rectangle, which
    // may be larger than the mask. Express its endpoints in parent's UV.
    if(paint.kind==GradientKind::Linear) {
      paint.origin.x=(child.x-parent.x+paint.origin.x*child.width)/parent.width;
      paint.origin.y=(child.y-parent.y+paint.origin.y*child.height)/parent.height;
      paint.target.x=(child.x-parent.x+paint.target.x*child.width)/parent.width;
      paint.target.y=(child.y-parent.y+paint.target.y*child.height)/parent.height;
    }
    if(base.compositeLayers)
      masked.push_back({std::move(paint),0u,parent});
    else layers.push_back(std::move(paint));
  };
  if(overlay.fillLayers) {
    for(const auto& layer:*overlay.fillLayers)addLayer(layer);
  }else addLayer(overlay.paint);
  if(base.compositeLayers) {
    if(masked.size()>32u)return false;
    base.compositeLayers=
      std::make_shared<const std::vector<MaskedFillLayer>>(std::move(masked));
  }else{
    if(layers.size()>64u)return false;
    base.fillLayers=std::make_shared<const std::vector<Paint>>(std::move(layers));
  }
  return true;
}
// A partially covered vector child is legitimate artwork inside the parent
// Frame. Keep its Slug outline as a MASKED paint layer, not as an independently
// blended masked framebuffer quad. This is single-pass group isolation for
// atlas vector children with supported solid/linear paints. No offscreen
// framebuffer, extra textures or full-screen resolve is needed.
bool fuseClippedAtlasShape(RoundedRectCommand& base,const DrawCommand& child) {
  if(base.opacity!=1.0f||child.opacity!=1.0f||
     base.border.resolvedWidths().maximum()>0.0f||
     !singlePassPaint(base.paint)||!singlePassPaint(child.paint)||
     child.italicShear!=0.0f||child.shape==0u)return false;
  if(!matchesRoundedParentMask(base,child.clip,child.roundedClips))
    return false;
  // A completely off-frame vector may be trivially culled by the normal
  // renderer. Do not merge it and then require a nonexistent GPU instance.
  const auto& frame=base.destination;
  const auto& rect=child.destination;
  if(rect.x+rect.width<frame.x-1.0f ||
     rect.y+rect.height<frame.y-1.0f ||
     rect.x>frame.x+frame.width+1.0f ||
     rect.y>frame.y+frame.height+1.0f)return false;
  std::vector<MaskedFillLayer> layers;
  if(base.compositeLayers)layers=*base.compositeLayers;
  else if(base.fillLayers) {
    for(const auto& paint:*base.fillLayers){
      if(!singlePassPaint(paint))return false;
      layers.push_back({paint,0u,base.destination});
    }
  }else layers.push_back({base.paint,0u,base.destination});
  if(layers.size()>=32u)return false;
  layers.push_back({child.paint,child.shape,child.destination});
  base.compositeLayers=
    std::make_shared<const std::vector<MaskedFillLayer>>(std::move(layers));
  base.fillLayers.reset();
  return true;
}

void validateView(const View2D& view) {
  if (!std::isfinite(view.centerX) || !std::isfinite(view.centerY) ||
      !std::isfinite(view.zoom) || view.zoom <= 0 ||
      !std::isfinite(view.origin.x) || !std::isfinite(view.origin.y))
    throw std::invalid_argument("View2D requires finite coordinates and positive zoom");
}
}

void DrawList::retainedDrawListView(RetainedDrawListId id, const View2D& view) {
  validateView(view);
  RetainedDrawListCommand command{id, {}, 1, clip_, opacity_, view};
  command.roundedClips = roundedClips_;
  (overlayMode_ ? overlayCommands_ : commands_).emplace_back(std::move(command));
}
void DrawList::retainedTextView(RetainedTextId id, const View2D& view) {
  validateView(view);
  RetainedTextCommand command{id, {}, 1, clip_, opacity_, view};
  command.roundedClips = roundedClips_;
  (overlayMode_ ? overlayCommands_ : commands_).emplace_back(std::move(command));
}

void DrawList::append(const DrawList &source) {
  if (&source == this)
    throw std::invalid_argument("Cannot append a DrawList to itself");
  DrawList copy = source;
  append(std::move(copy));
}
void DrawList::append(DrawList &&source) {
  if (&source == this)
    throw std::invalid_argument("Cannot append a DrawList to itself");
  const auto appendCommands = [&](auto &commands, bool overlay) {
    auto &target = (overlayMode_ || overlay) ? overlayCommands_ : commands_;
    target.reserve(target.size() + commands.size());
    for (auto &copy : commands) {
      std::visit(
          [&](auto &command) {
            command.opacity *= opacity_;
            const float x = std::max(clip_.x, command.clip.x),
                        y = std::max(clip_.y, command.clip.y);
            const float right = std::min(clip_.x + clip_.width,
                                         command.clip.x + command.clip.width);
            const float bottom = std::min(clip_.y + clip_.height,
                                          command.clip.y + command.clip.height);
            command.clip = {x, y, std::max(0.0f, right - x),
                            std::max(0.0f, bottom - y)};
            if (roundedClips_) {
              auto merged = std::make_shared<std::vector<RoundedClip>>(*roundedClips_);
              if (command.roundedClips) {
                merged->insert(merged->end(), command.roundedClips->begin(),
                               command.roundedClips->end());
              }
              command.roundedClips = std::move(merged);
            }
          },
          copy);
      target.push_back(std::move(copy));
    }
  };
  appendCommands(source.commands_, false);
  appendCommands(source.overlayCommands_, true);
}

void DrawList::projected(const DrawList &source,
                         ProjectiveTransform2D transform) {
  for (const float value : transform.matrix)
    if (!std::isfinite(value))
      throw std::invalid_argument("Projective transform must be finite");
  if (!std::isfinite(transform.clipDepth.x) ||
      !std::isfinite(transform.clipDepth.y) ||
      !std::isfinite(transform.clipDepth.z))
    throw std::invalid_argument("Projective depth must be finite");
  const auto nested = [](const auto &commands) {
    return std::any_of(commands.begin(), commands.end(), [](const auto &c) {
      return std::holds_alternative<ProjectedDrawListCommand>(c);
    });
  };
  if (nested(source.commands_) || nested(source.overlayCommands_))
    throw std::invalid_argument("Projected DrawLists cannot be nested");
  ProjectedDrawListCommand command{std::make_shared<DrawList>(source),
                                   transform, clip_, opacity_};
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::reserve(std::size_t commandCapacity, std::size_t overlayCapacity) {
  commands_.reserve(commandCapacity);
  overlayCommands_.reserve(overlayCapacity);
}

void DrawList::clear() {
  commands_.clear();
  overlayCommands_.clear();
  overlayMode_ = false;
  opacity_ = 1.0f;
  roundedClips_.reset();
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint) {
  shape(id, destination, paint, clip_);
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint, Rect clip) {
  if (id == 0 || destination.width <= 0.0f || destination.height <= 0.0f)
    return;
  DrawCommand command{id, destination, clip, paint, 0.0f};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  auto& stream=overlayMode_?overlayCommands_:commands_;
  if(!stream.empty()) {
    if(auto* previous=std::get_if<RoundedRectCommand>(&stream.back()))
      if(fuseClippedAtlasShape(*previous,command))return;
  }
  stream.emplace_back(std::move(command));
}

void DrawList::roundedRect(Rect destination, float radiusPx, Paint paint,
                           float continuousCornersPercent, BorderStyle border) {
  roundedRect(destination, CornerRadii::all(radiusPx), std::move(paint),
              CornerSmoothing::all(continuousCornersPercent),
              std::move(border));
}

void DrawList::roundedRect(Rect destination, CornerRadii radiiPx, Paint paint,
                           CornerSmoothing continuousCorners,
                           BorderStyle border) {
  if (destination.width <= 0.0f || destination.height <= 0.0f)
    return;
  RoundedRectCommand command{destination,       clip_, radiiPx,
                             continuousCorners, paint, border};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  auto& stream=overlayMode_?overlayCommands_:commands_;
  if(!stream.empty()) {
    if(auto* previous=std::get_if<RoundedRectCommand>(&stream.back()))
      if(fuseFullBleedMaskedPaint(*previous,command))return;
  }
  stream.emplace_back(std::move(command));
}

void DrawList::roundedRectLayers(Rect destination, CornerRadii radiiPx,
                                 std::vector<Paint> paints,
                                 CornerSmoothing smoothing, BorderStyle border) {
  if (paints.empty() || destination.width <= 0.0f || destination.height <= 0.0f)
    return;
  const Paint bottom = paints.front();
  auto& commands = overlayMode_ ? overlayCommands_ : commands_;
  const auto priorCount = commands.size();
  roundedRect(destination,radiiPx,bottom,smoothing,std::move(border));
  if (commands.size() == priorCount) {
    // The first full-bleed layer was fused into the previous masked Frame.
    // The remaining authored layers must be emitted IN ORDER, not assigned
    // to the parent's fillLayers (which would discard its masked child).
    for (std::size_t index=1; index<paints.size(); ++index)
      roundedRect(destination,radiiPx,paints[index],smoothing,{});
    return;
  }
  std::get<RoundedRectCommand>(commands.back()).fillLayers =
    std::make_shared<const std::vector<Paint>>(std::move(paints));
}

void DrawList::line(Vec2 from, Vec2 to, StrokeStyle style) {
  if (style.width <= 0.0f || style.paint.opacity <= 0.0f)
    return;
  LineCommand command{from, to, clip_, std::move(style), opacity_};
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::dynamicVectorPath(std::vector<QuadraticBezier2D> curves,
                                 Rect bounds, Paint paint, FillRule fillRule,
                                 bool cacheable, bool additiveCoverage) {
  if (curves.empty() || bounds.width <= 0.0f || bounds.height <= 0.0f ||
      paint.opacity <= 0.0f)
    return;
  DynamicVectorPathCommand command{std::move(curves), bounds,          clip_,
                                   std::move(paint),  fillRule,        opacity_,
                                   cacheable,         additiveCoverage};
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::cubicBezier(Vec2 from, Vec2 control1, Vec2 control2, Vec2 to,
                           StrokeStyle style) {
  if (style.width <= 0.0f || style.paint.opacity <= 0.0f)
    return;
  CubicBezierCommand command{from, control1, control2,
                             to,   clip_,    std::move(style)};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::externalImage(ExternalImageId image, Rect destination,
                             std::uint32_t width, std::uint32_t height) {
  if (image == 0 || width == 0 || height == 0 || destination.width <= 0.0f ||
      destination.height <= 0.0f)
    return;
  ExternalImageCommand command{image, destination, clip_,
                               width, height,      opacity_};
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(command);
  else
    commands_.emplace_back(command);
}

void DrawList::text(std::string utf8, Rect bounds, TextStyle style) {
  if (utf8.empty() || style.size <= 0.0f)
    return;
  TextCommand command{std::move(utf8), {}, {}, bounds, clip_, std::move(style)};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::arc(Vec2 center, float radius, float startRadians,
                   float sweepRadians, StrokeStyle style) {
  if (radius <= 0.0f || style.width <= 0.0f || style.paint.opacity <= 0.0f ||
      std::abs(sweepRadians) < 0.000001f)
    return;
  ArcCommand command{center,       radius, startRadians,
                     sweepRadians, clip_,  std::move(style)};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::textStatic(std::string_view utf8, Rect bounds, TextStyle style) {
  if (utf8.empty() || style.size <= 0.0f)
    return;
  TextCommand command{{}, utf8, {}, bounds, clip_, std::move(style)};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::textRunsStatic(std::span<const TextRun> runs, Rect bounds,
                              TextStyle paragraphStyle, float runScale) {
  if (runs.empty() || runScale <= 0.0f)
    return;
  TextCommand command{
      {}, {}, runs, bounds, clip_, std::move(paragraphStyle), runScale};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::retainedDrawList(RetainedDrawListId drawList, Vec2 position,
                                float scale) {
  if (drawList == 0 || scale <= 0.0f)
    return;
  RetainedDrawListCommand command{drawList, position, scale, clip_};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(command);
  else
    commands_.emplace_back(command);
}

void DrawList::retainedText(RetainedTextId text, Vec2 position, float scale) {
  if (text == 0 || scale <= 0.0f)
    return;
  RetainedTextCommand command{text, position, scale, clip_};
  command.opacity = opacity_;
  command.roundedClips = roundedClips_;
  if (overlayMode_)
    overlayCommands_.emplace_back(command);
  else
    commands_.emplace_back(command);
}

} // namespace slugia
