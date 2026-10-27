#include "slugia/vector3d.hpp"
#include "slugia/vector_path.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace slugia {
namespace {

bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
void requireDimension(float value) {
  if (!std::isfinite(value) || value <= 0.0f)
    throw std::invalid_argument("Mesh dimensions must be finite and positive");
}
void requireVertexBudget(std::uint64_t count) {
  if (count > generatedMesh3DVertexLimit)
    throw std::length_error("Generated mesh exceeds its vertex budget");
}
void requireView(Vec2 origin, Vec3 rotation, Camera3D camera) {
  if (!finite(origin) || !finite(rotation) || !finite(camera.position) ||
      !std::isfinite(camera.perspective))
    throw std::invalid_argument("3D view values must be finite");
  Mesh3DView view;
  view.origin = origin;
  view.transform.rotation = rotation;
  view.camera = camera;
  (void)projectPoint3D({}, view);
}

Vec3 rotate(Vec3 p, Vec3 r) {
  float c = std::cos(r.x), s = std::sin(r.x);
  p = {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
  c = std::cos(r.y);
  s = std::sin(r.y);
  p = {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
  c = std::cos(r.z);
  s = std::sin(r.z);
  return {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
}

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 v) { return std::sqrt(std::max(dot(v, v), 0.0f)); }
Vec3 scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 normalized3(Vec3 v) {
  const float l = length(v);
  return l > 1.0e-10f ? scale(v, 1.0f / l) : Vec3{0.0f, 0.0f, 1.0f};
}

Vec3 inverseRotate(Vec3 p, Vec3 r) {
  float c = std::cos(r.z), s = std::sin(r.z);
  p = {p.x * c + p.y * s, -p.x * s + p.y * c, p.z};
  c = std::cos(r.y);
  s = std::sin(r.y);
  p = {p.x * c - p.z * s, p.y, p.x * s + p.z * c};
  c = std::cos(r.x);
  s = std::sin(r.x);
  return {p.x, p.y * c + p.z * s, -p.y * s + p.z * c};
}

Vec2 projectPrimitivePoint(Vec3 local, Vec2 origin, Vec3 rotation,
                           Camera3D camera) {
  const Vec3 w = inverseRotate(sub(rotate(local, rotation), camera.position),
                               camera.rotation);
  const float focal =
      camera.focalLength > 0
          ? camera.focalLength
          : (std::abs(camera.position.z) > 1e-5f ? std::abs(camera.position.z)
                                                 : 520.0f);
  const float denominator = 1.0f + camera.perspective * (w.z / focal - 1.0f);
  return {origin.x + w.x / denominator, origin.y + w.y / denominator};
}

Vec2 projectPrimitiveTangent(Vec3 local, Vec3 derivative, Vec3 rotation,
                             Camera3D camera) {
  const Vec3 w = inverseRotate(sub(rotate(local, rotation), camera.position),
                               camera.rotation);
  const Vec3 dw = inverseRotate(rotate(derivative, rotation), camera.rotation);
  const float focal =
      camera.focalLength > 0
          ? camera.focalLength
          : (std::abs(camera.position.z) > 1e-5f ? std::abs(camera.position.z)
                                                 : 520.0f);
  const float denominator = 1.0f + camera.perspective * (w.z / focal - 1.0f);
  const float s = 1.0f / denominator,
              ds = -camera.perspective * dw.z / focal * s * s;
  return {dw.x * s + w.x * ds, dw.y * s + w.y * ds};
}

Vec3 effectiveEye(Camera3D camera) {
  const float focal =
      camera.focalLength > 0
          ? camera.focalLength
          : (std::abs(camera.position.z) > 1e-5f ? std::abs(camera.position.z)
                                                 : 520.0f);
  return add(camera.position,
             rotate({0, 0, focal * (1.0f - 1.0f / camera.perspective)},
                    camera.rotation));
}
bool needsPrimitiveClip(float boundingRadius, Camera3D camera) {
  const Vec3 center =
      inverseRotate(scale(camera.position, -1), camera.rotation);
  const float focal =
      camera.focalLength > 0
          ? camera.focalLength
          : (std::abs(camera.position.z) > 1e-5f ? std::abs(camera.position.z)
                                                 : 520.0f);
  return center.z - boundingRadius <= camera.nearPlane ||
         center.z + boundingRadius >= camera.farPlane ||
         1.0f + camera.perspective *
                     ((center.z - boundingRadius) / focal - 1.0f) <=
             camera.nearPlane / focal;
}
void drawClippedPrimitive(DrawList &draw, Mesh3D mesh, Vec2 origin,
                          Vec3 rotation, Camera3D camera,
                          const VectorSolid3DStyle &style, int kind) {
  mesh.trianglePaints.assign(mesh.triangles.size(), style.surface);
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i)
    if ((kind == 1 && i % 4 >= 2) || (kind == 2 && i % 2 == 1))
      mesh.trianglePaints[i] = style.cap;
  Mesh3DView view;
  view.origin = origin;
  view.transform.rotation = rotation;
  view.camera = camera;
  Mesh3DStyle material;
  material.fill = style.surface;
  material.edge = style.outline;
  material.drawEdges = style.outline.width > 0;
  drawMesh3D(draw, mesh, view, material);
}

float distanceSquared(Vec2 a, Vec2 b) {
  const Vec2 d = a - b;
  return d.x * d.x + d.y * d.y;
}

template <class F>
void appendParametricQuadratic(std::vector<QuadraticBezier2D> &out, const F &f,
                               float t0, float t1, float toleranceSquared,
                               int depth) {
  const float tm = 0.5f * (t0 + t1);
  const Vec2 p0 = f(t0), pm = f(tm), p2 = f(t1);
  const Vec2 p1 = pm * 2.0f - (p0 + p2) * 0.5f;
  if (!finite(p0) || !finite(p1) || !finite(p2))
    throw std::invalid_argument(
        "3D projection exceeds the finite coordinate range");
  const auto eval = [&](float u) {
    const float v = 1.0f - u;
    return p0 * (v * v) + p1 * (2.0f * u * v) + p2 * (u * u);
  };
  constexpr float probes[] = {0.125f, 0.25f, 0.375f, 0.625f, 0.75f, 0.875f};
  float error = 0.0f;
  for (float u : probes)
    error = std::max(error, distanceSquared(f(t0 + (t1 - t0) * u), eval(u)));
  if (error <= toleranceSquared || depth >= 18) {
    if (out.size() >= dynamicCurveSegmentLimit)
      throw std::length_error(
          "Projected vector geometry exceeds its segment budget");
    out.push_back({p0, p1, p2});
    return;
  }
  appendParametricQuadratic(out, f, t0, tm, toleranceSquared, depth + 1);
  appendParametricQuadratic(out, f, tm, t1, toleranceSquared, depth + 1);
}

template <class F>
void appendParametricArc(std::vector<QuadraticBezier2D> &out, const F &f,
                         float start, float sweep,
                         float tolerance = dynamicCurveTolerancePx) {
  constexpr float pi = 3.14159265358979323846f;
  const int pieces =
      std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (0.5f * pi))));
  const float step = sweep / static_cast<float>(pieces);
  const float toleranceSquared = tolerance * tolerance;
  for (int i = 0; i < pieces; ++i)
    appendParametricQuadratic(out, f, start + step * i, start + step * (i + 1),
                              toleranceSquared, 0);
}

