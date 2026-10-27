#include "slugia/vector3d.hpp"
#include "slugia/vector_path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace slugia {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, double s) {
  return {float(a.x * s), float(a.y * s), float(a.z * s)};
}
double dot(Vec3 a, Vec3 b) {
  return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
}
Vec3 cross(Vec3 a, Vec3 b) {
  return {float(double(a.y) * b.z - double(a.z) * b.y),
          float(double(a.z) * b.x - double(a.x) * b.z),
          float(double(a.x) * b.y - double(a.y) * b.x)};
}
Vec3 unit(Vec3 v) {
  const double n = std::sqrt(dot(v, v));
  return n > 1e-20 ? mul(v, 1 / n) : Vec3{};
}
bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
double area(Vec2 a, Vec2 b) { return double(a.x) * b.y - double(a.y) * b.x; }
Vec3 rotate(Vec3 p, Vec3 r) {
  double c = std::cos(r.x), s = std::sin(r.x);
  p = {p.x, float(p.y * c - p.z * s), float(p.y * s + p.z * c)};
  c = std::cos(r.y);
  s = std::sin(r.y);
  p = {float(p.x * c + p.z * s), p.y, float(-p.x * s + p.z * c)};
  c = std::cos(r.z);
  s = std::sin(r.z);
  return {float(p.x * c - p.y * s), float(p.x * s + p.y * c), p.z};
}
void checkAffine(const AffineTransform3D &a) {
  if (!finite(a.x) || !finite(a.y) || !finite(a.z) || !finite(a.translation))
    throw std::invalid_argument("3D transforms must be finite");
}
Vec3 vector(const AffineTransform3D &a, Vec3 p) {
  return add(add(mul(a.x, p.x), mul(a.y, p.y)), mul(a.z, p.z));
}
struct Camera {
  Camera3D source;
  Vec2 origin;
  Vec3 x, y, z;
  double focal, a, b, nearZ, farZ;
  Camera(const Mesh3DView &view) : source(view.camera), origin(view.origin) {
    if (!finite(source.position) || !finite(source.rotation) ||
        !std::isfinite(source.perspective) || source.perspective < 0 ||
        !std::isfinite(source.focalLength) || source.focalLength < 0 ||
        !std::isfinite(source.nearPlane) || source.nearPlane <= 0 ||
        !std::isfinite(source.farPlane) ||
        source.farPlane <= source.nearPlane || !std::isfinite(origin.x) ||
        !std::isfinite(origin.y))
      throw std::invalid_argument("Invalid 3D camera or clipping range");
    focal = source.focalLength > 0 ? source.focalLength
                                   : (std::abs(source.position.z) > 1e-5
                                          ? std::abs(source.position.z)
                                          : 520.0);
    a = 1.0 - source.perspective;
    b = source.perspective / focal;
    nearZ = source.nearPlane;
    farZ = source.farPlane;
    if (b > 0)
      nearZ = std::max(nearZ, (source.nearPlane / focal - a) / b);
    x = rotate({1, 0, 0}, source.rotation);
    y = rotate({0, 1, 0}, source.rotation);
    z = rotate({0, 0, 1}, source.rotation);
  }
  Vec3 toCamera(Vec3 p) const {
    p = sub(p, source.position);
    return {float(dot(p, x)), float(dot(p, y)), float(dot(p, z))};
  }
  Vec3 direction(Vec3 p) const {
    return {float(dot(p, x)), float(dot(p, y)), float(dot(p, z))};
  }
  Vec3 toWorld(Vec3 p) const {
    return add(source.position,
               add(add(mul(x, p.x), mul(y, p.y)), mul(z, p.z)));
  }
  double w(Vec3 p) const { return a + b * p.z; }
  Vec2 screen(Vec3 p) const {
    const double d = w(p);
    return {float(origin.x + p.x / d), float(origin.y + p.y / d)};
  }
  double metric(Vec3 p) const { return (1.0 - p.z / focal) / w(p); }
};
struct Plane3 {
  double a, b, c, d;
  double eval(Vec3 p) const { return a * p.x + b * p.y + c * p.z + d; }
};
std::array<Plane3, 6> frustum(const Camera &c, Rect clip, double padding) {
  const double l = clip.x - c.origin.x - padding,
               r = clip.x + clip.width - c.origin.x + padding;
  const double t = clip.y - c.origin.y - padding,
               b = clip.y + clip.height - c.origin.y + padding;
  return {{{0, 0, 1, -c.nearZ},
           {0, 0, -1, c.farZ},
           {1, 0, -l * c.b, -l * c.a},
           {-1, 0, r * c.b, r * c.a},
           {0, 1, -t * c.b, -t * c.a},
           {0, -1, b * c.b, b * c.a}}};
}
std::vector<Vec3> clip3(std::vector<Vec3> poly, const Plane3 &plane) {
  std::vector<Vec3> out;
  if (poly.empty())
    return out;
  Vec3 previous = poly.back();
  double dp = plane.eval(previous);
  for (Vec3 p : poly) {
    const double d = plane.eval(p);
    if ((d >= 0) != (dp >= 0)) {
      const double t = dp / (dp - d);
      out.push_back(add(previous, mul(sub(p, previous), t)));
    }
    if (d >= 0)
      out.push_back(p);
    previous = p;
    dp = d;
  }
  return out;
}
bool clipLine3(Vec3 &a, Vec3 &b, const std::array<Plane3, 6> &planes) {
  double lo = 0, hi = 1;
  const Vec3 delta = sub(b, a);
  for (const auto &plane : planes) {
    const double d = plane.eval(a), e = plane.eval(b);
    if (d < 0 && e < 0)
      return false;
    if ((d < 0) != (e < 0)) {
      const double t = d / (d - e);
      if (d < 0)
        lo = std::max(lo, t);
      else
        hi = std::min(hi, t);
    }
  }
  b = add(a, mul(delta, hi));
  a = add(a, mul(delta, lo));
  return hi > lo;
}
using Polygon = std::vector<Vec2>;
double signedArea(const Polygon &p) {
  double a = 0;
  for (std::size_t i = 0; i < p.size(); ++i)
    a += area(p[i], p[(i + 1) % p.size()]);
  return a * .5;
}
Rect bounds(const Polygon &p) {
  float l = p[0].x, r = l, t = p[0].y, b = t;
  for (Vec2 v : p) {
    l = std::min(l, v.x);
    r = std::max(r, v.x);
    t = std::min(t, v.y);
    b = std::max(b, v.y);
  }
  return {l, t, r - l, b - t};
}
bool overlaps(Rect a, Rect b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}
struct Plane2 {
  double a = 0, b = 0, c = 0;
  double eval(Vec2 p) const { return a * p.x + b * p.y + c; }
};
Polygon clip2(const Polygon &poly, Plane2 plane) {
  Polygon out;
  if (poly.empty())
    return out;
  out.reserve(poly.size() + 1);
  const auto append = [&](Vec2 point) {
    if (out.empty() || out.back().x != point.x || out.back().y != point.y)
      out.push_back(point);
  };
  Vec2 previous = poly.back();
  double dp = plane.eval(previous);
  for (Vec2 p : poly) {
    const double d = plane.eval(p);
    if ((d >= 0) != (dp >= 0)) {
      const double t = dp / (dp - d);
      append({float(previous.x + (p.x - previous.x) * t),
              float(previous.y + (p.y - previous.y) * t)});
    }
    if (d >= 0)
      append(p);
    previous = p;
    dp = d;
  }
  if (out.size() > 1 && out.front().x == out.back().x &&
      out.front().y == out.back().y)
    out.pop_back();
  return out;
}
Plane2 edgePlane(Vec2 a, Vec2 b) {
  return {double(a.y) - b.y, double(b.x) - a.x,
          double(a.x) * b.y - double(a.y) * b.x};
}
bool usable(const Polygon &p) {
  if (p.size() < 3)
    return false;
  double magnitude = 1, perimeter = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    magnitude = std::max(
        {magnitude, std::abs(double(p[i].x)), std::abs(double(p[i].y))});
    const auto delta = p[(i + 1) % p.size()] - p[i];
    perimeter += std::hypot(double(delta.x), double(delta.y));
  }
  // Intersections are ultimately stored as float coordinates. Reject fragments
  // whose area is within their coordinate-rounding uncertainty; sending those
  // zero-area slivers to a winding solver produces unstable false coverage.
  const double uncertainty =
      2 * std::numeric_limits<float>::epsilon() * magnitude * perimeter;
  return std::abs(signedArea(p)) > std::max(1e-7, uncertainty);
}
std::vector<Polygon> subtract(const Polygon &subject, const Polygon &clip) {
  std::vector<Polygon> result;
  Polygon inside = subject;
  for (std::size_t i = 0; i < clip.size() && usable(inside); ++i) {
    if (clip[i].x == clip[(i + 1) % clip.size()].x &&
        clip[i].y == clip[(i + 1) % clip.size()].y)
      continue;
    const Plane2 plane = edgePlane(clip[i], clip[(i + 1) % clip.size()]);
    auto outside = clip2(inside, {-plane.a, -plane.b, -plane.c});
    if (usable(outside))
      result.push_back(std::move(outside));
    inside = clip2(inside, plane);
  }
  return result;
}
Polygon intersect(Polygon subject, const Polygon &clip) {
  for (std::size_t i = 0; i < clip.size() && usable(subject); ++i)
    subject = clip2(subject, edgePlane(clip[i], clip[(i + 1) % clip.size()]));
  return subject;
}
Plane2 depthPlane(const Polygon &p, const std::vector<double> &depth) {
  for (std::size_t i = 1; i + 1 < p.size(); ++i) {
    const Vec2 a = p[i] - p[0], b = p[i + 1] - p[0];
    const double den = area(a, b);
    if (std::abs(den) < 1e-12)
      continue;
    const double d1 = depth[i] - depth[0], d2 = depth[i + 1] - depth[0];
    const double x = (d1 * b.y - d2 * a.y) / den,
                 y = (a.x * d2 - b.x * d1) / den;
    return {x, y, depth[0] - x * p[0].x - y * p[0].y};
  }
  return {};
}
bool opaque(const Paint &p) {
  return p.opacity * std::min(p.start.a, p.end.a) >= 1.0f - 1e-6f;
}
bool invisible(const Paint &p) {
  return p.opacity <= 0 || (p.start.a <= 0 && p.end.a <= 0);
}
Paint shade(Paint paint, Vec3 normal, const Camera &camera,
            const Scene3DConfig &config) {
  const Vec3 light = unit(camera.direction(config.lightDirection));
  const float raw =
      std::clamp(config.ambient +
                     config.diffuse * float(std::max(0.0, dot(normal, light))),
                 0.0f, 1.0f);
  const float s = std::round(raw * 256.0f) / 256.0f;
  for (Color *color : {&paint.start, &paint.end}) {
    color->r *= s;
    color->g *= s;
    color->b *= s;
  }
  return paint;
}
struct Face {
  Polygon polygon;
  Plane2 depth;
  Rect box{};
  Paint paint;
  std::size_t object = 0, triangle = 0;
  bool opaque = false;
};
class Index {
  struct Node {
    Rect box;
    std::size_t first = 0, count = 0, left = 0, right = 0;
  };
  const std::vector<Face> &faces;
  std::vector<std::size_t> order;
  std::vector<Node> nodes;
  std::size_t build(std::size_t first, std::size_t count) {
    Rect box = faces[order[first]].box;
    for (std::size_t i = first + 1; i < first + count; ++i) {
      const auto q = faces[order[i]].box;
      const float r = std::max(box.x + box.width, q.x + q.width),
                  b = std::max(box.y + box.height, q.y + q.height);
      box.x = std::min(box.x, q.x);
      box.y = std::min(box.y, q.y);
      box.width = r - box.x;
      box.height = b - box.y;
    }
    const auto id = nodes.size();
    nodes.push_back({box, first, count, 0, 0});
    if (count <= 8)
      return id;
    const auto middle = first + count / 2;
    const bool horizontal = box.width >= box.height;
    std::nth_element(order.begin() + first, order.begin() + middle,
                     order.begin() + first + count, [&](auto a, auto b) {
                       const auto x = faces[a].box, y = faces[b].box;
                       const float ca = horizontal ? x.x + x.width * .5f
                                                   : x.y + x.height * .5f;
                       const float cb = horizontal ? y.x + y.width * .5f
                                                   : y.y + y.height * .5f;
                       return ca == cb ? a < b : ca < cb;
                     });
    const auto left = build(first, middle - first),
               right = build(middle, first + count - middle);
    nodes[id].count = 0;
    nodes[id].left = left;
    nodes[id].right = right;
    return id;
  }

public:
  explicit Index(const std::vector<Face> &source)
      : faces(source), order(source.size()) {
    std::iota(order.begin(), order.end(), 0);
    nodes.reserve(source.size() * 2);
    if (!source.empty())
      build(0, source.size());
  }
  template <class F> void query(Rect box, F &&fn) const {
    if (nodes.empty())
      return;
    std::array<std::size_t, 64> stack{};
    std::size_t size = 1;
    while (size) {
      const auto &node = nodes[stack[--size]];
      if (!overlaps(box, node.box))
        continue;
      if (node.count) {
        for (std::size_t i = node.first; i < node.first + node.count; ++i)
          if (overlaps(box, faces[order[i]].box))
            fn(order[i]);
      } else {
        stack[size++] = node.left;
        stack[size++] = node.right;
      }
    }
  }
};
struct ObjectState {
  std::vector<Vec3> camera;
  std::vector<Vec3> normals;
  std::vector<bool> front, valid;
  std::vector<MeshEdge3D> fallbackEdges;
};
void emitPolygon(std::vector<QuadraticBezier2D> &out, const Polygon &p) {
  for (std::size_t i = 0; i < p.size(); ++i)
    appendLineQuadratic(out, p[i], p[(i + 1) % p.size()]);
}
void emitPath(DrawList &draw, std::vector<QuadraticBezier2D> curves,
              const Paint &paint, bool additive = false) {
  if (curves.empty() || invisible(paint))
    return;
  const Rect box = quadraticBounds(curves);
  if (box.width > 0 && box.height > 0)
    draw.dynamicVectorPath(std::move(curves), box, paint, FillRule::NonZero,
                           false, additive);
}
std::array<std::uint32_t, 15> paintKey(const Paint &p) {
  std::array<std::uint32_t, 15> key{};
  key[0] = static_cast<std::uint32_t>(p.kind);
  const float values[]{p.start.r,  p.start.g,        p.start.b,  p.start.a,
                       p.end.r,    p.end.g,          p.end.b,    p.end.a,
                       p.origin.x, p.origin.y,       p.target.x, p.target.y,
                       p.opacity,  p.shaderParameter};
  for (std::size_t i = 0; i < 14; ++i)
    key[i + 1] = std::bit_cast<std::uint32_t>(values[i]);
  return key;
}
} // namespace

