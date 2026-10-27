#include "slugia/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace slugia {

namespace {
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
  (overlayMode_ ? overlayCommands_ : commands_).emplace_back(std::move(command));
}
void DrawList::retainedTextView(RetainedTextId id, const View2D& view) {
  validateView(view);
  RetainedTextCommand command{id, {}, 1, clip_, opacity_, view};
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
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint) {
  shape(id, destination, paint, clip_);
}

void DrawList::shape(ShapeId id, Rect destination, Paint paint, Rect clip) {
  if (id == 0 || destination.width <= 0.0f || destination.height <= 0.0f)
    return;
  DrawCommand command{id, destination, clip, paint, 0.0f};
  command.opacity = opacity_;
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
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
  if (overlayMode_)
    overlayCommands_.emplace_back(std::move(command));
  else
    commands_.emplace_back(std::move(command));
}

void DrawList::line(Vec2 from, Vec2 to, StrokeStyle style) {
  if (style.width <= 0.0f || style.paint.opacity <= 0.0f)
    return;
  LineCommand command{from, to, clip_, std::move(style), opacity_};
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
  if (overlayMode_)
    overlayCommands_.emplace_back(command);
  else
    commands_.emplace_back(command);
}

} // namespace slugia
