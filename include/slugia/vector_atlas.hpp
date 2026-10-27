#pragma once

#include "slugia/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slughorn { class Atlas; }

namespace slugia {

struct ShapeMetrics {
  float bearingX = 0.0f;
  float bearingY = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float advance = 0.0f;
};

struct FontFace {
  std::string family;
  std::uint16_t weight = 0;
  bool italic = false;
  std::string style;
};

struct FontFaceMetrics {
  float capHeight = 0.7f;
  float xHeight = 0.5f;
  float ascender = 0.8f;
  float descender = 0.2f;
  float lineGap = 0.0f;
};

struct ShapedGlyph {
  ShapeId shape = 0;
  std::uint32_t glyphIndex = 0;
  std::uint32_t cluster = 0; // UTF-8 byte offset into the source text.
  float xAdvance = 0.0f;     // normalized em units.
  float yAdvance = 0.0f;
  float xOffset = 0.0f;
  float yOffset = 0.0f;
};

struct ShapedTextRun {
  std::vector<ShapedGlyph> glyphs;
  float xAdvance = 0.0f;
  float yAdvance = 0.0f;
  bool rightToLeft = false;
};

class Path {
public:
  Path();
  Path(const Path&);
  Path(Path&&) noexcept;
  Path& operator=(const Path&);
  Path& operator=(Path&&) noexcept;
  ~Path();

  Path& moveTo(float x, float y);
  Path& lineTo(float x, float y);
  Path& quadraticTo(float cx, float cy, float x, float y);
  Path& cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
  // Append SVG path-data commands in the SVG 1.1 coordinate system. This keeps
  // icon sources (for example Lucide's 24x24 paths) as their canonical data
  // instead of flattening them into disconnected runtime line segments.
  Path& svgPath(std::string_view data, float viewBoxHeight = 0.0f);
  // Parse SVG/Figma y-down path data into Slug's y-up atlas space by reflecting y about 0.
  // This avoids exploding validated path strings into thousands of generated C++ calls.
  Path& svgPathYDown(std::string_view data);
  Path& addPath(const Path& other);
  Path& close();
  Path& rect(float x, float y, float width, float height);
  Path& roundedRect(float x, float y, float width, float height, float radius,
                    float continuousCornersPercent = 0.0f);
  Path& circle(float centerX, float centerY, float radius);
  Path& ellipse(float centerX, float centerY, float radiusX, float radiusY);
  Path& polygon(Vec2 center, float radius, std::uint32_t sides, float rotationRadians = 0.0f);
  Path& star(Vec2 center, float outerRadius, float innerRadius, std::uint32_t points,
             float rotationRadians = 0.0f);

private:
  friend class VectorAtlas;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class VectorAtlas {
public:
  VectorAtlas();
  ~VectorAtlas();
  VectorAtlas(VectorAtlas&&) noexcept;
  VectorAtlas& operator=(VectorAtlas&&) noexcept;
  VectorAtlas(const VectorAtlas&) = delete;
  VectorAtlas& operator=(const VectorAtlas&) = delete;

  // Optional authoredOrigin receives the lower-left position removed when the
  // outline is localized, in the path's y-up coordinates (useful for SVG scenes).
  ShapeId addPath(const Path& path, FillRule fillRule = FillRule::NonZero,
                  Vec2* authoredOrigin = nullptr);
  ShapeId addStroke(const Path& path, const StrokeStyle& style, Vec2* authoredOrigin = nullptr);
  // Registers a stroked path without discarding its authored square-viewBox padding.
  // This is intended for SVG icon atlases whose state variants must keep one stable quad.
  ShapeId addStrokeInSquareViewBox(const Path& path, const StrokeStyle& style,
                                   float viewBoxSize);
  bool loadFont(const std::string& fontPath, const std::vector<std::uint32_t>& codepoints = {});
  bool loadFont(const std::string& fontPath, FontFace face,
                const std::vector<std::uint32_t>& codepoints = {});
  bool loadFontMemory(std::span<const std::uint8_t> fontData, FontFace face = {},
                      const std::vector<std::uint32_t>& codepoints = {});
  // Resolves a platform-installed font by family/weight/style and registers only the glyphs
  // requested by the imported UI. Intended for Figma-authored font families.
  bool loadSystemFont(std::string_view family, std::uint16_t weight = 400, bool italic = false,
                      const std::vector<std::uint32_t>& codepoints = {},
                      std::string_view style = {});
  // Shapes text with HarfBuzz before atlas finalization and registers exactly the glyph indices
  // produced by GSUB/GPOS. This keeps static Figma/SlugUI documents compact: no whole-font atlas.
  bool prepareText(std::string_view text, std::string_view fontName = "system-ui",
                   std::uint16_t weight = 400, bool italic = false,
                   std::string_view style = {});
  void build();

  [[nodiscard]] bool built() const;
  [[nodiscard]] std::optional<ShapeMetrics> metrics(ShapeId id) const;
  [[nodiscard]] std::string fontFamily() const;
  [[nodiscard]] std::string fontStyle() const;
  [[nodiscard]] ShapeId glyph(std::uint32_t codepoint,
                              std::string_view fontName = "system-ui",
                              std::uint16_t weight = 400,
                              bool italic = false,
                              std::string_view style = {}) const;
  // Returns a fully positioned HarfBuzz run when all shaped glyphs were prepared in the atlas.
  // Dynamic/unprepared text returns nullopt so callers can use the legacy codepoint fallback.
  [[nodiscard]] std::optional<ShapedTextRun> shapeText(
      std::string_view text, std::string_view fontName = "system-ui",
      std::uint16_t weight = 400, bool italic = false,
      std::string_view style = {}) const;
  [[nodiscard]] bool hasFontFace(std::string_view fontName, bool italic,
                                 std::string_view style = {}) const;
  [[nodiscard]] std::optional<FontFaceMetrics> fontMetrics(
      std::string_view fontName, std::uint16_t weight = 400, bool italic = false,
      std::string_view style = {}) const;
  [[nodiscard]] float kerning(std::uint32_t leftCodepoint, std::uint32_t rightCodepoint,
                              std::string_view fontName,
                              std::uint16_t weight = 400, bool italic = false,
                              std::string_view style = {}) const;
  [[nodiscard]] const slughorn::Atlas& native() const;

private:
  ShapeId addStrokeImpl(const Path& path, const StrokeStyle& style,
                        std::optional<float> squareViewBoxSize, Vec2* authoredOrigin = nullptr);
  bool loadFontFace(void* nativeFace, FontFace face,
                    const std::vector<std::uint32_t>& codepoints,
                    std::shared_ptr<const std::vector<std::uint8_t>> fontData);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::vector<std::uint32_t> decodeUtf8(std::string_view text);
// Resolve an installed font by its Figma/CSS family name and requested face. The path may be
// empty when the family is not installed locally.
std::string findSystemFont(std::string_view family, std::uint16_t weight = 400,
                           bool italic = false);
std::string findDefaultSystemFont();

} // namespace slugia