AffineTransform3D affineTransform3D(const Transform3D &transform) {
  if (!finite(transform.translation) || !finite(transform.rotation) ||
      !finite(transform.scale))
    throw std::invalid_argument("3D transforms must be finite");
  AffineTransform3D result{
      rotate({transform.scale.x, 0, 0}, transform.rotation),
      rotate({0, transform.scale.y, 0}, transform.rotation),
      rotate({0, 0, transform.scale.z}, transform.rotation),
      transform.translation};
  checkAffine(result);
  return result;
}
Vec3 transformPoint3D(const AffineTransform3D &transform, Vec3 point) {
  return add(vector(transform, point), transform.translation);
}
AffineTransform3D composeTransform3D(const AffineTransform3D &parent,
                                     const AffineTransform3D &local) {
  checkAffine(parent);
  checkAffine(local);
  AffineTransform3D result{vector(parent, local.x), vector(parent, local.y),
                           vector(parent, local.z),
                           transformPoint3D(parent, local.translation)};
  checkAffine(result);
  return result;
}
ProjectedPoint3D projectPoint3D(Vec3 point, const Mesh3DView &view) {
  if (!finite(point))
    throw std::invalid_argument("3D point must be finite");
  const Camera c(view);
  const auto world = transformPoint3D(
      composeTransform3D(view.parent, affineTransform3D(view.transform)),
      point);
  const Vec3 p = c.toCamera(world);
  const double w = c.w(p);
  if (!finite(p) || w <= 0)
    return {{}, p.z, float(w), false};
  const Vec2 screen = c.screen(p);
  return {screen, p.z, float(w),
          p.z >= c.nearZ && p.z <= c.farZ && std::isfinite(screen.x) &&
              std::isfinite(screen.y)};
}
ProjectiveTransform2D projectPlane3D(const Mesh3DView &view) {
  const Camera c(view);
  const auto world =
      composeTransform3D(view.parent, affineTransform3D(view.transform));
  const Vec3 p = c.toCamera(world.translation), x = c.direction(world.x),
             y = c.direction(world.y);
  ProjectiveTransform2D h;
  h.matrix = {float(x.x + c.origin.x * c.b * x.z),
              float(y.x + c.origin.x * c.b * y.z),
              float(p.x + c.origin.x * c.w(p)),
              float(x.y + c.origin.y * c.b * x.z),
              float(y.y + c.origin.y * c.b * y.z),
              float(p.y + c.origin.y * c.w(p)),
              float(c.b * x.z),
              float(c.b * y.z),
              float(c.w(p))};
  const double factor = c.w({0, 0, float(c.farZ)}) / (c.farZ - c.nearZ);
  h.clipDepth = {float(x.z * factor), float(y.z * factor),
                 float((p.z - c.nearZ) * factor)};
  return h;
}
std::optional<Vec2> unprojectPlane3D(Vec2 screen, const Mesh3DView &view) {
  const auto h = projectPlane3D(view).matrix;
  const double a = h[0] - screen.x * h[6], b = h[1] - screen.x * h[7],
               c = screen.x * h[8] - h[2];
  const double d = h[3] - screen.y * h[6], e = h[4] - screen.y * h[7],
               f = screen.y * h[8] - h[5];
  const double determinant = a * e - b * d;
  if (std::abs(determinant) < 1e-12)
    return std::nullopt;
  const Vec2 local{float((c * e - b * f) / determinant),
                   float((a * f - c * d) / determinant)};
  if (!projectPoint3D({local.x, local.y, 0}, view).visible)
    return std::nullopt;
  return local;
}
void drawPlane3D(DrawList &draw, const DrawList &content,
                 const Mesh3DView &view) {
  draw.projected(content, projectPlane3D(view));
}

