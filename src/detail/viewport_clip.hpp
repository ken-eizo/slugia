#pragma once
#include "slugia/types.hpp"
#include <array>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>

namespace slugia::detail {
struct DPoint {
  double x = 0, y = 0;
};
struct DQuadratic {
  DPoint p0, p1, p2;
};
inline DPoint mix(DPoint a, DPoint b, double t) {
  return {std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t)};
}
inline std::array<DQuadratic, 2> split(DQuadratic q, double t) {
  const auto a = mix(q.p0, q.p1, t), b = mix(q.p1, q.p2, t), c = mix(a, b, t);
  return {{{q.p0, a, c}, {c, b, q.p2}}};
}
inline DQuadratic interval(DQuadratic q, double a, double b) {
  if (b < 1)
    q = split(q, b)[0];
  if (a > 0)
    q = split(q, a / b)[1];
  return q;
}
inline std::vector<DQuadratic> clipHalfPlane(std::span<const DQuadratic> source,
                                             int axis, double boundary,
                                             bool minimum) {
  std::vector<DQuadratic> kept;
  const auto coordinate = [&](DPoint p) { return axis == 0 ? p.x : p.y; };
  for (const auto &q : source) {
    const double p0 = coordinate(q.p0) - boundary,
                 p1 = coordinate(q.p1) - boundary,
                 p2 = coordinate(q.p2) - boundary;
    const double a = p0 - 2 * p1 + p2, b = 2 * (p1 - p0), c = p0;
    std::array<double, 4> stops{0, 1, 1, 1};
    std::size_t count = 2;
    const auto root = [&](double t) {
      if (t > 0 && t < 1 && std::isfinite(t))
        stops[count++] = t;
    };
    if (std::abs(a) <= std::numeric_limits<double>::epsilon() *
                           std::max({std::abs(b), std::abs(c), 1e-300})) {
      if (b != 0)
        root(-c / b);
    } else {
      const double d = std::fma(b, b, -4 * a * c);
      if (d > 0) {
        const double r = -.5 * (b + std::copysign(std::sqrt(d), b));
        root(r / a);
        if (r != 0)
          root(c / r);
      }
    }
    std::sort(stops.begin(), stops.begin() + count);
    for (std::size_t i = 0; i + 1 < count; ++i) {
      if (stops[i + 1] <= stops[i])
        continue;
      const double t = std::midpoint(stops[i], stops[i + 1]);
      const double value = std::fma(std::fma(a, t, b), t, c);
      if (minimum ? value >= 0 : value <= 0) {
        auto piece = interval(q, stops[i], stops[i + 1]);
        // Intersection endpoints belong exactly to the boundary, preventing
        // tiny gaps when very distant controls are clipped into the viewport.
        if (stops[i] > 0)
          (axis == 0 ? piece.p0.x : piece.p0.y) = boundary;
        if (stops[i + 1] < 1)
          (axis == 0 ? piece.p2.x : piece.p2.y) = boundary;
        kept.push_back(piece);
      }
    }
  }
  if (kept.empty())
    return {};
  std::vector<DQuadratic> closed;
  closed.reserve(kept.size() * 2);
  for (std::size_t i = 0; i < kept.size(); ++i) {
    closed.push_back(kept[i]);
    const auto end = kept[i].p2, start = kept[(i + 1) % kept.size()].p0;
    if (end.x != start.x || end.y != start.y)
      closed.push_back({end, mix(end, start, .5), start});
  }
  return closed;
}
inline std::vector<QuadraticBezier2D>
clipViewport(std::span<const DQuadratic> source, Rect clip) {
  std::vector<QuadraticBezier2D> out;
  // Process contours independently so holes and winding survive clipping.
  for (std::size_t start = 0; start < source.size();) {
    std::size_t end = start + 1;
    while (end < source.size() && source[end - 1].p2.x == source[end].p0.x &&
           source[end - 1].p2.y == source[end].p0.y)
      ++end;
    std::vector<DQuadratic> contour(source.begin() + start,
                                    source.begin() + end);
    if (contour.front().p0.x != contour.back().p2.x ||
        contour.front().p0.y != contour.back().p2.y) {
      const auto a = contour.back().p2, b = contour.front().p0;
      contour.push_back({a, mix(a, b, .5), b});
    }
    contour = clipHalfPlane(contour, 0, clip.x, true);
    contour = clipHalfPlane(contour, 0, double(clip.x) + clip.width, false);
    contour = clipHalfPlane(contour, 1, clip.y, true);
    contour = clipHalfPlane(contour, 1, double(clip.y) + clip.height, false);
    for (const auto &q : contour) {
      const auto f = [](DPoint p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            std::abs(p.x) > 1e30 || std::abs(p.y) > 1e30)
          throw std::overflow_error(
              "Zoomed geometry exceeds representable precision");
        return Vec2{float(p.x), float(p.y)};
      };
      out.push_back({f(q.p0), f(q.p1), f(q.p2)});
      if (out.size() > 65536)
        throw std::length_error("Viewport curve budget exceeded");
    }
    start = end;
  }
  return out;
}
} // namespace slugia::detail
