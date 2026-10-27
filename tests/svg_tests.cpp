#include "detail/path_tiles.hpp"
#include "slugia/slugia.hpp"
#include <iostream>
#include <slughorn/slughorn.hpp>
#include <stdexcept>

using namespace slugia;
namespace {
void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
bool inside(const slughorn::Atlas::Curves &curves, double x, double y) {
  int winding = 0;
  for (const auto &c : curves) {
    const double a = double(c.y1) - 2.0 * c.y2 + c.y3,
                 b = 2 * (double(c.y2) - c.y1);
    const double cut = a != 0 ? -b / (2 * a) : -1;
    const double stops[]{0, cut > 0 && cut < 1 ? cut : 1, 1};
    const auto value = [&](double t) {
      const double s = 1 - t;
      return s * s * c.y1 + 2 * s * t * c.y2 + t * t * c.y3;
    };
    for (int j = 0; j < 2; ++j) {
      double lo = stops[j], hi = stops[j + 1];
      const double y0 = value(lo), y1 = value(hi);
      if (!((y0 <= y && y < y1) || (y1 <= y && y < y0)))
        continue;
      for (int k = 0; k < 48; ++k) {
        const double t = (lo + hi) * .5;
        if ((value(t) < y) == (y1 > y0))
          lo = t;
        else
          hi = t;
      }
      const double t = (lo + hi) * .5, s = 1 - t;
      if (s * s * c.x1 + 2 * s * t * c.x2 + t * t * c.x3 > x)
        winding += y1 > y0 ? 1 : -1;
    }
  }
  return winding != 0;
}
} // namespace
int main() try {
  const auto rejects = [](std::string_view xml) {
    try {
      SvgDocument invalid(xml);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  require(rejects("<svg width='1' height='1'><g></svg>"),
          "Reject mismatched SVG tags");
  require(rejects("<svg width='1' height='1'><clipPath/></svg>"),
          "Reject unsupported SVG clipping");
  require(rejects("<svg width='1' height='1'><g opacity='.5'/></svg>"),
          "Reject unsupported group isolation");
  require(rejects("<svg width='1' height='1'"), "Reject incomplete SVG tags");
  {
    SvgDocument svg(
        R"SVG(<svg width="100" height="100"><g transform="translate(10 20)"><path d="M0 0H30V40H0" fill="#ff0000"/></g></svg>)SVG");
    VectorAtlas atlas;
    const auto viewport = svg.size();
    const auto count = svg.shapeCount();
    svg.registerShapes(atlas);
    require(svg.size().x == viewport.x && svg.size().y == viewport.y &&
                svg.shapeCount() == count,
            "SVG metadata survives releasing parser paths");
    atlas.build();
    const auto draw = svg.drawList(atlas);
    require(svg.drawList(atlas).commands().size() == draw.commands().size(),
            "SVG can rebuild draw lists without parser paths");
    require(draw.commands().size() == 1, "SVG implicit closed fill");
    const auto &c = std::get<DrawCommand>(draw.commands()[0]);
    require(std::abs(c.destination.x - 10) < 1e-5 &&
                std::abs(c.destination.y - 20) < 1e-5 &&
                std::abs(c.destination.width - 30) < 1e-5 &&
                std::abs(c.destination.height - 40) < 1e-5,
            "SVG preserves authored placement");
  }
  auto svg = SvgDocument::load(SLUGIA_TEST_TIGER);
  VectorAtlas atlas;
  svg.registerShapes(atlas);
  atlas.build();
  std::size_t tested = 0;
  for (const auto &[key, s] : atlas.native().getShapes()) {
    if (s.curves.size() < 128)
      continue;
    const auto tiles = detail::pathTiles(s);
    const double sx = float(s.bearingX + s.width) - double(s.bearingX),
                 sy = float(s.bearingY - s.height) - double(s.bearingY);
    for (const auto &tile : tiles) {
      slughorn::Atlas::Curves local;
      for (const auto &q : tile.curves)
        local.push_back({q.p0.x, q.p0.y, q.p1.x, q.p1.y, q.p2.x, q.p2.y});
      for (double dy : {.17, .53, .81})
        for (double dx : {.23, .61, .87}) {
          const double u = (tile.x + dx) / detail::pathTileGrid,
                       v = (tile.y + dy) / detail::pathTileGrid;
          const bool a = inside(s.curves, s.bearingX + u * sx,
                                s.bearingY + v * sy),
                     b = inside(local, u, v);
          if (a != b) {
            std::cerr << "Tile mismatch shape=" << key << " tile=" << tile.x
                      << ',' << tile.y << " point=" << u << ',' << v
                      << " curves=" << s.curves.size() << '\n';
            throw std::runtime_error("Clipped SVG tile winding mismatch");
          }
          ++tested;
        }
    }
  }
  std::cout << "SVG geometry and " << tested
            << " clipped tile reference probes passed\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