Scene3DStats drawScene3D(DrawList &draw, std::span<const SceneObject3D> objects,
                         const Mesh3DView &view, const Scene3DConfig &config) {
  const Camera camera(view);
  const auto root =
      composeTransform3D(view.parent, affineTransform3D(view.transform));
  if (!finite(config.lightDirection) ||
      dot(config.lightDirection, config.lightDirection) < 1e-20 ||
      !std::isfinite(config.ambient) || !std::isfinite(config.diffuse) ||
      config.ambient < 0 || config.diffuse < 0 ||
      !std::isfinite(config.curveTolerancePx) || config.curveTolerancePx <= 0)
    throw std::invalid_argument("Invalid 3D scene lighting");
  Scene3DStats stats;
  std::vector<Face> faces;
  std::vector<ObjectState> states(objects.size());
  const auto charge = [&]() {
    if (++stats.occlusionTests > config.maxOcclusionTests)
      throw std::length_error("3D scene occlusion budget exceeded");
  };
  const auto fragmentBudget = [&](std::size_t count) {
    if (count > config.maxFragments)
      throw std::length_error("3D scene fragment budget exceeded");
  };
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto &object = objects[oi];
    if (!object.mesh)
      continue;
    const auto &mesh = *object.mesh;
    auto &state = states[oi];
    if (!std::isfinite(object.style.edge.width) ||
        !std::isfinite(object.style.creaseAngleRadians))
      throw std::invalid_argument("3D stroke values must be finite");
    const auto world = composeTransform3D(
        root,
        composeTransform3D(object.parent, affineTransform3D(object.transform)));
    const double orientation =
        dot(cross(world.x, world.y), world.z) < 0 ? -1 : 1;
    state.camera.reserve(mesh.vertices.size());
    for (const Vec3 p : mesh.vertices) {
      const auto q = camera.toCamera(transformPoint3D(world, p));
      if (!finite(p) || !finite(q))
        throw std::invalid_argument("3D transformed vertex must be finite");
      state.camera.push_back(q);
    }
    state.normals.resize(mesh.triangles.size());
    state.front.resize(mesh.triangles.size());
    state.valid.resize(mesh.triangles.size());
    if (!mesh.trianglePaints.empty() &&
        mesh.trianglePaints.size() != mesh.triangles.size())
      throw std::invalid_argument("Triangle material count mismatch");
    const auto planes =
        frustum(camera, draw.clip(), std::max(1.0f, object.style.edge.width));
    for (std::size_t ti = 0; ti < mesh.triangles.size(); ++ti) {
      ++stats.triangles;
      const auto t = mesh.triangles[ti];
      if (t.a >= state.camera.size() || t.b >= state.camera.size() ||
          t.c >= state.camera.size())
        continue;
      const Vec3 a = state.camera[t.a], b = state.camera[t.b],
                 c = state.camera[t.c];
      Vec3 normal = unit(mul(cross(sub(b, a), sub(c, a)), orientation));
      if (!finite(normal) || dot(normal, normal) < .5)
        continue;
      const Vec3 center = mul(add(add(a, b), c), 1.0 / 3.0);
      const Vec3 direction{float(camera.source.perspective * center.x),
                           float(camera.source.perspective * center.y),
                           float(camera.focal * camera.w(center))};
      state.normals[ti] = normal;
      state.front[ti] = dot(normal, direction) < 0;
      state.valid[ti] = true;
      if (object.style.cullBackfaces && !state.front[ti])
        continue;
      std::vector<Vec3> poly{a, b, c};
      bool clipped = false;
      for (const auto &plane : planes) {
        if (std::any_of(poly.begin(), poly.end(),
                        [&](Vec3 p) { return plane.eval(p) < 0; }))
          clipped = true;
        poly = clip3(std::move(poly), plane);
        if (poly.empty())
          break;
      }
      if (clipped)
        ++stats.clippedTriangles;
      if (poly.size() < 3)
        continue;
      Polygon screen;
      std::vector<double> depth;
      for (Vec3 p : poly) {
        screen.push_back(camera.screen(p));
        depth.push_back(camera.metric(p));
      }
      if (!usable(screen))
        continue;
      const auto dp = depthPlane(screen, depth);
      if (signedArea(screen) < 0)
        std::reverse(screen.begin(), screen.end());
      Paint paint = mesh.trianglePaints.empty() ? object.style.fill
                                                : mesh.trianglePaints[ti];
      if (object.style.shaded)
        paint = shade(paint, state.front[ti] ? normal : mul(normal, -1), camera,
                      config);
      const Rect box = bounds(screen);
      faces.push_back(
          {std::move(screen), dp, box, paint, oi, ti, opaque(paint)});
      fragmentBudget(faces.size());
    }
    if (mesh.edges.empty() && object.style.drawEdges) {
      auto copy = mesh;
      prepareMesh3D(copy);
      state.fallbackEdges = std::move(copy.edges);
    }
  }
  const Index index(faces);
  struct Fragment {
    Polygon polygon;
    std::size_t face;
  };
  std::vector<Fragment> fragments, transparent;
  for (std::size_t fi = 0; fi < faces.size(); ++fi) {
    const auto &face = faces[fi];
    if (invisible(face.paint))
      continue;
    std::vector<Polygon> pieces{face.polygon};
    index.query(face.box, [&](std::size_t otherIndex) {
      const auto &other = faces[otherIndex];
      if (otherIndex == fi || !other.opaque || pieces.empty())
        return;
      if (other.object == face.object &&
          ((objects[face.object].mesh->convex &&
            objects[face.object].style.cullBackfaces) ||
           (!objects[face.object].style.shaded &&
            objects[face.object].mesh->trianglePaints.empty())))
        return;
      charge();
      const Plane2 difference{other.depth.a - face.depth.a,
                              other.depth.b - face.depth.b,
                              other.depth.c - face.depth.c};
      double separation = 0;
      for (const auto p : other.polygon)
        separation = std::max(separation, std::abs(difference.eval(p)));
      // Equal-depth surfaces need one deterministic owner. Other surfaces meet
      // exactly at their depth boundary: a positive bias leaves thin back-face
      // strips that have no stable winding coverage in float precision.
      if (separation < 1e-10 && otherIndex < fi)
        return;
      auto occluder = clip2(other.polygon, difference);
      if (!usable(occluder))
        return;
      std::vector<Polygon> next;
      for (const auto &piece : pieces) {
        if (!overlaps(bounds(piece), other.box)) {
          next.push_back(piece);
          continue;
        }
        auto remaining = subtract(piece, occluder);
        for (auto &p : remaining)
          next.push_back(std::move(p));
        fragmentBudget(next.size() + fragments.size() + transparent.size());
      }
      pieces = std::move(next);
    });
    // Transparent surfaces are split at overlap and equal-depth boundaries.
    // This turns crossing faces into a finite arrangement with a consistent
    // local order.
    if (!face.opaque)
      index.query(face.box, [&](std::size_t oi) {
        const auto &other = faces[oi];
        if (oi == fi || other.opaque || invisible(other.paint))
          return;
        if (!usable(intersect(face.polygon, other.polygon)))
          return;
        charge();
        std::vector<Plane2> lines;
        lines.push_back({other.depth.a - face.depth.a,
                         other.depth.b - face.depth.b,
                         other.depth.c - face.depth.c});
        for (std::size_t i = 0; i < other.polygon.size(); ++i)
          lines.push_back(edgePlane(
              other.polygon[i], other.polygon[(i + 1) % other.polygon.size()]));
        for (const auto line : lines) {
          if (std::abs(line.a) + std::abs(line.b) < 1e-14)
            continue;
          std::vector<Polygon> next;
          for (auto &p : pieces) {
            auto a = clip2(p, line), b = clip2(p, {-line.a, -line.b, -line.c});
            if (usable(a))
              next.push_back(std::move(a));
            if (usable(b))
              next.push_back(std::move(b));
          }
          pieces = std::move(next);
          fragmentBudget(pieces.size() + fragments.size() + transparent.size());
        }
      });
    for (auto &piece : pieces) {
      auto &target = face.opaque ? fragments : transparent;
      target.push_back({std::move(piece), fi});
    }
    fragmentBudget(fragments.size() + transparent.size());
  }
  DrawList output;
  output.setClip(draw.clip());
  using GroupKey = std::array<std::uint32_t, 15>;
  struct Group {
    Paint paint;
    std::vector<QuadraticBezier2D> curves;
  };
  std::map<GroupKey, Group> groups;
  const auto emitGroups = [&](std::map<GroupKey, Group> &layer) {
    if (layer.empty())
      return;
    const auto &first = layer.begin()->second.paint;
    const float alpha = first.opacity * first.start.a;
    const bool constantAlpha =
        std::all_of(layer.begin(), layer.end(), [&](const auto &entry) {
          const auto &p = entry.second.paint;
          return std::abs(p.opacity * p.start.a - alpha) < 1e-7f &&
                 std::abs(p.opacity * p.end.a - alpha) < 1e-7f;
        });
    if (layer.size() > 1 && constantAlpha) {
      // First remove the background through one union coverage mask. Then add
      // the non-overlapping material coverages in linear color. Shared
      // triangle/face boundaries sum to the union instead of alpha-blending a
      // dark seam.
      std::vector<QuadraticBezier2D> mask;
      for (const auto &[key, group] : layer) {
        (void)key;
        mask.insert(mask.end(), group.curves.begin(), group.curves.end());
      }
      emitPath(output, std::move(mask), Paint::solid({0, 0, 0, alpha}));
      for (auto &[key, group] : layer) {
        (void)key;
        emitPath(output, std::move(group.curves), group.paint, true);
      }
    } else
      for (auto &[key, group] : layer) {
        (void)key;
        emitPath(output, std::move(group.curves), group.paint);
      }
  };
  for (const auto &fragment : fragments) {
    const auto &face = faces[fragment.face];
    auto &group = groups[paintKey(face.paint)];
    group.paint = face.paint;
    emitPolygon(group.curves, fragment.polygon);
  }
  emitGroups(groups);
  if (!transparent.empty()) {
    const auto n = transparent.size();
    std::vector<std::vector<std::size_t>> next(n);
    std::vector<std::size_t> degree(n);
    std::vector<Rect> boxes;
    for (const auto &f : transparent)
      boxes.push_back(bounds(f.polygon));
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = i + 1; j < n; ++j) {
        if (!overlaps(boxes[i], boxes[j]))
          continue;
        charge();
        const auto overlap =
            intersect(transparent[i].polygon, transparent[j].polygon);
        if (!usable(overlap))
          continue;
        Vec2 p{};
        for (const auto vertex : overlap)
          p = p + vertex;
        p = p * (1.0f / static_cast<float>(overlap.size()));
        const double d = faces[transparent[i].face].depth.eval(p) -
                         faces[transparent[j].face].depth.eval(p);
        const auto far = d > 1e-7 ? j : i, near = d > 1e-7 ? i : j;
        next[far].push_back(near);
        ++degree[near];
      }
    std::vector<std::size_t> ready;
    std::vector<std::size_t> level(n);
    std::map<std::size_t, std::map<GroupKey, Group>> layers;
    for (std::size_t i = 0; i < n; ++i)
      if (degree[i] == 0)
        ready.push_back(i);
    std::size_t emitted = 0;
    while (!ready.empty()) {
      const auto i = ready.back();
      ready.pop_back();
      ++emitted;
      const auto &paint = faces[transparent[i].face].paint;
      auto &group = layers[level[i]][paintKey(paint)];
      group.paint = paint;
      emitPolygon(group.curves, transparent[i].polygon);
      for (auto j : next[i]) {
        level[j] = std::max(level[j], level[i] + 1);
        if (--degree[j] == 0)
          ready.push_back(j);
      }
    }
    if (emitted != n)
      throw std::runtime_error(
          "Numerically ambiguous transparent 3D arrangement");
    for (auto &[depth, layer] : layers) {
      (void)depth;
      emitGroups(layer);
    }
  }
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto &object = objects[oi];
    if (!object.mesh || !object.style.drawEdges || object.style.edge.width <= 0)
      continue;
    const auto &mesh = *object.mesh;
    const auto &state = states[oi];
    const auto &edges = mesh.edges.empty() ? state.fallbackEdges : mesh.edges;
    const auto planes =
        frustum(camera, draw.clip(), object.style.edge.width + 1);
    const double crease =
        std::cos(std::max(0.0f, object.style.creaseAngleRadians));
    std::vector<QuadraticBezier2D> curves;
    for (const auto &edge : edges) {
      if (edge.a >= state.camera.size() || edge.b >= state.camera.size())
        continue;
      const auto valid = [&](std::int32_t i) {
        return i >= 0 && std::size_t(i) < state.valid.size() &&
               state.valid[std::size_t(i)];
      };
      const bool v0 = valid(edge.firstTriangle),
                 v1 = valid(edge.secondTriangle);
      if (!v0 && !v1)
        continue;
      const bool f0 = v0 && state.front[std::size_t(edge.firstTriangle)],
                 f1 = v1 && state.front[std::size_t(edge.secondTriangle)];
      if (object.style.cullBackfaces && !f0 && !f1)
        continue;
      if (!edge.sharp && v0 && v1 && f0 == f1 &&
          dot(state.normals[std::size_t(edge.firstTriangle)],
              state.normals[std::size_t(edge.secondTriangle)]) >= crease)
        continue;
      Vec3 a = state.camera[edge.a], b = state.camera[edge.b];
      if (!clipLine3(a, b, planes))
        continue;
      const Vec2 p = camera.screen(a), q = camera.screen(b);
      const double da = camera.metric(a), db = camera.metric(b);
      const Rect box{std::min(p.x, q.x) - .001f, std::min(p.y, q.y) - .001f,
                     std::abs(p.x - q.x) + .002f, std::abs(p.y - q.y) + .002f};
      std::vector<std::pair<double, double>> intervals{{0, 1}};
      index.query(box, [&](std::size_t fi) {
        const auto &face = faces[fi];
        if (!face.opaque || intervals.empty())
          return;
        if (face.object == oi &&
            (std::int32_t(face.triangle) == edge.firstTriangle ||
             std::int32_t(face.triangle) == edge.secondTriangle ||
             (mesh.convex && object.style.cullBackfaces)))
          return;
        charge();
        double lo = 0, hi = 1;
        for (std::size_t i = 0; i < face.polygon.size(); ++i) {
          const auto line = edgePlane(
              face.polygon[i], face.polygon[(i + 1) % face.polygon.size()]);
          const double x = line.eval(p), y = line.eval(q);
          if (x < 0 && y < 0)
            return;
          if ((x < 0) != (y < 0)) {
            const double t = x / (x - y);
            if (x < 0)
              lo = std::max(lo, t);
            else
              hi = std::min(hi, t);
          }
        }
        const double x = face.depth.eval(p) - da - 1e-7,
                     y = face.depth.eval(q) - db - 1e-7;
        if (x <= 0 && y <= 0)
          return;
        if (x <= 0 || y <= 0) {
          const double t = x / (x - y);
          if (x <= 0)
            lo = std::max(lo, t);
          else
            hi = std::min(hi, t);
        }
        if (lo >= hi)
          return;
        std::vector<std::pair<double, double>> remaining;
        for (auto [l, h] : intervals) {
          if (hi <= l || lo >= h) {
            remaining.push_back({l, h});
            continue;
          }
          if (lo > l)
            remaining.push_back({l, std::min(lo, h)});
          if (hi < h)
            remaining.push_back({std::max(hi, l), h});
        }
        intervals = std::move(remaining);
      });
      for (auto [lo, hi] : intervals) {
        const Vec2 x = p + (q - p) * float(lo), y = p + (q - p) * float(hi);
        auto stroke = makeLineStrokeQuadratics(x, y, object.style.edge.width,
                                               object.style.edge.cap);
        curves.insert(curves.end(), stroke.begin(), stroke.end());
      }
    }
    emitPath(output, std::move(curves), object.style.edge.paint);
  }
  // Native rational Bezier spans are flattened adaptively in the current view.
  // Positive-weight convex hulls give a conservative screen-space chord error.
  std::size_t subdivisions = 0;
  for (const auto &object : objects) {
    if (object.curves.empty() || object.style.edge.width <= 0 ||
        invisible(object.style.edge.paint))
      continue;
    const auto world = composeTransform3D(
        root,
        composeTransform3D(object.parent, affineTransform3D(object.transform)));
    const auto planes =
        frustum(camera, draw.clip(), object.style.edge.width + 1);
    std::vector<QuadraticBezier2D> curves;
    const auto line = [&](Vec3 a, Vec3 b) {
      if (!clipLine3(a, b, planes))
        return;
      const Vec2 p = camera.screen(a), q = camera.screen(b);
      const double da = camera.metric(a), db = camera.metric(b);
      const Rect box{std::min(p.x, q.x) - .001f, std::min(p.y, q.y) - .001f,
                     std::abs(p.x - q.x) + .002f, std::abs(p.y - q.y) + .002f};
      std::vector<std::pair<double, double>> intervals{{0, 1}};
      index.query(box, [&](std::size_t fi) {
        const auto &face = faces[fi];
        if (!face.opaque || intervals.empty())
          return;
        charge();
        double lo = 0, hi = 1;
        for (std::size_t i = 0; i < face.polygon.size(); ++i) {
          const auto plane = edgePlane(
              face.polygon[i], face.polygon[(i + 1) % face.polygon.size()]);
          const double x = plane.eval(p), y = plane.eval(q);
          if (x < 0 && y < 0)
            return;
          if ((x < 0) != (y < 0)) {
            const double t = x / (x - y);
            if (x < 0)
              lo = std::max(lo, t);
            else
              hi = std::min(hi, t);
          }
        }
        const double x = face.depth.eval(p) - da - 1e-7,
                     y = face.depth.eval(q) - db - 1e-7;
        if (x <= 0 && y <= 0)
          return;
        if (x <= 0 || y <= 0) {
          const double t = x / (x - y);
          if (x <= 0)
            lo = std::max(lo, t);
          else
            hi = std::min(hi, t);
        }
        if (lo >= hi)
          return;
        std::vector<std::pair<double, double>> remaining;
        for (auto [l, h] : intervals) {
          if (hi <= l || lo >= h) {
            remaining.push_back({l, h});
            continue;
          }
          if (lo > l)
            remaining.push_back({l, std::min(lo, h)});
          if (hi < h)
            remaining.push_back({std::max(hi, l), h});
        }
        intervals = std::move(remaining);
      });
      for (auto [lo, hi] : intervals) {
        auto stroke = makeLineStrokeQuadratics(
            p + (q - p) * float(lo), p + (q - p) * float(hi),
            object.style.edge.width, LineCap::Round);
        curves.insert(curves.end(), stroke.begin(), stroke.end());
        ++stats.curveSegments;
      }
    };
    struct Weighted {
      Vec3 p;
      double w = 1;
    };
    struct Controls {
      std::array<Weighted, 16> points;
      std::size_t count = 0;
    };
    const auto flatten = [&](auto &&self, const Controls &control,
                             int depth) -> void {
      if (++subdivisions > config.maxCurveSubdivisions)
        throw std::length_error("3D curve subdivision budget exceeded");
      for (const auto &plane : planes) {
        bool outside = true;
        for (std::size_t i = 0; i < control.count; ++i)
          if (plane.eval(control.points[i].p) >= 0) {
            outside = false;
            break;
          }
        if (outside)
          return;
      }
      bool positive = true;
      for (std::size_t i = 0; i < control.count; ++i)
        if (camera.w(control.points[i].p) <= 0)
          positive = false;
      if (positive) {
        const Vec2 a = camera.screen(control.points[0].p),
                   b = camera.screen(control.points[control.count - 1].p),
                   delta = b - a;
        const double length =
            double(delta.x) * delta.x + double(delta.y) * delta.y;
        double error = 0;
        for (std::size_t i = 1; i + 1 < control.count; ++i) {
          const Vec2 q = camera.screen(control.points[i].p);
          const double t = length > 1e-20
                               ? std::clamp((double(q.x - a.x) * delta.x +
                                             double(q.y - a.y) * delta.y) /
                                                length,
                                            0.0, 1.0)
                               : 0;
          const double dx = q.x - a.x - t * delta.x,
                       dy = q.y - a.y - t * delta.y;
          error = std::max(error, dx * dx + dy * dy);
        }
        if (error <=
            double(config.curveTolerancePx) * config.curveTolerancePx) {
          line(control.points[0].p, control.points[control.count - 1].p);
          return;
        }
      }
      if (depth >= 24)
        throw std::length_error(
            "3D curve could not meet the screen-space tolerance");
      Controls left{}, right{}, work = control;
      left.count = right.count = control.count;
      left.points[0] = work.points[0];
      right.points[control.count - 1] = work.points[control.count - 1];
      for (std::size_t level = 1; level < control.count; ++level) {
        for (std::size_t i = 0; i < control.count - level; ++i) {
          const auto a = work.points[i], b = work.points[i + 1];
          const double w = a.w + b.w;
          work.points[i] = {{float((a.p.x * a.w + b.p.x * b.w) / w),
                             float((a.p.y * a.w + b.p.y * b.w) / w),
                             float((a.p.z * a.w + b.p.z * b.w) / w)},
                            w * .5};
        }
        left.points[level] = work.points[0];
        right.points[control.count - 1 - level] =
            work.points[control.count - 1 - level];
      }
      self(self, left, depth + 1);
      self(self, right, depth + 1);
    };
    for (const auto &source : object.curves) {
      Curve3D prepared;
      const Curve3D *curve = &source;
      if (curve->segments.empty()) {
        prepared = source;
        prepareCurve3D(prepared);
        curve = &prepared;
      }
      for (const auto &span : curve->segments) {
        Controls control{};
        control.count = span.control.size();
        if (control.count < 2 || control.count > 16)
          throw std::invalid_argument("Invalid prepared 3D curve span");
        for (std::size_t i = 0; i < control.count; ++i) {
          const auto &point = span.control[i];
          if (!finite(point.position) || !std::isfinite(point.weight) ||
              point.weight <= 0)
            throw std::invalid_argument("Invalid rational 3D curve control");
          control.points[i] = {
              camera.toCamera(transformPoint3D(world, point.position)),
              point.weight};
        }
        flatten(flatten, control, 0);
      }
    }
    emitPath(output, std::move(curves), object.style.edge.paint);
  }
  stats.visibleFragments = fragments.size() + transparent.size();
  stats.emittedCommands = output.commands().size();
  draw.append(std::move(output));
  return stats;
}

