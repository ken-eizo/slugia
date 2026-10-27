#include "slugia/slugia.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace slugia;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Error, class F> void rejects(F &&f, const char *message) {
  try {
    f();
  } catch (const Error &) {
    return;
  }
  throw std::runtime_error(message);
}
struct D {
  double x, y, z;
};
D sub(D a, D b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D cross(D a, D b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(D a, D b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D point(Vec3 p) { return {p.x, p.y, p.z}; }
struct Hit {
  double depth;
  Color color;
  std::uint64_t id;
};
// Independent ray/triangle reference. It does not use the renderer's
// projection, polygon clipping, depth planes, BVH, or visible-fragment
// implementation.
std::vector<Hit> ray(Vec2 sample, std::span<const SceneObject3D> objects,
                     const Mesh3DView &view) {
  const auto camera = view.camera;
  const double f =
      camera.focalLength > 0 ? camera.focalLength : std::abs(camera.position.z);
  const double x = sample.x - view.origin.x, y = sample.y - view.origin.y,
               p = camera.perspective;
  const D origin{x * (1 - p) + camera.position.x,
                 y * (1 - p) + camera.position.y, camera.position.z};
  const D direction{x * p / f, y * p / f, 1};
  std::vector<Hit> hits;
  for (const auto &object : objects) {
    if (!object.mesh)
      continue;
    const auto &mesh = *object.mesh;
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
      const auto triangle = mesh.triangles[i];
      const D a = point(mesh.vertices[triangle.a]),
              b = point(mesh.vertices[triangle.b]),
              c = point(mesh.vertices[triangle.c]);
      const D e1 = sub(b, a), e2 = sub(c, a), q = cross(direction, e2);
      const double determinant = dot(e1, q);
      if (std::abs(determinant) < 1e-12)
        continue;
      const D offset = sub(origin, a);
      const double u = dot(offset, q) / determinant;
      const D r = cross(offset, e1);
      const double v = dot(direction, r) / determinant,
                   t = dot(e2, r) / determinant;
      if (u < 0 || v < 0 || u + v > 1 || t < camera.nearPlane ||
          t > camera.farPlane || 1 + p * (t / f - 1) <= camera.nearPlane / f)
        continue;
      const auto paint = mesh.trianglePaints.empty() ? object.style.fill
                                                     : mesh.trianglePaints[i];
      auto color = paint.start;
      color.a *= paint.opacity;
      hits.push_back({t, color, object.id});
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](auto a, auto b) { return a.depth > b.depth; });
  return hits;
}
Color composite(std::span<const Hit> hits) {
  Color out{};
  out.a = 1;
  for (auto hit : hits) {
    const auto c = hit.color;
    out.r = c.r * c.a + out.r * (1 - c.a);
    out.g = c.g * c.a + out.g * (1 - c.a);
    out.b = c.b * c.a + out.b * (1 - c.a);
  }
  return out;
}
Color sample(const DrawList &draw, Vec2 p) {
  Color out{};
  out.a = 1;
  for (const auto &command : draw.commands()) {
    const auto *path = std::get_if<DynamicVectorPathCommand>(&command);
    if (!path)
      continue;
    int winding = 0;
    for (const auto &curve : path->curves) {
      const auto a = curve.p0, b = curve.p2;
      const double side =
          double(b.x - a.x) * (p.y - a.y) - double(b.y - a.y) * (p.x - a.x);
      if (a.y <= p.y && b.y > p.y && side > 0)
        ++winding;
      else if (b.y <= p.y && a.y > p.y && side < 0)
        --winding;
    }
    if (winding) {
      auto c = path->paint.start;
      c.a *= path->paint.opacity * path->opacity;
      const float destination = path->additiveCoverage ? 1.0f : 1 - c.a;
      out.r = c.r * c.a + out.r * destination;
      out.g = c.g * c.a + out.g * destination;
      out.b = c.b * c.a + out.b * destination;
    }
  }
  return out;
}
void compare(std::span<const SceneObject3D> objects, Mesh3DView view) {
  DrawList draw;
  draw.setClip({0, 0, 64, 64});
  const auto stats = drawScene3D(draw, objects, view);
  require(stats.triangles > 0 && stats.emittedCommands > 0,
          "3D scene must emit visible vector geometry");
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const Vec2 p{float(x) + .213f, float(y) + .371f};
      const auto expected = composite(ray(p, objects, view)),
                 actual = sample(draw, p);
      if (std::abs(expected.r - actual.r) > 1e-4f ||
          std::abs(expected.g - actual.g) > 1e-4f ||
          std::abs(expected.b - actual.b) > 1e-4f) {
        std::cerr << "Mismatch at " << p.x << "," << p.y
                  << " perspective=" << view.camera.perspective << " expected "
                  << expected.r << "," << expected.g << "," << expected.b
                  << " actual " << actual.r << "," << actual.g << ","
                  << actual.b << '\n';
        throw std::runtime_error(
            "3D visibility disagrees with the ray reference");
      }
    }
}
void scenes() {
  Mesh3D a, b;
  a.vertices = {{-30, -25, -70}, {30, -25, 70}, {0, 30, 0}};
  a.triangles = {{0, 1, 2}};
  b.vertices = {{-30, -25, 70}, {30, -25, -70}, {0, 30, 0}};
  b.triangles = {{0, 1, 2}};
  prepareMesh3D(a);
  prepareMesh3D(b);
  std::array<SceneObject3D, 2> objects{};
  objects[0].mesh = &a;
  objects[1].mesh = &b;
  objects[0].id = 11;
  objects[1].id = 22;
  for (auto &object : objects) {
    object.style.drawEdges = false;
    object.style.cullBackfaces = false;
  }
  objects[0].style.fill = Paint::solid(Color::fromRgb8(0xff3311));
  objects[1].style.fill = Paint::solid(Color::fromRgb8(0x1166ff));
  Mesh3DView view;
  view.origin = {32, 32};
  for (float p : {0.0f, 0.3f, 1.0f, 2.0f}) {
    view.camera.perspective = p;
    compare(objects, view);
    std::swap(objects[0], objects[1]);
    compare(objects, view);
  }
  for (auto &object : objects)
    object.style.fill.opacity = .45f;
  view.camera.perspective = 1;
  compare(objects, view);
  std::swap(objects[0], objects[1]);
  compare(objects, view);
  for (auto &object : objects)
    object.style.fill.opacity = 1;
  a.vertices = {{-1, -1, -1.8f}, {1, -1, 1}, {0, 1, 1}};
  b.vertices = {{-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
  prepareMesh3D(a);
  prepareMesh3D(b);
  view.camera.position = {0, 0, -2};
  view.camera.focalLength = 50;
  view.camera.nearPlane = .5f;
  view.camera.farPlane = 5;
  compare(objects, view);
  const auto selected = pickScene3D({32, 32}, objects, view);
  require(selected && selected->objectId == 11,
          "picking must select the nearest visible surface");
  DrawList limited;
  limited.roundedRect({0, 0, 1, 1}, 0, Paint::solid({1, 1, 1, 1}));
  Scene3DConfig budget;
  budget.maxFragments = 0;
  rejects<std::length_error>(
      [&] { drawScene3D(limited, objects, view, budget); },
      "scene budgets must be enforced");
  require(limited.commands().size() == 1,
          "failed scenes must preserve the caller's commands");
}
void cameraAndPlanes() {
  {
    auto cube = makeBoxMesh3D({22, 22, 22});
    cube.convex = false;
    Transform3D rotation;
    rotation.rotation = {-.31f, .42f, 0};
    const auto matrix = affineTransform3D(rotation);
    for (auto &vertex : cube.vertices)
      vertex = transformPoint3D(matrix, vertex);
    for (std::size_t i = 0; i < cube.triangles.size(); ++i)
      cube.trianglePaints.push_back(
          Paint::solid(Color::fromRgb8(i / 2 % 2 ? 0xff2255 : 0x3366ff)));
    prepareMesh3D(cube);
    SceneObject3D object;
    object.mesh = &cube;
    object.style.cullBackfaces = false;
    object.style.drawEdges = false;
    Mesh3DView camera;
    camera.origin = {32, 32};
    camera.camera.position = {0, 0, -100};
    camera.camera.focalLength = 100;
    compare(std::span<const SceneObject3D>(&object, 1), camera);
  }
  Mesh3DView view;
  view.origin = {100, 100};
  const Vec3 p{80, 30, 200};
  view.camera.perspective = 0;
  const auto ortho = projectPoint3D(p, view);
  view.camera.perspective = .5f;
  const auto half = projectPoint3D(p, view);
  view.camera.perspective = 1;
  const auto full = projectPoint3D(p, view);
  require(ortho.position.x > half.position.x &&
              half.position.x > full.position.x,
          "perspective must interpolate continuously");
  view.transform.rotation = {.31f, .65f, -.22f};
  view.transform.translation = {10, -20, 40};
  view.transform.scale = {1.2f, .8f, 1};
  for (Vec2 local : std::array<Vec2, 3>{{{0, 0}, {40, 30}, {-20, 15}}}) {
    const auto screen = projectPoint3D({local.x, local.y, 0}, view);
    const auto back = unprojectPlane3D(screen.position, view);
    require(back && std::hypot(back->x - local.x, back->y - local.y) < .001f,
            "3D plane pointer mapping must invert projection");
  }
  view.camera.rotation = {0, .3f, 0};
  const auto front = projectPoint3D({}, view);
  require(front.visible, "rotated cameras must retain visible content");
  view.camera.nearPlane = -1;
  rejects<std::invalid_argument>([&] { projectPoint3D({}, view); },
                                 "invalid camera ranges");
}
std::array<double, 4> eval(const CurveBezier3D &span, double t) {
  std::array<std::array<double, 4>, 16> p{};
  const auto count = span.control.size();
  for (std::size_t i = 0; i < count; ++i) {
    const auto q = span.control[i];
    p[i] = {q.position.x * q.weight, q.position.y * q.weight,
            q.position.z * q.weight, q.weight};
  }
  for (std::size_t level = 1; level < count; ++level)
    for (std::size_t i = 0; i < count - level; ++i)
      for (int j = 0; j < 4; ++j)
        p[i][j] = p[i][j] * (1 - t) + p[i + 1][j] * t;
  return {p[0][0] / p[0][3], p[0][1] / p[0][3], p[0][2] / p[0][3], p[0][3]};
}
void curvesAndImport() {
  Curve3D circle;
  circle.basis = CurveBasis3D::Nurbs;
  circle.degree = 2;
  circle.points = {{{60, 0, 0}, {}, {}, 1},
                   {{60, 60, 0}, {}, {}, float(std::sqrt(.5))},
                   {{0, 60, 0}, {}, {}, 1}};
  prepareCurve3D(circle);
  require(circle.segments.size() == 1,
          "a quadratic NURBS span must stay quadratic");
  const auto middle = eval(circle.segments[0], .5);
  require(std::abs(middle[0] - 60 * std::sqrt(.5)) < 1e-5 &&
              std::abs(middle[1] - middle[0]) < 1e-5,
          "rational NURBS weights must preserve the conic");
  Curve3D uniform;
  uniform.basis = CurveBasis3D::Nurbs;
  uniform.degree = 2;
  uniform.endpoint = false;
  for (int i = 0; i < 5; ++i)
    uniform.points.push_back({{float(i), float(i * i), 0}, {}, {}, 1});
  prepareCurve3D(uniform);
  require(uniform.segments.size() == 3, "non-clamped NURBS span count");
  const auto first = eval(uniform.segments.front(), 0),
             last = eval(uniform.segments.back(), 1);
  require(std::abs(first[0] - .5) < 1e-6 && std::abs(last[0] - 3.5) < 1e-6 &&
              std::abs(last[1] - 12.5) < 1e-6,
          "non-clamped NURBS endpoints must match the B-spline basis");
  uniform.cyclic = true;
  prepareCurve3D(uniform);
  const auto begin = eval(uniform.segments.front(), 0),
             end = eval(uniform.segments.back(), 1);
  require(std::hypot(begin[0] - end[0], begin[1] - end[1]) < 1e-5,
          "cyclic NURBS must close without losing control data");
  SceneObject3D curveObject;
  curveObject.curves = std::span<const Curve3D>(&circle, 1);
  curveObject.style.edge.width = 2;
  curveObject.style.edge.paint = Paint::solid({1, 1, 1, 1});
  DrawList draw;
  draw.setClip({0, 0, 128, 128});
  Mesh3DView view;
  view.origin = {20, 20};
  const auto stats =
      drawScene3D(draw, std::span<const SceneObject3D>(&curveObject, 1), view);
  require(stats.curveSegments > 8 && !draw.commands().empty(),
          "native NURBS must render as adaptive vector strokes");
  const auto concave = parseObjMesh3D(
      "v 0 0 0\nv 3 0 0\nv 3 1 0\nv 1 1 0\nv 1 3 0\nv 0 3 0\nf 1 2 3 4 5 6\n");
  double area = 0;
  for (auto t : concave.triangles) {
    const auto a = concave.vertices[t.a], b = concave.vertices[t.b],
               c = concave.vertices[t.c];
    area += std::abs(double(b.x - a.x) * (c.y - a.y) -
                     double(b.y - a.y) * (c.x - a.x)) *
            .5;
  }
  require(concave.triangles.size() == 4 && std::abs(area - 5) < 1e-6,
          "concave OBJ polygons must not fill their notch");
  const auto asset = parseSlug3D(
      R"({"format":"slug3d","version":1,"coordinate_system":"blender-z-up-rh","objects":[{"name":"\u7403","matrix_world":[[1,0,0,2],[0,1,0,3],[0,0,1,4],[0,0,0,1]],"geometry":{"kind":"mesh","vertices":[[0,0,0],[1,0,0],[0,1,0]],"faces":[[0,1,2]],"materials":[[1,0,0,1]],"face_materials":[0],"sharp_edges":[[0,1]]}}]})");
  require(asset.objects.size() == 1 &&
              asset.objects[0].mesh.triangles.size() == 1 &&
              asset.objects[0].polygons.size() == 1 &&
              asset.objects[0].transform.translation.y == -4 &&
              asset.objects[0].transform.translation.z == 3 &&
              asset.objects[0].mesh.trianglePaints[0].start.r == 1 &&
              asset.objects[0].mesh.edges.front().sharp,
          "Slug3D must import transforms, coordinate axes, polygons, materials "
          "and authored edges");
  rejects<std::invalid_argument>(
      [] { parseSlug3D(R"({"format":"slug3d","version":2,"objects":[]})"); },
      "Slug3D version checking");
  rejects<std::invalid_argument>(
      [] {
        parseSlug3D(
            R"({"format":"slug3d","format":"other","version":1,"objects":[]})");
      },
      "duplicate JSON fields");
  Mesh3DImportLimits limits;
  limits.maxVertices = 2;
  rejects<std::length_error>(
      [&] {
        parseSlug3D(
            R"({"format":"slug3d","version":1,"objects":[{"geometry":{"kind":"mesh","vertices":[[0,0,0],[1,0,0],[0,1,0]],"faces":[[0,1,2]]}}]})",
            limits);
      },
      "Slug3D allocation limits");
}
} // namespace
int main() try {
  scenes();
  cameraAndPlanes();
  curvesAndImport();
  std::cout << "3D ray-reference visibility, transparency, clipping, picking, "
               "planes, NURBS and imports passed\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << "3D test failure: " << e.what() << '\n';
  return 1;
}
