#pragma once
#include "slugia/draw_list.hpp"
#include "slugia/vector_atlas.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace slugia {
struct SvgImportLimits {
  std::size_t maxInputBytes = 64U << 20U;
  std::size_t maxShapes = 200000;
  std::size_t maxControlPoints = 4000000;
};
// Static SVG geometry, solid fills/strokes, fill rules, transforms and dashes.
// Unsupported painting/content is rejected, never silently reported as success.
class SvgDocument {
public:
  explicit SvgDocument(std::string_view xml,
                       const SvgImportLimits &limits = {});
  static SvgDocument load(const std::string &path,
                          const SvgImportLimits &limits = {});
  ~SvgDocument();
  SvgDocument(SvgDocument &&) noexcept;
  SvgDocument &operator=(SvgDocument &&) noexcept;
  SvgDocument(const SvgDocument &) = delete;
  SvgDocument &operator=(const SvgDocument &) = delete;
  Vec2 size() const;
  std::size_t shapeCount() const;
  void registerShapes(VectorAtlas &atlas);
  // Call after registerShapes() and atlas.build(). All curves stay in the
  // atlas.
  DrawList drawList(const VectorAtlas &atlas) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace slugia
