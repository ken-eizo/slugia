#pragma once

#include "slugia/types.hpp"

#include <span>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace slugia {

struct DrawCommand {
  ShapeId shape = 0;
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  Paint paint = {};
  float italicShear = 0.0f;
  float opacity = 1.0f;
};

struct TextCommand {
  std::string utf8;
  std::string_view borrowedUtf8 = {};
  std::span<const TextRun> runs = {};
  Rect bounds = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  TextStyle style = {};
  float runScale = 1.0f;
  float opacity = 1.0f;

  [[nodiscard]] std::string_view text() const {
    return borrowedUtf8.data() ? borrowedUtf8 : std::string_view(utf8);
  }
};

struct RoundedRectCommand {
  Rect destination = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  CornerRadii radiiPx = {};
  CornerSmoothing continuousCorners = {};
  Paint paint = {};
  BorderStyle border = {};
  float opacity = 1.0f;
};

// Screen-space vector commands remain semantic geometry. At frame-build time they are
// lowered to closed quadratic Slug contours and evaluated by the same horizontal/vertical
// root-coverage solver as atlas shapes. No bitmap, distance field, or analytic-AA side path.
struct LineCommand {
  Vec2 from = {};
  Vec2 to = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  StrokeStyle style = {};
  float opacity = 1.0f;
};

struct DynamicVectorPathCommand {
  std::vector<QuadraticBezier2D> curves{};
  Rect bounds{};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  Paint paint{};
  FillRule fillRule = FillRule::NonZero;
  float opacity = 1.0f;
  bool cacheable = false;
  // Coverage accumulation follows a shared union mask; used for adjacent 3D materials.
  bool additiveCoverage = false;
};

struct CubicBezierCommand {
  Vec2 from = {};
  Vec2 control1 = {};
  Vec2 control2 = {};
  Vec2 to = {};
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  StrokeStyle style = {};
  float opacity = 1.0f;
};

struct RetainedTextCommand {
  RetainedTextId text = 0;
  Vec2 position = {};
  float scale = 1.0f;
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  float opacity = 1.0f;
  std::optional<View2D> view;
};

struct RetainedDrawListCommand {
  RetainedDrawListId drawList = 0;
  Vec2 position = {};
  float scale = 1.0f;
  Rect clip = {0.0f, 0.0f, 100000.0f, 100000.0f};
  float opacity = 1.0f;
  std::optional<View2D> view;
};

// Circular centerline source. VulkanRenderer lowers its stroked outline to quadratic
// Slug contours, so arcs share the same coverage path as every other vector primitive.
struct ArcCommand {
  Vec2 center = {};
  float radius = 0.0f;
  float startRadians = 0.0f;
  float sweepRadians = 0.0f;
  Rect clip = {};
  StrokeStyle style = {};
  float opacity = 1.0f;
};

struct ExternalImageCommand {
  ExternalImageId image = 0;
  Rect destination = {};
  Rect clip = {};
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  float opacity = 1.0f;
};

class DrawList;
struct ProjectedDrawListCommand {
  std::shared_ptr<const DrawList> content;
  ProjectiveTransform2D transform{};
  Rect clip{};
  float opacity=1.0f;
};

using DisplayCommand = std::variant<DrawCommand, TextCommand, RoundedRectCommand,
                                    LineCommand, DynamicVectorPathCommand, CubicBezierCommand, RetainedTextCommand,
                                    RetainedDrawListCommand, ArcCommand, ExternalImageCommand,
                                    ProjectedDrawListCommand>;

class DrawList {
public:
  void clear();
  // Reserve recording storage without adding, removing, or reordering commands.
  void reserve(std::size_t commandCapacity, std::size_t overlayCapacity = 0);
  // Copies commands, preserving order and applying this list's current opacity/clip.
  void append(const DrawList& source);
  void append(DrawList&& source);
  // Keeps vector shapes and text vector under a 3D plane's homography. Content must
  // be an ordinary 2D list; already projected lists cannot be nested.
  void projected(const DrawList& source, ProjectiveTransform2D transform);
  void setOpacity(float opacity) { opacity_ = std::clamp(opacity, 0.0f, 1.0f); }
  [[nodiscard]] float opacity() const { return opacity_; }
  void setClip(Rect clip) {
    clip_ = clip;
  }
  [[nodiscard]] Rect clip() const {
    return clip_;
  }

