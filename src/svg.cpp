#include "slugia/svg.hpp"
#include <slughorn/slughorn.hpp>
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include <nanosvg.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace slugia {
namespace {
void inspect(std::string_view xml, const SvgImportLimits &limits) {
  if (xml.size() > limits.maxInputBytes)
    throw std::length_error("SVG input byte budget exceeded");
  if (xml.find('\0') != std::string_view::npos)
    throw std::invalid_argument("SVG contains a NUL byte");
  if (xml.find("<!ENTITY") != std::string_view::npos ||
      xml.find("<!DOCTYPE") != std::string_view::npos)
    throw std::invalid_argument(
        "SVG document type/entity declarations are not supported");
  const std::set<std::string_view> allowed{
      "svg",      "g",       "path",  "rect", "circle",   "ellipse", "line",
      "polyline", "polygon", "title", "desc", "metadata", "defs"};
  std::size_t elements = 0;
  std::vector<std::string_view> stack;
  bool root = false;
  for (std::size_t pos = 0;
       (pos = xml.find('<', pos)) != std::string_view::npos;) {
    if (xml.substr(pos, 4) == "<!--") {
      const auto end = xml.find("-->", pos + 4);
      if (end == std::string_view::npos)
        throw std::invalid_argument("Unterminated SVG comment");
      pos = end + 3;
      continue;
    }
    ++pos;
    if (pos < xml.size() && xml[pos] == '?') {
      const auto end = xml.find("?>", pos + 1);
      if (end == std::string_view::npos)
        throw std::invalid_argument("Unterminated SVG declaration");
      pos = end + 2;
      continue;
    }
    const bool closing = pos < xml.size() && xml[pos] == '/';
    if (closing)
      ++pos;
    const auto start = pos;
    while (pos < xml.size() &&
           (std::isalnum(static_cast<unsigned char>(xml[pos])) ||
            xml[pos] == ':' || xml[pos] == '_' || xml[pos] == '-'))
      ++pos;
    const auto name = xml.substr(start, pos - start);
    if (!allowed.contains(name))
      throw std::invalid_argument("Unsupported SVG element: " +
                                  std::string(name));
    const auto attributes = pos;
    char quote = 0;
    while (pos < xml.size()) {
      const char c = xml[pos];
      if (quote) {
        if (c == quote)
          quote = 0;
      } else if (c == '\'' || c == '"')
        quote = c;
      else if (c == '>')
        break;
      ++pos;
    }
    if (pos == xml.size() || quote)
      throw std::invalid_argument("Unterminated SVG tag");
    auto end = pos;
    while (end > attributes &&
           std::isspace(static_cast<unsigned char>(xml[end - 1])))
      --end;
    const bool empty = end > attributes && xml[end - 1] == '/';
    if (closing) {
      if (stack.empty() || stack.back() != name)
        throw std::invalid_argument("Mismatched SVG closing tag");
      stack.pop_back();
    } else {
      if (stack.empty()) {
        if (root || name != "svg")
          throw std::invalid_argument("SVG requires one svg root");
        root = true;
      }
      if (!empty)
        stack.push_back(name);
      if (stack.size() > 64)
        throw std::length_error("SVG nesting budget exceeded");
      if (name == "g") {
        const auto tag = xml.substr(attributes, pos - attributes);
        for (std::size_t i = 0;
             (i = tag.find("opacity", i)) != std::string_view::npos; ++i)
          if (i == 0 || tag[i - 1] != '-')
            throw std::invalid_argument(
                "SVG group opacity requires an isolation layer");
      }
    }
    ++pos;
    if (++elements > limits.maxShapes * 4U + 100U)
      throw std::length_error("SVG element budget exceeded");
  }
  if (!root || !stack.empty())
    throw std::invalid_argument("Incomplete SVG document");
  // NanoSVG deliberately ignores these features. Make that boundary explicit.
  for (auto feature : {"clip-path", "mask=", "filter=", "url(", "vector-effect",
                       "paint-order", "mix-blend-mode", "href=", "class="})
    if (xml.find(feature) != std::string_view::npos)
      throw std::invalid_argument("Unsupported SVG feature: " +
                                  std::string(feature));
}
Paint color(unsigned value, float opacity) {
  return Paint::solid(
      {float(value & 255U) / 255, float((value >> 8U) & 255U) / 255,
       float((value >> 16U) & 255U) / 255, float(value >> 24U) / 255},
      opacity);
}
Path pathOf(const NSVGshape &shape, bool fill) {
  Path path;
  for (auto *p = shape.paths; p; p = p->next) {
    if (p->npts < 1)
      continue;
    path.moveTo(p->pts[0], -p->pts[1]);
    for (int i = 1; i + 2 < p->npts; i += 3)
      path.cubicTo(p->pts[2 * i], -p->pts[2 * i + 1], p->pts[2 * i + 2],
                   -p->pts[2 * i + 3], p->pts[2 * i + 4], -p->pts[2 * i + 5]);
    // SVG fills implicitly close every subpath. Strokes close only authored Z.
    if (fill || p->closed)
      path.close();
  }
  return path;
}
} // namespace
struct SvgDocument::Impl {
  std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image{nullptr, nsvgDelete};
  struct Item {
    ShapeId id;
    Paint paint;
    Vec2 origin;
  };
  std::vector<Item> items;
  std::size_t shapes = 0;
  Vec2 viewport{};
  const VectorAtlas *registered = nullptr;
};
SvgDocument::SvgDocument(std::string_view xml, const SvgImportLimits &limits)
    : impl_(std::make_unique<Impl>()) {
  inspect(xml, limits);
  std::string input(xml);
  impl_->image.reset(nsvgParse(input.data(), "px", 96));
  if (!impl_->image || !std::isfinite(impl_->image->width) ||
      !std::isfinite(impl_->image->height) || impl_->image->width <= 0 ||
      impl_->image->height <= 0)
    throw std::invalid_argument("SVG requires a finite positive viewport");
  impl_->viewport = {impl_->image->width, impl_->image->height};
  std::size_t points = 0;
  for (auto *s = impl_->image->shapes; s; s = s->next) {
    if (++impl_->shapes > limits.maxShapes)
      throw std::length_error("SVG shape budget exceeded");
    if (s->fill.type > NSVG_PAINT_COLOR || s->stroke.type > NSVG_PAINT_COLOR)
      throw std::invalid_argument(
          "SVG gradients require the gradient importer");
    if (s->opacity < 1 && s->fill.type == NSVG_PAINT_COLOR &&
        s->stroke.type == NSVG_PAINT_COLOR)
      throw std::invalid_argument(
          "SVG combined fill/stroke opacity requires an isolation layer");
    for (auto *p = s->paths; p; p = p->next) {
      if (p->npts < 0 ||
          std::size_t(p->npts) > limits.maxControlPoints - points)
        throw std::length_error("SVG control point budget exceeded");
      points += std::size_t(p->npts);
      for (int i = 0; i < p->npts * 2; ++i)
        if (!std::isfinite(p->pts[i]))
          throw std::invalid_argument("SVG contains non-finite coordinates");
    }
  }
}
SvgDocument SvgDocument::load(const std::string &path,
                              const SvgImportLimits &limits) {
  const std::u8string utf8(path.begin(), path.end());
  std::ifstream in(std::filesystem::path(utf8),
                   std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("Cannot open SVG: " + path);
  const auto bytes = in.tellg();
  if (bytes < 0 || std::uint64_t(bytes) > limits.maxInputBytes)
    throw std::length_error("SVG input byte budget exceeded");
  std::string xml(std::size_t(bytes), '\0');
  in.seekg(0);
  if (!in.read(xml.data(), std::streamsize(xml.size())))
    throw std::runtime_error("Cannot read SVG: " + path);
  return SvgDocument(xml, limits);
}
SvgDocument::~SvgDocument() = default;
SvgDocument::SvgDocument(SvgDocument &&) noexcept = default;
SvgDocument &SvgDocument::operator=(SvgDocument &&) noexcept = default;
Vec2 SvgDocument::size() const { return impl_->viewport; }
std::size_t SvgDocument::shapeCount() const { return impl_->shapes; }
void SvgDocument::registerShapes(VectorAtlas &atlas) {
  if (impl_->registered)
    throw std::logic_error("SVG shapes are already registered");
  for (auto *s = impl_->image->shapes; s; s = s->next) {
    if ((s->flags & NSVG_FLAGS_VISIBLE) == 0)
      continue;
    if (s->fill.type == NSVG_PAINT_COLOR) {
      Vec2 origin;
      const auto id = atlas.addPath(pathOf(*s, true),
                                    s->fillRule == NSVG_FILLRULE_EVENODD
                                        ? FillRule::EvenOdd
                                        : FillRule::NonZero,
                                    &origin);
      if (id)
        impl_->items.push_back({id, color(s->fill.color, s->opacity), origin});
    }
    if (s->stroke.type == NSVG_PAINT_COLOR && s->strokeWidth > 0) {
      StrokeStyle style;
      style.paint = color(s->stroke.color, s->opacity);
      style.width = s->strokeWidth;
      style.cap = s->strokeLineCap == NSVG_CAP_ROUND    ? LineCap::Round
                  : s->strokeLineCap == NSVG_CAP_SQUARE ? LineCap::Square
                                                        : LineCap::Butt;
      style.join = s->strokeLineJoin == NSVG_JOIN_ROUND   ? LineJoin::Round
                   : s->strokeLineJoin == NSVG_JOIN_BEVEL ? LineJoin::Bevel
                                                          : LineJoin::Miter;
      style.dashLengths.assign(s->strokeDashArray,
                               s->strokeDashArray + s->strokeDashCount);
      style.dashOffset = s->strokeDashOffset;
      Vec2 origin;
      const auto id = atlas.addStroke(pathOf(*s, false), style, &origin);
      if (id)
        impl_->items.push_back({id, style.paint, origin});
    }
  }
  impl_->registered = &atlas;
  // The atlas now owns the vector geometry. Keep only placement/paint metadata
  // and release the duplicate parser paths before GPU and retained allocations.
  impl_->image.reset();
}
DrawList SvgDocument::drawList(const VectorAtlas &atlas) const {
  if (impl_->registered != &atlas || !atlas.native().isBuilt())
    throw std::logic_error("SVG requires its finalized atlas");
  DrawList draw;
  for (const auto &item : impl_->items) {
    const auto s = atlas.native().getShape(slughorn::Key(item.id));
    if (s)
      draw.shape(item.id,
                 {item.origin.x + s->bearingX, -item.origin.y - s->bearingY,
                  s->width, s->height},
                 item.paint);
  }
  return draw;
}
} // namespace slugia
