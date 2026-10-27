#include "slugia/slugia.hpp"
#include "detail/geometry_cache.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace slugia;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class Error, class F>
void rejects(F&& operation, const char* message) {
  try { operation(); }
  catch (const Error&) { return; }
  throw std::runtime_error(message);
}

void cacheTests() {
  detail::GeometryCache<std::vector<int>> disabled(0, 16);
  std::vector<int> original{1, 2, 3};
  require(disabled.insert("key", std::move(original), 12) == nullptr && original.size() == 3,
          "disabled caches must not consume rejected values");

  detail::GeometryCache<int> cache(12, 2);
  cache.insert("a", 1, 4);
  cache.insert("b", 2, 4);
  require(cache.find("a") && *cache.find("a") == 1, "cache lookup");
  cache.insert("c", 3, 4);
  require(!cache.find("b") && cache.find("a") && cache.find("c"),
          "eviction must keep recently used geometry");
  require(cache.insert("oversized", 4, std::numeric_limits<std::size_t>::max()) == nullptr &&
          cache.size() == 2, "oversized cache entries must not flush useful geometry");

  detail::GeometryCache<std::vector<int>> churn(4096, 32);
  for (int i = 0; i < 10000; ++i) {
    std::vector<int> values(static_cast<std::size_t>(i % 80 + 1), i);
    const auto bytes = sizeof(values) + values.capacity() * sizeof(int);
    const auto key = std::to_string(i);
    auto* stored = churn.insert(key, std::move(values), bytes);
    require(stored && stored->front() == i && churn.bytes() <= 4096 && churn.size() <= 32,
            "changing geometry must remain bounded across sustained cache churn");
  }
}

void pathTests() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  rejects<std::invalid_argument>([&] { makeEllipseQuadratics({nan, 0}, {1, 1}); },
                                 "NaN coordinates must be rejected");
  rejects<std::invalid_argument>([&] { makeEllipseQuadratics({}, {1, 1}, nan); },
                                 "NaN tolerances must be rejected");
  rejects<std::invalid_argument>([&] { makeCubicStrokeQuadratics({}, {}, {}, {}, inf); },
                                 "infinite stroke widths must be rejected");
  rejects<std::invalid_argument>([&] { makeContinuousRoundedRectQuadratics({0,0,inf,1}, 1, 0); },
                                 "infinite extents must be rejected");
  require(makeEllipseQuadratics({}, {-1,1}).empty(), "negative extents remain a no-op");

  std::vector<QuadraticBezier2D> curves{{{0,0},{1,1},{2,0}}};
  rejects<std::invalid_argument>([&] { appendCubicQuadratics(curves, {}, {inf,0}, {}, {}); },
                                 "invalid cubic input must be rejected");
  require(curves.size() == 1, "failed appends must preserve existing contours");
  rejects<std::length_error>([&] {
    appendCubicQuadratics(curves, {0,0}, {1e8f,-1e8f}, {-1e8f,1e8f}, {1e8f,1e8f}, 1e-6f);
  }, "pathological adaptive subdivision must stop at its segment budget");
  require(curves.size() == 1, "subdivision exhaustion must roll back the append");

  curves.clear();
  appendCubicQuadratics(curves, {0,0}, {2,2}, {4,2}, {6,0});
  require(curves.size() == 1 && std::abs(curves[0].p1.y-3.0f) < 1e-5f,
          "degree-reduced quadratic cubics must remain exact");
}

#if SLUGIA_ENABLE_3D
void meshTests() {
  const auto huge = std::numeric_limits<std::uint32_t>::max();
  rejects<std::length_error>([&] { makeUvSphereMesh3D(1, huge, huge); },
                            "sphere allocation arithmetic must be checked before allocation");
  rejects<std::length_error>([&] { makeCylinderMesh3D(1, 1, huge); }, "cylinder budget");
  rejects<std::length_error>([&] { makeConeMesh3D(1, 1, huge); }, "cone budget");
  rejects<std::invalid_argument>([] { makeBoxMesh3D({1,-1,1}); }, "invalid box dimensions");
  rejects<std::invalid_argument>([] { makeOctahedronMesh3D(std::numeric_limits<float>::infinity()); },
                                 "non-finite primitive dimensions");

  const std::string vertices = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
  for (const char* face : {"f 0 2 3", "f 1 99 3", "f 1x 2 3", "f 1 2", "f /1 2 3",
                           "f -9223372036854775808 2 3", "f 99999999999999999999 2 3"}) {
    rejects<std::invalid_argument>([&] { parseObjMesh3D(vertices + face); },
                                   "malformed OBJ faces must not be partially imported");
  }
  const auto obj = parseObjMesh3D(vertices + "f +1/2/3 2//1 -1 # inline comment\r\n");
  require(obj.triangles.size() == 1 && obj.triangles[0].c == 2, "OBJ indices and comments");
  rejects<std::invalid_argument>([] { parseObjMesh3D("v broken 0 0\n"); }, "bad OBJ position");
  Mesh3DImportLimits limits;
  limits.maxVertices = 2;
  rejects<std::length_error>([&] { parseObjMesh3D(vertices, limits); }, "OBJ vertex budget");
  limits = {};
  limits.maxTriangles = 0;
  rejects<std::length_error>([&] { parseObjMesh3D(vertices + "f 1 2 3", limits); }, "OBJ triangle budget");
  limits = {};
  limits.maxFaceVertices = 2;
  rejects<std::length_error>([&] { parseObjMesh3D(vertices + "f 1 2 3", limits); }, "OBJ face budget");

  auto mesh = makeBoxMesh3D({1,1,1});
  const auto previousEdges = mesh.edges;
  mesh.triangles[0].a = huge;
  rejects<std::invalid_argument>([&] { prepareMesh3D(mesh); }, "invalid mesh topology");
  require(mesh.edges.size() == previousEdges.size() && mesh.edges.front().a == previousEdges.front().a,
          "failed topology preparation must preserve previous topology");
  mesh = makeBoxMesh3D({1,1,1});
  auto nonManifold = mesh;
  nonManifold.triangles.push_back(nonManifold.triangles.front());
  rejects<std::invalid_argument>([&] { prepareMesh3D(nonManifold); },
                                 "non-manifold edge topology must be rejected");
  require(std::is_sorted(mesh.edges.begin(), mesh.edges.end(), [](auto a, auto b) {
    return a.a < b.a || (a.a == b.a && a.b < b.b);
  }), "prepared edge traversal must be reproducible");
  mesh.edges[0].firstTriangle = std::numeric_limits<std::int32_t>::max();
  mesh.edges[0].secondTriangle = std::numeric_limits<std::int32_t>::max();
  DrawList list;
  drawMesh3D(list, mesh, {}, {});
  require(!list.commands().empty(), "stale public edge references must remain safe to draw");
}
#endif
} // namespace

int main() try {
  cacheTests();
  pathTests();
#if SLUGIA_ENABLE_3D
  meshTests();
#endif
  std::cout << "Geometry budgets, adaptive paths and mesh validation passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Geometry test failure: " << error.what() << '\n';
  return 1;
}
