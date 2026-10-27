#pragma once

#include "slugia/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace slugia {

inline constexpr float dynamicCurveTolerancePx = 1.0f / 256.0f;
// Adaptive builders reject non-finite input and throw std::length_error before
// exceeding this per-output contour budget. Non-positive finite extents remain empty.
inline constexpr std::size_t dynamicCurveSegmentLimit = 65536;

void appendLineQuadratic(std::vector<QuadraticBezier2D>& curves, Vec2 from, Vec2 to);
void appendCubicQuadratics(std::vector<QuadraticBezier2D>& curves,
                           Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3,
                           float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeRoundedRectQuadratics(
    Rect bounds, float radius, float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadratics(
    Rect bounds, float radius, float smoothingPercent,
    float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadratics(
    Rect bounds, CornerRadii radii, CornerSmoothing smoothing,
    float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeContinuousRoundedRectQuadraticsXY(
    Rect bounds, std::array<Vec2,4> radii, CornerSmoothing smoothing,
    float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeEllipseQuadratics(
    Vec2 center, Vec2 radius, float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeLineStrokeQuadratics(
    Vec2 from, Vec2 to, float width, LineCap cap = LineCap::Butt,
    float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeCubicStrokeQuadratics(
    Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float width,
    LineCap cap = LineCap::Butt, float tolerance = dynamicCurveTolerancePx);
std::vector<QuadraticBezier2D> makeArcStrokeQuadratics(
    Vec2 center, float radius, float startRadians, float sweepRadians, float width,
    LineCap cap = LineCap::Butt, float tolerance = dynamicCurveTolerancePx);
void reverseQuadraticContour(std::vector<QuadraticBezier2D>& curves);
Rect quadraticBounds(std::span<const QuadraticBezier2D> curves, float padding = 0.0f);

} // namespace slugia