std::optional<PickResult3D> pickScene3D(Vec2 screen,
                                        std::span<const SceneObject3D> objects,
                                        const Mesh3DView &view) {
  const Camera camera(view);
  if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
    return std::nullopt;
  const auto root =
      composeTransform3D(view.parent, affineTransform3D(view.transform));
  const double u = screen.x - camera.origin.x, v = screen.y - camera.origin.y;
  const Vec3 origin{float(u * camera.a), float(v * camera.a), 0},
      direction{float(u * camera.b), float(v * camera.b), 1};
  std::optional<PickResult3D> result;
  double closest = camera.farZ;
  for (std::size_t oi = 0; oi < objects.size(); ++oi) {
    const auto &object = objects[oi];
    if (!object.mesh)
      continue;
    const auto world = composeTransform3D(
        root,
        composeTransform3D(object.parent, affineTransform3D(object.transform)));
    const double sign = dot(cross(world.x, world.y), world.z) < 0 ? -1 : 1;
    const auto &mesh = *object.mesh;
    for (std::size_t ti = 0; ti < mesh.triangles.size(); ++ti) {
      const auto t = mesh.triangles[ti];
      if (t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
          t.c >= mesh.vertices.size())
        continue;
      const Vec3 a = camera.toCamera(
                     transformPoint3D(world, mesh.vertices[t.a])),
                 b = camera.toCamera(
                     transformPoint3D(world, mesh.vertices[t.b])),
                 c = camera.toCamera(
                     transformPoint3D(world, mesh.vertices[t.c]));
      const Vec3 e1 = sub(b, a), e2 = sub(c, a), h = cross(direction, e2);
      const double determinant = dot(e1, h);
      if (std::abs(determinant) < 1e-12 ||
          (object.style.cullBackfaces && determinant * sign <= 0))
        continue;
      const Vec3 s = sub(origin, a);
      const double bx = dot(s, h) / determinant;
      if (bx < 0 || bx > 1)
        continue;
      const Vec3 q = cross(s, e1);
      const double by = dot(direction, q) / determinant;
      if (by < 0 || bx + by > 1)
        continue;
      const double depth = dot(e2, q) / determinant;
      if (depth < camera.nearZ || depth >= closest)
        continue;
      closest = depth;
      const Vec3 cp = add(origin, mul(direction, depth));
      const Vec3 normal = unit(mul(cross(e1, e2), sign));
      result = PickResult3D{
          object.id,
          oi,
          ti,
          camera.toWorld(cp),
          unit(add(add(mul(camera.x, normal.x), mul(camera.y, normal.y)),
                   mul(camera.z, normal.z))),
          {float(1 - bx - by), float(bx), float(by)},
          float(depth)};
    }
  }
  return result;
}

