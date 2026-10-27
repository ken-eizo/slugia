#pragma once
#include "slugia/draw_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slugia {

struct MeshTriangle3D {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::uint32_t c = 0;
};

struct MeshEdge3D {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  std::int32_t firstTriangle = -1;
  std::int32_t secondTriangle = -1;
  bool sharp = false;
};

struct Mesh3D {
  std::vector<Vec3> vertices;
  std::vector<MeshTriangle3D> triangles;
  // Prepared once at import/build time. Rendering then performs only transform,
  // visibility, projection and Slug 2D lowering.
  std::vector<MeshEdge3D> edges;
  std::vector<std::array<std::int32_t, 3>> triangleNeighbors;
  // Convex closed meshes need no per-edge occluder search: every front-facing
  // silhouette/crease is visible. This is the intended fast path for 3D UI
  // solids.
  bool convex = false;
  // Optional material for each triangle; takes precedence over
  // Mesh3DStyle::fill.
  std::vector<Paint> trianglePaints;
  std::vector<std::array<std::uint32_t, 2>> authoredSharpEdges;
};

struct Mesh3DStyle {
  Paint fill = Paint::solid(Color::fromRgb8(0x5265ba));
  StrokeStyle edge{};
  bool drawEdges = true;
  bool cullBackfaces = true;
  float creaseAngleRadians = 0.35f;
  bool shaded = false;
};
struct Mesh3DView {
  Vec2 origin{};
  Transform3D transform{};
  Camera3D camera{};
  AffineTransform3D parent{};
};

enum class CurveBasis3D { Bezier, Polyline, Nurbs };
struct CurvePoint3D {
  Vec3 position{}, incoming{}, outgoing{};
  float weight = 1.0f;
};
struct CurveBezier3D {
  std::vector<CurvePoint3D> control;
  float parameterStart = 0, parameterEnd = 1;
};
struct Curve3D {
  CurveBasis3D basis = CurveBasis3D::Bezier;
  std::vector<CurvePoint3D> points;
  std::vector<float> knots;
  std::uint32_t degree = 3;
  bool cyclic = false, endpoint = true, bezierKnots = false;
  // Prepared rational Bezier spans; source points/knots remain authoritative.
  std::vector<CurveBezier3D> segments;
};
void prepareCurve3D(Curve3D &curve);

struct SceneObject3D {
  const Mesh3D *mesh =
      nullptr; // borrowed only for the duration of drawScene3D/pickScene3D
  Transform3D transform{};
  Mesh3DStyle style{};
  std::uint64_t id = 0;
  AffineTransform3D parent{};
  std::span<const Curve3D> curves{};
};
struct Scene3DConfig {
  Vec3 lightDirection{-0.35f, -0.55f, -1.0f};
  float ambient = 0.28f;
  float diffuse = 0.72f;
  std::size_t maxFragments = 65536;
  std::size_t maxOcclusionTests = 4000000;
  float curveTolerancePx = 0.1f;
  std::size_t maxCurveSubdivisions = 65536;
};
struct Scene3DStats {
  std::size_t triangles = 0, clippedTriangles = 0, visibleFragments = 0;
  std::size_t occlusionTests = 0, emittedCommands = 0;
  std::size_t curveSegments = 0;
};
struct ProjectedPoint3D {
  Vec2 position{};
  float depth = 0, homogeneousW = 0;
  bool visible = false;
};
struct PickResult3D {
  std::uint64_t objectId = 0;
  std::size_t objectIndex = 0, triangleIndex = 0;
  Vec3 position{}, normal{}, barycentric{};
  float cameraDepth = 0;
};

AffineTransform3D affineTransform3D(const Transform3D &transform);
AffineTransform3D composeTransform3D(const AffineTransform3D &parent,
                                     const AffineTransform3D &local);
Vec3 transformPoint3D(const AffineTransform3D &transform, Vec3 point);
ProjectedPoint3D projectPoint3D(Vec3 point, const Mesh3DView &view);
ProjectiveTransform2D projectPlane3D(const Mesh3DView &view);
std::optional<Vec2> unprojectPlane3D(Vec2 screen, const Mesh3DView &view);
void drawPlane3D(DrawList &draw, const DrawList &content,
                 const Mesh3DView &view);