void appendCurves(std::vector<QuadraticBezier2D> &dst,
                  std::vector<QuadraticBezier2D> src) {
  dst.insert(dst.end(), std::make_move_iterator(src.begin()),
             std::make_move_iterator(src.end()));
}

void emitDynamicPath(DrawList &draw, std::vector<QuadraticBezier2D> curves,
                     const Paint &paint,
                     FillRule fillRule = FillRule::NonZero) {
  if (curves.empty())
    return;
  const Rect bounds = quadraticBounds(curves);
  if (bounds.width <= 0.0f || bounds.height <= 0.0f)
    return;
  draw.dynamicVectorPath(std::move(curves), bounds, paint, fillRule);
}

template <class Position, class Tangent>
std::vector<QuadraticBezier2D>
makeParametricStroke(const Position &position, const Tangent &tangent,
                     float start, float sweep, float width, bool closed) {
  std::vector<QuadraticBezier2D> out;
  if (width <= 0.0f)
    return out;
  const float half = 0.5f * width;
  const auto offset = [&](float sign) {
    return [&, sign](float t) {
      const Vec2 p = position(t);
      const Vec2 d = tangent(t);
      const float l = std::hypot(d.x, d.y);
      if (l <= 1.0e-8f)
        return p;
      const Vec2 n{-d.y / l, d.x / l};
      return p + n * (sign * half);
    };
  };
  auto outer = offset(1.0f);
  auto inner = offset(-1.0f);
  appendParametricArc(out, outer, start, sweep);
  if (closed) {
    std::vector<QuadraticBezier2D> hole;
    appendParametricArc(hole, inner, start + sweep, -sweep);
    out.insert(out.end(), hole.begin(), hole.end());
  } else {
    const Vec2 oe = outer(start + sweep), ie = inner(start + sweep);
    appendLineQuadratic(out, oe, ie);
    appendParametricArc(out, inner, start + sweep, -sweep);
    appendLineQuadratic(out, inner(start), outer(start));
  }
  return out;
}

float positiveSweep(float start, float end) {
  constexpr float tau = 6.28318530717958647692f;
  float s = std::fmod(end - start, tau);
  if (s < 0.0f)
    s += tau;
  return s;
}

