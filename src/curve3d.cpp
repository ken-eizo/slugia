#include "slugia/vector3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace slugia {
namespace {
using H = std::array<double, 4>;
bool finite(Vec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
H homogeneous(const CurvePoint3D &p) {
  return {double(p.position.x) * p.weight, double(p.position.y) * p.weight,
          double(p.position.z) * p.weight, p.weight};
}
CurvePoint3D euclidean(const H &p) {
  CurvePoint3D q;
  q.position = {float(p[0] / p[3]), float(p[1] / p[3]), float(p[2] / p[3])};
  q.weight = float(p[3]);
  return q;
}
} // namespace
void prepareCurve3D(Curve3D &curve) {
  if (curve.points.size() > 65536)
    throw std::length_error("3D curve control point budget exceeded");
  for (const auto &p : curve.points)
    if (!finite(p.position) || !finite(p.incoming) || !finite(p.outgoing) ||
        !std::isfinite(p.weight) || p.weight <= 0)
      throw std::invalid_argument(
          "3D curve controls need finite positions and positive weights");
  std::vector<CurveBezier3D> spans;
  if (curve.points.size() < 2) {
    curve.segments.clear();
    return;
  }
  if (curve.basis != CurveBasis3D::Nurbs) {
    const auto count =
        curve.cyclic ? curve.points.size() : curve.points.size() - 1;
    for (std::size_t i = 0; i < count; ++i) {
      const auto &a = curve.points[i];
      const auto &b = curve.points[(i + 1) % curve.points.size()];
      CurveBezier3D span;
      span.parameterStart = float(i);
      span.parameterEnd = float(i + 1);
      if (curve.basis == CurveBasis3D::Polyline)
        span.control = {a, b};
      else {
        auto c1 = a, c2 = b;
        c1.position = a.outgoing;
        c2.position = b.incoming;
        span.control = {a, c1, c2, b};
      }
      spans.push_back(std::move(span));
    }
    curve.segments = std::move(spans);
    return;
  }
  const auto p = std::size_t(curve.degree);
  if (p == 0 || p > 15 || p >= curve.points.size())
    throw std::invalid_argument(
        "NURBS degree must be 1..15 and below its control count");
  std::vector<H> controls;
  for (const auto &q : curve.points)
    controls.push_back(homogeneous(q));
  if (curve.cyclic)
    for (std::size_t i = 0; i < p; ++i)
      controls.push_back(controls[i]);
  std::vector<double> knots(curve.knots.begin(), curve.knots.end());
  if (knots.empty()) {
    const auto count = controls.size();
    if (curve.bezierKnots && !curve.cyclic) {
      if ((count - 1) % p != 0)
        throw std::invalid_argument(
            "Bezier NURBS knot mode requires 1 + N * degree controls");
      const auto intervals = (count - 1) / p;
      for (std::size_t i = 0; i <= p; ++i)
        knots.push_back(0);
      for (std::size_t i = 1; i < intervals; ++i)
        for (std::size_t j = 0; j < p; ++j)
          knots.push_back(double(i));
      for (std::size_t i = 0; i <= p; ++i)
        knots.push_back(double(intervals));
    } else
      for (std::size_t i = 0; i < count + p + 1; ++i) {
        if (curve.endpoint && !curve.cyclic)
          knots.push_back(i <= p       ? 0.0
                          : i >= count ? double(count - p)
                                       : double(i - p));
        else
          knots.push_back(double(i));
      }
  }
  if (knots.size() != controls.size() + p + 1 ||
      !std::is_sorted(knots.begin(), knots.end()))
    throw std::invalid_argument("Invalid NURBS knot vector");
  for (double u : knots)
    if (!std::isfinite(u))
      throw std::invalid_argument("NURBS knots must be finite");
  const double start = knots[p], end = knots[controls.size()];
  if (!(end > start))
    throw std::invalid_argument("Empty NURBS parameter interval");
  std::vector<double> boundaries;
  for (double u : knots)
    if (u >= start && u <= end &&
        (boundaries.empty() || boundaries.back() != u))
      boundaries.push_back(u);
  std::size_t work = 0;
  // Insert knots in homogeneous coordinates until every span is rational
  // Bezier. This preserves weights and geometry, including non-uniform/cyclic
  // splines.
  for (const double u : boundaries) {
    auto multiplicity = std::size_t(std::count(knots.begin(), knots.end(), u));
    while (multiplicity < p) {
      const auto n = controls.size() - 1;
      auto k = std::size_t(std::upper_bound(knots.begin(), knots.end(), u) -
                           knots.begin() - 1);
      k = std::min(k, n + 1);
      std::vector<H> next(controls.size() + 1);
      if ((work += next.size()) > 16000000)
        throw std::length_error("NURBS preparation work budget exceeded");
      for (std::size_t i = 0; i <= k - p; ++i)
        next[i] = controls[i];
      for (std::size_t i = k - multiplicity; i <= n; ++i)
        next[i + 1] = controls[i];
      for (std::size_t i = k - p + 1; i <= k - multiplicity; ++i) {
        const double denominator = knots[i + p] - knots[i];
        const double alpha = denominator > 0 ? (u - knots[i]) / denominator : 0;
        for (std::size_t j = 0; j < 4; ++j)
          next[i][j] =
              (1 - alpha) * controls[i - 1][j] + alpha * controls[i][j];
      }
      controls = std::move(next);
      knots.insert(knots.begin() + std::ptrdiff_t(k + 1), u);
      ++multiplicity;
    }
  }
  for (std::size_t k = p; k < controls.size(); ++k) {
    if (knots[k] < start || knots[k + 1] > end || knots[k + 1] <= knots[k])
      continue;
    CurveBezier3D span;
    span.parameterStart = float(knots[k]);
    span.parameterEnd = float(knots[k + 1]);
    for (std::size_t i = k - p; i <= k; ++i)
      span.control.push_back(euclidean(controls[i]));
    spans.push_back(std::move(span));
  }
  curve.segments = std::move(spans);
}
} // namespace slugia