Scene3DStats drawScene3D(DrawList &draw, std::span<const SceneObject3D> objects,
                         const Mesh3DView &view = {},
                         const Scene3DConfig &config = {});
std::optional<PickResult3D> pickScene3D(Vec2 screen,
                                        std::span<const SceneObject3D> objects,
                                        const Mesh3DView &view = {});

// Dominant-plane ear clipping supports concave polygons and a deterministic
// triangle fallback for non-planar faces. Original 3D vertices stay unchanged.
std::vector<MeshTriangle3D>
triangulateFace3D(std::span<const Vec3> vertices,
                  std::span<const std::uint32_t> polygon);

// Guard allocations before processing untrusted assets or procedural
// resolutions.
struct Mesh3DImportLimits {
  std::size_t maxInputBytes = 64U << 20U;
  std::size_t maxVertices = 1U << 20U;
  std::size_t maxTriangles = 2U << 20U;
  std::size_t maxFaceVertices = 65536;
};
inline constexpr std::size_t generatedMesh3DVertexLimit = 1U << 20U;

struct VectorSolid3DStyle {
  Paint surface = Paint::solid(Color::fromRgb8(0x40518f));
  Paint cap = Paint::solid(Color::fromRgb8(0x667be0));
  StrokeStyle outline{Paint::solid(Color::fromRgb8(0xa8b6ff)), 1.0f};
};

void drawSphere3D(DrawList &draw, Vec2 origin, float radius, Vec3 rotation,
                  Camera3D camera, const VectorSolid3DStyle &style = {});
void drawCylinder3D(DrawList &draw, Vec2 origin, float radius, float halfHeight,
                    Vec3 rotation, Camera3D camera,
                    const VectorSolid3DStyle &style = {});
void drawCone3D(DrawList &draw, Vec2 origin, float radius, float halfHeight,
                Vec3 rotation, Camera3D camera,
                const VectorSolid3DStyle &style = {});

void drawMesh3D(DrawList &draw, const Mesh3D &mesh, const Mesh3DView &view,
                const Mesh3DStyle &style = {});

// Rejects non-finite vertices, invalid/repeated triangle indices and
// non-manifold edges; publishes topology only on success.
void prepareMesh3D(Mesh3D &mesh);
Mesh3D makeBoxMesh3D(Vec3 halfExtents);
Mesh3D makeOctahedronMesh3D(float radius);
Mesh3D makeUvSphereMesh3D(float radius, std::uint32_t slices = 48,
                          std::uint32_t stacks = 24);
Mesh3D makeCylinderMesh3D(float radius, float halfHeight,
                          std::uint32_t slices = 64);
Mesh3D makeConeMesh3D(float radius, float halfHeight,
                      std::uint32_t slices = 64);
// Position-only OBJ import; polygons use concave-aware ear clipping.
// Invalid records throw std::invalid_argument with a line
// number; budget violations throw std::length_error. Unknown OBJ record types
// are ignored.
Mesh3D parseObjMesh3D(std::string_view objText,
                      const Mesh3DImportLimits &limits = {});

struct AssetObject3D {
  std::string name;
  Mesh3D mesh;
  std::vector<Curve3D> curves;
  AffineTransform3D transform{};
  Mesh3DStyle style{};
  std::vector<std::vector<std::uint32_t>> polygons;
  std::vector<std::array<std::uint32_t, 2>> sharpEdges;
  float bevelDepth = 0, extrude = 0;
  bool useMeshFallback = false;
};
struct SceneAsset3D {
  std::vector<AssetObject3D> objects;
};
SceneAsset3D parseSlug3D(std::string_view json,
                         const Mesh3DImportLimits &limits = {});
SceneAsset3D loadScene3D(const std::string &path,
                         const Mesh3DImportLimits &limits = {});
Scene3DStats drawAsset3D(DrawList &draw, const SceneAsset3D &asset,
                         const Mesh3DView &view = {},
                         const Scene3DConfig &config = {});

} // namespace slugia