template <class F>
float chooseOuterSweep(const F &circle, float start, float end, Vec2 center,
                       Vec2 awayFrom) {
  constexpr float tau = 6.28318530717958647692f;
  const float s1 = positiveSweep(start, end);
  const float s2 = s1 - tau;
  const Vec2 dir = center - awayFrom;
  const Vec2 m1 = circle(start + 0.5f * s1), m2 = circle(start + 0.5f * s2);
  const float d1 = (m1.x - center.x) * dir.x + (m1.y - center.y) * dir.y;
  const float d2 = (m2.x - center.x) * dir.x + (m2.y - center.y) * dir.y;
  return d1 >= d2 ? s1 : s2;
}

bool solveAngularTangents(float A, float B, float C, float &a, float &b) {
  const float l = std::hypot(A, B);
  if (l <= 1.0e-7f || std::abs(C) > l)
    return false;
  const float q = C / l;
  const float phi = std::atan2(B, A);
  const float d = std::acos(std::clamp(q, -1.0f, 1.0f));
  a = phi - d;
  b = phi + d;
  return std::abs(a - b) > 1.0e-7f;
}

bool capFacesCamera(float y, float normalY, Vec3 rotation, Camera3D camera) {
  const Vec3 n{0.0f, normalY, 0.0f};
  if (camera.perspective > 1.0e-5f) {
    const Vec3 cam = inverseRotate(effectiveEye(camera), rotation);
    return dot(n, sub(cam, {0.0f, y, 0.0f})) > 0.0f;
  }
  const Vec3 toCamera =
      inverseRotate(rotate({0.0f, 0.0f, -1.0f}, camera.rotation), rotation);
  return dot(n, toCamera) > 0.0f;
}

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
  if (a > b)
    std::swap(a, b);
  return (std::uint64_t(a) << 32U) | std::uint64_t(b);
}
int edgeSlot(const MeshTriangle3D &t, std::uint32_t a, std::uint32_t b) {
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> e{
      {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}}};
  for (int i = 0; i < 3; ++i) {
    const auto [x, y] = e[std::size_t(i)];
    if ((x == a && y == b) || (x == b && y == a))
      return i;
  }
  return -1;
}

void orientGeneratedOutward(Mesh3D &mesh) {
  for (auto &t : mesh.triangles) {
    if (t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
        t.c >= mesh.vertices.size())
      continue;
    const Vec3 a = mesh.vertices[t.a], b = mesh.vertices[t.b],
               c = mesh.vertices[t.c];
    const Vec3 n = cross(sub(b, a), sub(c, a));
    const Vec3 centroid{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f,
                        (a.z + b.z + c.z) / 3.0f};
    if (dot(n, centroid) < 0.0f)
      std::swap(t.b, t.c);
  }
}

std::vector<MeshEdge3D> buildMeshEdges(const Mesh3D &mesh) {
  struct Pending {
    std::int32_t first = -1, second = -1;
  };
  std::unordered_map<std::uint64_t, Pending> map;
  map.reserve(mesh.triangles.size() * 3);
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto t = mesh.triangles[i];
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> edges{
        {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}}};
    for (auto [a, b] : edges) {
      auto &q = map[edgeKey(a, b)];
      if (q.first < 0)
        q.first = static_cast<std::int32_t>(i);
      else if (q.second < 0)
        q.second = static_cast<std::int32_t>(i);
      else
        throw std::invalid_argument("Mesh contains a non-manifold edge");
    }
  }
  std::vector<MeshEdge3D> result;
  result.reserve(map.size());
  for (const auto &[key, q] : map)
    result.push_back({std::uint32_t(key >> 32U),
                      std::uint32_t(key & 0xffffffffU), q.first, q.second});
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.a < b.a || (a.a == b.a && a.b < b.b);
  });
  return result;
}

} // namespace