  // Overlay commands are emitted after all regular commands while staying in the same GPU batch.
  void beginOverlay() {
    overlayMode_ = true;
  }
  void endOverlay() {
    overlayMode_ = false;
  }
  [[nodiscard]] bool overlayMode() const {
    return overlayMode_;
  }

  void shape(ShapeId shape, Rect destination, Paint paint);
  void shape(ShapeId shape, Rect destination, Paint paint, Rect clip);
  void fill(ShapeId shape, Rect destination, Paint paint) {
    this->shape(shape, destination, paint);
  }
  void stroke(ShapeId shape, Rect destination, Paint paint) {
    this->shape(shape, destination, paint);
  }
  void stroke(ShapeId shape, Rect destination, const StrokeStyle& style) {
    this->shape(shape, destination, style.paint);
  }
  // Applies radius after destination sizing. radiusPx is absolute framebuffer pixels.
  void roundedRect(Rect destination, float radiusPx, Paint paint,
                   float continuousCornersPercent = 0.0f, BorderStyle border = {});
  // Individual values are optional at the API level: use this overload only where a corner
  // differs. Radii remain absolute framebuffer pixels after destination sizing.
  void roundedRect(Rect destination, CornerRadii radiiPx, Paint paint,
                   CornerSmoothing continuousCorners = {}, BorderStyle border = {});
  void line(Vec2 from, Vec2 to, StrokeStyle style);
  void dynamicVectorPath(std::vector<QuadraticBezier2D> curves, Rect bounds, Paint paint,
                         FillRule fillRule = FillRule::NonZero, bool cacheable = false,
                         bool additiveCoverage = false);
  void cubicBezier(Vec2 from, Vec2 control1, Vec2 control2, Vec2 to, StrokeStyle style);
  void arc(Vec2 center, float radius, float startRadians, float sweepRadians, StrokeStyle style);
  // Draw an image registered through VulkanInterop without CPU readback.
  void externalImage(ExternalImageId image, Rect destination,
                     std::uint32_t width, std::uint32_t height);
  void text(std::string utf8, Rect bounds, TextStyle style);
  // The referenced bytes must remain alive until VulkanRenderer::draw() returns.
  void textStatic(std::string_view utf8, Rect bounds, TextStyle style);
  // The referenced runs and their strings must remain alive until VulkanRenderer::draw() returns.
  void textRunsStatic(std::span<const TextRun> runs, Rect bounds, TextStyle paragraphStyle,
                      float runScale = 1.0f);
  // Draws text whose glyph instances were retained by VulkanRenderer. Only this transform and
  // clip are dynamic, so zoom/pan does not relayout or upload the document.
  void retainedText(RetainedTextId text, Vec2 position, float scale);
  // Draws a renderer-retained DrawList. Use this for static or rarely-changing UI subtrees.
  void retainedDrawList(RetainedDrawListId drawList, Vec2 position = {}, float scale = 1.0f);
  // Double camera coordinates and viewport clipping keep extreme zoom away
  // from GPU float precision limits. Original vector outlines are preserved.
  void retainedDrawListView(RetainedDrawListId drawList, const View2D& view);
  void retainedTextView(RetainedTextId text, const View2D& view);

  [[nodiscard]] const std::vector<DisplayCommand>& commands() const {
    return commands_;
  }
  [[nodiscard]] const std::vector<DisplayCommand>& overlayCommands() const {
    return overlayCommands_;
  }

private:
  Rect clip_ = {0.0f, 0.0f, 100000.0f, 100000.0f};
  std::vector<DisplayCommand> commands_{};
  std::vector<DisplayCommand> overlayCommands_{};
  bool overlayMode_ = false;
  float opacity_ = 1.0f;
};

} // namespace slugia
