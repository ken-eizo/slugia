#pragma once
#include "viewport_clip.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <slughorn/slughorn.hpp>
#include <vector>

namespace slugia::detail {
struct PathTile {
  unsigned x, y;
  std::vector<QuadraticBezier2D> curves;
  bool attempted = false, resident = false;
  std::array<std::uint32_t, 4> shapeData{};
  std::array<float, 4> bandTransform{}, emRect{};
  Rect bounds{};
};
inline constexpr unsigned pathTileGrid = 16;
// Conservative occupancy. Optional clipped contours are experimental and need
// GPU image checks; clipCurves=false keeps the original curves for scissors.
inline std::vector<PathTile> pathTiles(const slughorn::Atlas::Shape &shape,
                                     bool clipCurves = true) {
  constexpr int n = pathTileGrid;
  std::array<bool, n * n> boundary{};
  const double x0 = shape.bearingX, y0 = shape.bearingY;
  const double w = float(shape.bearingX + shape.width) - x0;
  const double h = float(shape.bearingY - shape.height) - y0;
  const auto mark = [&](auto &&self, double ax, double ay, double bx, double by,
                        double cx, double cy, int depth) -> void {
    const double minX = std::min({ax, bx, cx}), maxX = std::max({ax, bx, cx});
    const double minY = std::min({ay, by, cy}), maxY = std::max({ay, by, cy});
    if (depth < 12 && (maxX - minX > 1.0 / n || maxY - minY > 1.0 / n)) {
      const double abx = (ax + bx) * .5, aby = (ay + by) * .5,
                   bcx = (bx + cx) * .5, bcy = (by + cy) * .5;
      const double mx = (abx + bcx) * .5, my = (aby + bcy) * .5;
      self(self, ax, ay, abx, aby, mx, my, depth + 1);
      self(self, mx, my, bcx, bcy, cx, cy, depth + 1);
      return;
    }
    const int l = std::clamp(int(std::floor(minX * n)) - 1, 0, n - 1),
              r = std::clamp(int(std::floor(maxX * n)) + 1, 0, n - 1);
    const int t = std::clamp(int(std::floor(minY * n)) - 1, 0, n - 1),
              b = std::clamp(int(std::floor(maxY * n)) + 1, 0, n - 1);
    for (int y = t; y <= b; ++y)
      for (int x = l; x <= r; ++x)
        boundary[y * n + x] = true;
  };
  for (const auto &c : shape.curves)
    mark(mark, (c.x1 - x0) / w, (c.y1 - y0) / h, (c.x2 - x0) / w,
         (c.y2 - y0) / h, (c.x3 - x0) / w, (c.y3 - y0) / h, 0);
  std::vector<PathTile> result;
  for (int ty = 0; ty < n; ++ty)
    for (int tx = 0; tx < n; ++tx) {
      if (boundary[ty * n + tx]) {
        result.push_back({unsigned(tx), unsigned(ty)});
        continue;
      }
      const double x = x0 + (tx + .5) / n * w, y = y0 + (ty + .5) / n * h;
      int winding = 0;
      for (const auto &c : shape.curves) {
        const double a = double(c.y1) - 2.0 * c.y2 + c.y3,
                     b = 2.0 * (double(c.y2) - c.y1);
        const double extremum = a != 0 ? -b / (2 * a) : -1;
        const std::array<double, 3> ends{
            0, extremum > 0 && extremum < 1 ? extremum : 1, 1};
        const auto value = [&](double t) {
          const double s = 1 - t;
          return s * s * c.y1 + 2 * s * t * c.y2 + t * t * c.y3;
        };
        for (int part = 0; part < 2; ++part) {
          double low = ends[part], high = ends[part + 1];
          const double y1 = value(low), y2 = value(high);
          if (!((y1 <= y && y < y2) || (y2 <= y && y < y1)))
            continue;
          for (int i = 0; i < 40; ++i) {
            double mid = (low + high) * .5;
            if ((value(mid) < y) == (y2 > y1))
              low = mid;
            else
              high = mid;
          }
          const double t = (low + high) * .5, s = 1 - t;
          if (s * s * c.x1 + 2 * s * t * c.x2 + t * t * c.x3 > x)
            winding += y2 > y1 ? 1 : -1;
        }
      }
      if (shape.evenOdd ? (std::abs(winding) & 1) != 0 : winding != 0)
        result.push_back({unsigned(tx), unsigned(ty)});
    }
  if (clipCurves && shape.curves.size() >= 128) {
    std::vector<DQuadratic> normalized;
    for (const auto &c : shape.curves)
      normalized.push_back({{(c.x1 - x0) / w, (c.y1 - y0) / h},
                            {(c.x2 - x0) / w, (c.y2 - y0) / h},
                            {(c.x3 - x0) / w, (c.y3 - y0) / h}});
    for (auto &tile : result) {
      // Artificial clipping edges must remain outside the tile's sampling
      // footprint. At the minimum 128 px extent this margin is two pixels.
      constexpr float margin = .25f / n;
      const Rect box{float(tile.x) / n - margin, float(tile.y) / n - margin,
                     1.0f / n + 2 * margin, 1.0f / n + 2 * margin};
      tile.curves = clipViewport(normalized, box);
    }
  }
  return result;
}
} // namespace slugia::detail