void prepareMesh3D(Mesh3D &mesh) {
  // Topology preparation is intentionally geometry-neutral. Imported winding is
  // preserved; authoring tools remain the authority for front/back orientation.
  if (mesh.triangles.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
      mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("Mesh exceeds its topology index range");
  for (const Vec3 v : mesh.vertices)
    if (!finite(v))
      throw std::invalid_argument("Mesh vertices must be finite");
  for (const auto t : mesh.triangles)
    if (t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
        t.c >= mesh.vertices.size())
      throw std::invalid_argument("Mesh triangle references an invalid vertex");
    else if (t.a == t.b || t.b == t.c || t.c == t.a)
      throw std::invalid_argument("Mesh triangle repeats a vertex index");
  auto edges = buildMeshEdges(mesh);
  for (auto pair : mesh.authoredSharpEdges) {
    if (pair[0] >= mesh.vertices.size() || pair[1] >= mesh.vertices.size())
      throw std::invalid_argument("Invalid authored sharp edge");
    if (pair[0] > pair[1])
      std::swap(pair[0], pair[1]);
    const auto found = std::lower_bound(
        edges.begin(), edges.end(), pair,
        [](const auto &edge, const auto &key) {
          return edge.a < key[0] || (edge.a == key[0] && edge.b < key[1]);
        });
    if (found == edges.end() || found->a != pair[0] || found->b != pair[1])
      throw std::invalid_argument("Authored sharp edge is not in the mesh");
    found->sharp = true;
  }
  std::vector<std::array<std::int32_t, 3>> neighbors(mesh.triangles.size(),
                                                     {-1, -1, -1});
  for (const auto &e : edges) {
    if (e.firstTriangle >= 0 && e.secondTriangle >= 0) {
      const int s0 =
          edgeSlot(mesh.triangles[std::size_t(e.firstTriangle)], e.a, e.b);
      const int s1 =
          edgeSlot(mesh.triangles[std::size_t(e.secondTriangle)], e.a, e.b);
      if (s0 >= 0)
        neighbors[std::size_t(e.firstTriangle)][std::size_t(s0)] =
            e.secondTriangle;
      if (s1 >= 0)
        neighbors[std::size_t(e.secondTriangle)][std::size_t(s1)] =
            e.firstTriangle;
    }
  }
  mesh.edges = std::move(edges);
  mesh.triangleNeighbors = std::move(neighbors);
}

void drawSphere3D(DrawList &draw, Vec2 origin, float radius, Vec3 rotation,
                  Camera3D camera, const VectorSolid3DStyle &style) {
  requireView(origin, rotation, camera);
  if (!std::isfinite(radius) || !std::isfinite(style.outline.width))
    throw std::invalid_argument("3D primitive dimensions must be finite");
  if (radius <= 0.0f)
    return;
  if (needsPrimitiveClip(radius, camera)) {
    drawClippedPrimitive(draw, makeUvSphereMesh3D(radius, 96, 48), origin,
                         rotation, camera, style, 0);
    return;
  }
  constexpr float tau = 6.28318530717958647692f;

  Vec3 normal{};
  Vec3 circleCenter{};
  float circleRadius = radius;
  if (camera.perspective > 1.0e-5f) {
    const Vec3 cam = inverseRotate(effectiveEye(camera), rotation);
    const float d2 = dot(cam, cam);
    if (d2 <= radius * radius + 1.0e-8f)
      return;
    normal = normalized3(cam);
    const float factor = (radius * radius) / d2;
    circleCenter = scale(cam, factor);
    circleRadius = radius * std::sqrt(std::max(1.0f - factor, 0.0f));
  } else {
    normal = normalized3(
        inverseRotate(rotate({0.0f, 0.0f, -1.0f}, camera.rotation), rotation));
    circleCenter = {0.0f, 0.0f, 0.0f};
  }

  const Vec3 reference = std::abs(normal.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f}
                                                   : Vec3{1.0f, 0.0f, 0.0f};
  const Vec3 basisU = normalized3(cross(normal, reference));
  const Vec3 basisV = normalized3(cross(normal, basisU));
  const auto silhouette = [&](float a) {
    const Vec3 radial = add(scale(basisU, circleRadius * std::cos(a)),
                            scale(basisV, circleRadius * std::sin(a)));
    return projectPrimitivePoint(add(circleCenter, radial), origin, rotation,
                                 camera);
  };
  const auto silhouetteTangent = [&](float a) {
    const Vec3 radial = add(scale(basisU, circleRadius * std::cos(a)),
                            scale(basisV, circleRadius * std::sin(a)));
    const Vec3 derivative = add(scale(basisU, -circleRadius * std::sin(a)),
                                scale(basisV, circleRadius * std::cos(a)));
    return projectPrimitiveTangent(add(circleCenter, radial), derivative,
                                   rotation, camera);
  };

  std::vector<QuadraticBezier2D> fill;
  appendParametricArc(fill, silhouette, 0.0f, tau);
  emitDynamicPath(draw, std::move(fill), style.surface);

  if (style.outline.width > 0.0f && style.outline.paint.opacity > 0.0f) {
    auto stroke = makeParametricStroke(silhouette, silhouetteTangent, 0.0f, tau,
                                       style.outline.width, true);
    emitDynamicPath(draw, std::move(stroke), style.outline.paint);
  }
}

void drawCylinder3D(DrawList &draw, Vec2 origin, float radius, float halfHeight,
                    Vec3 rotation, Camera3D camera,
                    const VectorSolid3DStyle &style) {
  requireView(origin, rotation, camera);
  if (!std::isfinite(radius) || !std::isfinite(halfHeight) ||
      !std::isfinite(style.outline.width))
    throw std::invalid_argument("3D primitive dimensions must be finite");
  if (radius <= 0.0f || halfHeight <= 0.0f)
    return;
  if (needsPrimitiveClip(std::hypot(radius, halfHeight), camera)) {
    drawClippedPrimitive(draw, makeCylinderMesh3D(radius, halfHeight, 128),
                         origin, rotation, camera, style, 1);
    return;
  }
  constexpr float tau = 6.28318530717958647692f;
  const float y0 = -halfHeight, y1 = halfHeight;
  const auto ring0 = [&](float a) {
    return projectPrimitivePoint(
        {radius * std::cos(a), y0, radius * std::sin(a)}, origin, rotation,
        camera);
  };
  const auto ring1 = [&](float a) {
    return projectPrimitivePoint(
        {radius * std::cos(a), y1, radius * std::sin(a)}, origin, rotation,
        camera);
  };
  const auto ring0Tangent = [&](float a) {
    return projectPrimitiveTangent(
        {radius * std::cos(a), y0, radius * std::sin(a)},
        {-radius * std::sin(a), 0.0f, radius * std::cos(a)}, rotation, camera);
  };
  const auto ring1Tangent = [&](float a) {
    return projectPrimitiveTangent(
        {radius * std::cos(a), y1, radius * std::sin(a)},
        {-radius * std::sin(a), 0.0f, radius * std::cos(a)}, rotation, camera);
  };
  const Vec2 center0 =
      projectPrimitivePoint({0.0f, y0, 0.0f}, origin, rotation, camera);
  const Vec2 center1 =
      projectPrimitivePoint({0.0f, y1, 0.0f}, origin, rotation, camera);

  Vec3 eyeDirection{};
  float A = 0.0f, B = 0.0f, C = 0.0f;
  if (camera.perspective > 1.0e-5f) {
    const Vec3 cam = inverseRotate(effectiveEye(camera), rotation);
    A = cam.x;
    B = cam.z;
    C = radius;
  } else {
    eyeDirection =
        inverseRotate(rotate({0.0f, 0.0f, -1.0f}, camera.rotation), rotation);
    A = eyeDirection.x;
    B = eyeDirection.z;
    C = 0.0f;
  }
  float a = 0.0f, b = 0.0f;
  const bool haveTangents = solveAngularTangents(A, B, C, a, b);
  const bool cap0Front = capFacesCamera(y0, -1.0f, rotation, camera);
  const bool cap1Front = capFacesCamera(y1, 1.0f, rotation, camera);

  if (!haveTangents) {
    const bool useTop = cap1Front || !cap0Front;
    const auto visibleRing = [&](float t) {
      return useTop ? ring1(t) : ring0(t);
    };
    const auto visibleTangent = [&](float t) {
      return useTop ? ring1Tangent(t) : ring0Tangent(t);
    };
    std::vector<QuadraticBezier2D> disk;
    appendParametricArc(disk, visibleRing, 0.0f, tau);
    emitDynamicPath(draw, std::move(disk),
                    (useTop ? cap1Front : cap0Front) ? style.cap
                                                     : style.surface);
    if (style.outline.width > 0.0f) {
      auto stroke = makeParametricStroke(visibleRing, visibleTangent, 0.0f, tau,
                                         style.outline.width, true);
      emitDynamicPath(draw, std::move(stroke), style.outline.paint);
    }
    return;
  }

  const float sweep0 = chooseOuterSweep(ring0, a, b, center0, center1);
  const float sweep1 = chooseOuterSweep(ring1, b, a, center1, center0);
  std::vector<QuadraticBezier2D> silhouette;
  appendParametricArc(silhouette, ring0, a, sweep0);
  appendLineQuadratic(silhouette, ring0(b), ring1(b));
  appendParametricArc(silhouette, ring1, b, sweep1);
  appendLineQuadratic(silhouette, ring1(a), ring0(a));
  emitDynamicPath(draw, std::move(silhouette), style.surface);

  const auto drawCap = [&](const auto &ring, bool front) {
    if (!front)
      return;
    std::vector<QuadraticBezier2D> cap;
    appendParametricArc(cap, ring, 0.0f, tau);
    emitDynamicPath(draw, std::move(cap), style.cap);
  };
  drawCap(ring0, cap0Front);
  drawCap(ring1, cap1Front);

  if (style.outline.width <= 0.0f || style.outline.paint.opacity <= 0.0f)
    return;
  // Keep every visible outline component as an independent positive-winding
  // Slug contour. This prevents coincident joins from canceling when cap/side
  // parameterizations happen to have opposite screen-space orientation.
  emitDynamicPath(draw,
                  makeLineStrokeQuadratics(ring0(a), ring1(a),
                                           style.outline.width, LineCap::Butt),
                  style.outline.paint);
  emitDynamicPath(draw,
                  makeLineStrokeQuadratics(ring0(b), ring1(b),
                                           style.outline.width, LineCap::Butt),
                  style.outline.paint);
  emitDynamicPath(draw,
                  cap0Front
                      ? makeParametricStroke(ring0, ring0Tangent, 0.0f, tau,
                                             style.outline.width, true)
                      : makeParametricStroke(ring0, ring0Tangent, a, sweep0,
                                             style.outline.width, false),
                  style.outline.paint);
  emitDynamicPath(draw,
                  cap1Front
                      ? makeParametricStroke(ring1, ring1Tangent, 0.0f, tau,
                                             style.outline.width, true)
                      : makeParametricStroke(ring1, ring1Tangent, b, sweep1,
                                             style.outline.width, false),
                  style.outline.paint);
}

void drawCone3D(DrawList &draw, Vec2 origin, float radius, float halfHeight,
                Vec3 rotation, Camera3D camera,
                const VectorSolid3DStyle &style) {
  requireView(origin, rotation, camera);
  if (!std::isfinite(radius) || !std::isfinite(halfHeight) ||
      !std::isfinite(style.outline.width))
    throw std::invalid_argument("3D primitive dimensions must be finite");
  if (radius <= 0.0f || halfHeight <= 0.0f)
    return;
  if (needsPrimitiveClip(std::hypot(radius, halfHeight), camera)) {
    drawClippedPrimitive(draw, makeConeMesh3D(radius, halfHeight, 128), origin,
                         rotation, camera, style, 2);
    return;
  }
  constexpr float tau = 6.28318530717958647692f;
  const float baseY = halfHeight;
  const Vec2 apex = projectPrimitivePoint({0.0f, -halfHeight, 0.0f}, origin,
                                          rotation, camera);
  const Vec2 baseCenter =
      projectPrimitivePoint({0.0f, baseY, 0.0f}, origin, rotation, camera);
  const auto ring = [&](float a) {
    return projectPrimitivePoint(
        {radius * std::cos(a), baseY, radius * std::sin(a)}, origin, rotation,
        camera);
  };
  const auto ringTangent = [&](float a) {
    return projectPrimitiveTangent(
        {radius * std::cos(a), baseY, radius * std::sin(a)},
        {-radius * std::sin(a), 0.0f, radius * std::cos(a)}, rotation, camera);
  };

  const float k = radius / (2.0f * halfHeight);
  float A = 0.0f, B = 0.0f, C = 0.0f;
  if (camera.perspective > 1.0e-5f) {
    const Vec3 cam = inverseRotate(effectiveEye(camera), rotation);
    A = cam.x;
    B = cam.z;
    C = k * (cam.y + halfHeight);
  } else {
    const Vec3 toCamera =
        inverseRotate(rotate({0.0f, 0.0f, -1.0f}, camera.rotation), rotation);
    A = toCamera.x;
    B = toCamera.z;
    C = k * toCamera.y;
  }
  float a = 0.0f, b = 0.0f;
  const bool haveTangents = solveAngularTangents(A, B, C, a, b);
  const bool capFront = capFacesCamera(baseY, 1.0f, rotation, camera);

  if (!haveTangents) {
    std::vector<QuadraticBezier2D> disk;
    appendParametricArc(disk, ring, 0.0f, tau);
    emitDynamicPath(draw, std::move(disk),
                    capFront ? style.cap : style.surface);
    if (style.outline.width > 0.0f) {
      auto stroke = makeParametricStroke(ring, ringTangent, 0.0f, tau,
                                         style.outline.width, true);
      emitDynamicPath(draw, std::move(stroke), style.outline.paint);
    }
    return;
  }

  const float sweep = chooseOuterSweep(ring, a, b, baseCenter, apex);
  std::vector<QuadraticBezier2D> silhouette;
  appendParametricArc(silhouette, ring, a, sweep);
  appendLineQuadratic(silhouette, ring(b), apex);
  appendLineQuadratic(silhouette, apex, ring(a));
  emitDynamicPath(draw, std::move(silhouette), style.surface);

  if (capFront) {
    std::vector<QuadraticBezier2D> cap;
    appendParametricArc(cap, ring, 0.0f, tau);
    emitDynamicPath(draw, std::move(cap), style.cap);
  }

  if (style.outline.width <= 0.0f || style.outline.paint.opacity <= 0.0f)
    return;
  emitDynamicPath(draw,
                  makeLineStrokeQuadratics(apex, ring(a), style.outline.width,
                                           LineCap::Butt),
                  style.outline.paint);
  emitDynamicPath(draw,
                  makeLineStrokeQuadratics(apex, ring(b), style.outline.width,
                                           LineCap::Butt),
                  style.outline.paint);
  emitDynamicPath(draw,
                  capFront ? makeParametricStroke(ring, ringTangent, 0.0f, tau,
                                                  style.outline.width, true)
                           : makeParametricStroke(ring, ringTangent, a, sweep,
                                                  style.outline.width, false),
                  style.outline.paint);
}

void drawMesh3D(DrawList &draw, const Mesh3D &mesh, const Mesh3DView &view,
                const Mesh3DStyle &style) {
  SceneObject3D object;
  object.mesh = &mesh;
  object.style = style;
  drawScene3D(draw, std::span<const SceneObject3D>(&object, 1), view);
}

Mesh3D makeBoxMesh3D(Vec3 h) {
  requireDimension(h.x);
  requireDimension(h.y);
  requireDimension(h.z);
  Mesh3D m;
  m.vertices = {{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z},
                {-h.x, h.y, -h.z},  {-h.x, -h.y, h.z}, {h.x, -h.y, h.z},
                {h.x, h.y, h.z},    {-h.x, h.y, h.z}};
  m.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                 {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},
                 {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
  orientGeneratedOutward(m);
  m.convex = true;
  prepareMesh3D(m);
  return m;
}

Mesh3D makeOctahedronMesh3D(float r) {
  requireDimension(r);
  Mesh3D m;
  m.vertices = {{r, 0, 0},  {-r, 0, 0}, {0, r, 0},
                {0, -r, 0}, {0, 0, r},  {0, 0, -r}};
  m.triangles = {{4, 0, 2}, {4, 2, 1}, {4, 1, 3}, {4, 3, 0},
                 {5, 2, 0}, {5, 1, 2}, {5, 3, 1}, {5, 0, 3}};
  orientGeneratedOutward(m);
  m.convex = true;
  prepareMesh3D(m);
  return m;
}

Mesh3D makeUvSphereMesh3D(float r, std::uint32_t slices, std::uint32_t stacks) {
  requireDimension(r);
  constexpr float pi = 3.14159265358979323846f;
  slices = std::max<std::uint32_t>(slices, 8);
  stacks = std::max<std::uint32_t>(stacks, 4);
  const std::uint64_t vertexCount = 2ull + std::uint64_t(stacks - 1) * slices;
  requireVertexBudget(vertexCount);
  Mesh3D m;
  m.vertices.reserve(static_cast<std::size_t>(vertexCount));
  m.triangles.reserve(static_cast<std::size_t>(2ull * slices * (stacks - 1)));
  const std::uint32_t bottom = 0;
  m.vertices.push_back({0, -r, 0});
  for (std::uint32_t j = 1; j < stacks; ++j) {
    const float phi = -0.5f * pi + pi * float(j) / float(stacks);
    const float cp = std::cos(phi), sp = std::sin(phi);
    for (std::uint32_t i = 0; i < slices; ++i) {
      const float theta = 2.0f * pi * float(i) / float(slices);
      m.vertices.push_back(
          {r * cp * std::cos(theta), r * sp, r * cp * std::sin(theta)});
    }
  }
  const std::uint32_t top = static_cast<std::uint32_t>(m.vertices.size());
  m.vertices.push_back({0, r, 0});
  const auto ring = [&](std::uint32_t j, std::uint32_t i) {
    return 1u + j * slices + (i % slices);
  };
  for (std::uint32_t i = 0; i < slices; ++i)
    m.triangles.push_back({bottom, ring(0, i), ring(0, i + 1)});
  for (std::uint32_t j = 0; j + 1 < stacks - 1; ++j) {
    for (std::uint32_t i = 0; i < slices; ++i) {
      const auto a = ring(j, i), b = ring(j, i + 1), c = ring(j + 1, i),
                 d = ring(j + 1, i + 1);
      m.triangles.push_back({a, c, b});
      m.triangles.push_back({b, c, d});
    }
  }
  const std::uint32_t last = stacks - 2;
  for (std::uint32_t i = 0; i < slices; ++i)
    m.triangles.push_back({top, ring(last, i + 1), ring(last, i)});
  m.convex = true;
  prepareMesh3D(m);
  return m;
}

Mesh3D makeCylinderMesh3D(float r, float h, std::uint32_t slices) {
  requireDimension(r);
  requireDimension(h);
  constexpr float pi = 3.14159265358979323846f;
  slices = std::max<std::uint32_t>(slices, 8);
  requireVertexBudget(2ull * slices + 2);
  Mesh3D m;
  m.vertices.reserve(slices * 2 + 2);
  m.triangles.reserve(static_cast<std::size_t>(slices) * 4);
  for (std::uint32_t i = 0; i < slices; ++i) {
    const float a = 2.0f * pi * float(i) / float(slices);
    m.vertices.push_back({r * std::cos(a), -h, r * std::sin(a)});
  }
  for (std::uint32_t i = 0; i < slices; ++i) {
    const float a = 2.0f * pi * float(i) / float(slices);
    m.vertices.push_back({r * std::cos(a), h, r * std::sin(a)});
  }
  const std::uint32_t bottomCenter =
      static_cast<std::uint32_t>(m.vertices.size());
  m.vertices.push_back({0, -h, 0});
  const std::uint32_t topCenter = static_cast<std::uint32_t>(m.vertices.size());
  m.vertices.push_back({0, h, 0});
  for (std::uint32_t i = 0; i < slices; ++i) {
    const std::uint32_t n = (i + 1) % slices;
    const std::uint32_t bi = i, bn = n, ti = slices + i, tn = slices + n;
    m.triangles.push_back({bi, ti, bn});
    m.triangles.push_back({bn, ti, tn});
    m.triangles.push_back({bottomCenter, bi, bn});
    m.triangles.push_back({topCenter, tn, ti});
  }
  m.convex = true;
  prepareMesh3D(m);
  return m;
}

Mesh3D makeConeMesh3D(float r, float h, std::uint32_t slices) {
  requireDimension(r);
  requireDimension(h);
  constexpr float pi = 3.14159265358979323846f;
  slices = std::max<std::uint32_t>(slices, 8);
  requireVertexBudget(std::uint64_t(slices) + 2);
  Mesh3D m;
  m.vertices.reserve(static_cast<std::size_t>(slices) + 2);
  m.triangles.reserve(static_cast<std::size_t>(slices) * 2);
  const std::uint32_t apex = 0;
  m.vertices.push_back({0, -h, 0});
  for (std::uint32_t i = 0; i < slices; ++i) {
    const float a = 2.0f * pi * float(i) / float(slices);
    m.vertices.push_back({r * std::cos(a), h, r * std::sin(a)});
  }
  const std::uint32_t center = static_cast<std::uint32_t>(m.vertices.size());
  m.vertices.push_back({0, h, 0});
  for (std::uint32_t i = 0; i < slices; ++i) {
    const std::uint32_t a = 1 + i, b = 1 + (i + 1) % slices;
    m.triangles.push_back({apex, a, b});
    m.triangles.push_back({center, b, a});
  }
  m.convex = true;
  prepareMesh3D(m);
  return m;
}

Mesh3D parseObjMesh3D(std::string_view objText,
                      const Mesh3DImportLimits &limits) {
  if (objText.size() > limits.maxInputBytes)
    throw std::length_error("OBJ input exceeds its byte budget");
  Mesh3D mesh;
  std::istringstream input{std::string(objText)};
  input.imbue(std::locale::classic());
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (const auto comment = line.find('#'); comment != std::string::npos)
      line.resize(comment);
    std::istringstream row(line);
    row.imbue(std::locale::classic());
    std::string tag;
    if (!(row >> tag))
      continue;
    const auto invalid = [&](const char *reason) {
      throw std::invalid_argument("OBJ line " + std::to_string(lineNumber) +
                                  ": " + reason);
    };
    if (tag == "v") {
      Vec3 v{};
      if (!(row >> v.x >> v.y >> v.z) || !finite(v))
        invalid("invalid vertex position");
      if (mesh.vertices.size() >= limits.maxVertices ||
          mesh.vertices.size() >= std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("OBJ exceeds its vertex budget");
      mesh.vertices.push_back(v);
      continue;
    }
    if (tag != "f")
      continue;
    std::vector<std::uint32_t> face;
    std::string token;
    while (row >> token) {
      const auto slash = token.find('/');
      std::string_view indexText(
          token.data(), slash == std::string::npos ? token.size() : slash);
      if (!indexText.empty() && indexText.front() == '+')
        indexText.remove_prefix(1);
      std::int64_t raw = 0;
      const auto parsed = std::from_chars(
          indexText.data(), indexText.data() + indexText.size(), raw);
      if (indexText.empty() || parsed.ec != std::errc{} ||
          parsed.ptr != indexText.data() + indexText.size() || raw == 0)
        invalid("invalid face index");
      const std::int64_t count =
          static_cast<std::int64_t>(mesh.vertices.size());
      // Check negative values before adding, including INT64_MIN.
      if (raw > count || raw < -count)
        invalid("face index is out of range");
      const std::int64_t index = raw > 0 ? raw - 1 : count + raw;
      if (face.size() >= limits.maxFaceVertices)
        throw std::length_error("OBJ face exceeds its vertex budget");
      face.push_back(static_cast<std::uint32_t>(index));
    }
    if (face.size() < 3)
      invalid("a face needs at least three vertices");
    const std::size_t added = face.size() - 2;
    const std::size_t triangleLimit = std::min<std::size_t>(
        limits.maxTriangles,
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
    if (added > triangleLimit || mesh.triangles.size() > triangleLimit - added)
      throw std::length_error("OBJ exceeds its triangle budget");
    try {
      const auto triangles = triangulateFace3D(mesh.vertices, face);
      mesh.triangles.insert(mesh.triangles.end(), triangles.begin(),
                            triangles.end());
    } catch (const std::invalid_argument &error) {
      invalid(error.what());
    }
  }
  prepareMesh3D(mesh);
  return mesh;
}

} // namespace slugia