std::vector<MeshTriangle3D>
triangulateFace3D(std::span<const Vec3> vertices,
                  std::span<const std::uint32_t> input) {
  std::vector<std::uint32_t> face(input.begin(), input.end());
  if (face.size() > 3 && face.front() == face.back())
    face.pop_back();
  if (face.size() < 3)
    throw std::invalid_argument("A mesh polygon needs three vertices");
  for (auto id : face)
    if (id >= vertices.size() || !finite(vertices[id]))
      throw std::invalid_argument("Invalid polygon vertex");
  Vec3 normal{};
  for (std::size_t i = 0; i < face.size(); ++i) {
    if (face[i] >= vertices.size() || !finite(vertices[face[i]]))
      throw std::invalid_argument("Invalid polygon vertex");
    const auto a = vertices[face[i]], b = vertices[face[(i + 1) % face.size()]];
    normal = add(normal,
                 cross(sub(a, vertices[face[0]]), sub(b, vertices[face[0]])));
  }
  normal = unit(normal);
  if (!finite(normal) || dot(normal, normal) < .5)
    throw std::invalid_argument("Degenerate polygon");
  const int axis = std::abs(normal.x) > std::abs(normal.y)
                       ? (std::abs(normal.x) > std::abs(normal.z) ? 0 : 2)
                       : (std::abs(normal.y) > std::abs(normal.z) ? 1 : 2);
  Polygon points;
  for (auto id : face) {
    const auto p = vertices[id];
    points.push_back(axis == 0   ? Vec2{p.y, p.z}
                     : axis == 1 ? Vec2{p.x, p.z}
                                 : Vec2{p.x, p.y});
  }
  const double sign = signedArea(points) >= 0 ? 1 : -1;
  std::vector<MeshTriangle3D> result;
  result.reserve(face.size() - 2);
  std::vector<std::size_t> indices(face.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::size_t tests = 0;
  while (indices.size() > 3) {
    bool found = false;
    for (std::size_t i = 0; i < indices.size(); ++i) {
      const auto a = indices[(i + indices.size() - 1) % indices.size()],
                 b = indices[i], c = indices[(i + 1) % indices.size()];
      if (sign * area(points[b] - points[a], points[c] - points[b]) <= 1e-12)
        continue;
      bool contains = false;
      for (auto j : indices) {
        if (++tests > 16000000)
          throw std::length_error("Polygon triangulation budget exceeded");
        if (j == a || j == b || j == c)
          continue;
        if (sign * area(points[b] - points[a], points[j] - points[a]) >=
                -1e-10 &&
            sign * area(points[c] - points[b], points[j] - points[b]) >=
                -1e-10 &&
            sign * area(points[a] - points[c], points[j] - points[c]) >=
                -1e-10) {
          contains = true;
          break;
        }
      }
      if (contains)
        continue;
      result.push_back({face[a], face[b], face[c]});
      indices.erase(indices.begin() + i);
      found = true;
      break;
    }
    if (!found)
      throw std::invalid_argument("Self-intersecting or degenerate polygon");
  }
  result.push_back({face[indices[0]], face[indices[1]], face[indices[2]]});
  return result;
}
} // namespace slugia
