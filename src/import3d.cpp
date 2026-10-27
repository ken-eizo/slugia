#include "slugia/vector3d.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <variant>

namespace slugia {
namespace {
struct Json {
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data =
      nullptr;
};
class Parser {
  std::string_view source;
  std::size_t position = 0, nodes = 0;
  [[noreturn]] void fail() const {
    throw std::invalid_argument("Invalid Slug3D JSON at byte " +
                                std::to_string(position));
  }
  void space() {
    while (position < source.size() &&
           (source[position] == ' ' || source[position] == '\t' ||
            source[position] == '\r' || source[position] == '\n'))
      ++position;
  }
  bool take(char c) {
    space();
    if (position < source.size() && source[position] == c) {
      ++position;
      return true;
    }
    return false;
  }
  unsigned hex() {
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) {
      if (position >= source.size())
        fail();
      const char c = source[position++];
      n <<= 4;
      if (c >= '0' && c <= '9')
        n += unsigned(c - '0');
      else if (c >= 'a' && c <= 'f')
        n += unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        n += unsigned(c - 'A' + 10);
      else
        fail();
    }
    return n;
  }
  static void utf8(std::string &s, unsigned c) {
    if (c < 128)
      s.push_back(char(c));
    else if (c < 2048) {
      s.push_back(char(192 | (c >> 6)));
      s.push_back(char(128 | (c & 63)));
    } else if (c < 65536) {
      s.push_back(char(224 | (c >> 12)));
      s.push_back(char(128 | ((c >> 6) & 63)));
      s.push_back(char(128 | (c & 63)));
    } else {
      s.push_back(char(240 | (c >> 18)));
      s.push_back(char(128 | ((c >> 12) & 63)));
      s.push_back(char(128 | ((c >> 6) & 63)));
      s.push_back(char(128 | (c & 63)));
    }
  }
  std::string string() {
    if (!take('"'))
      fail();
    std::string out;
    while (position < source.size()) {
      const unsigned char c = static_cast<unsigned char>(source[position++]);
      if (c == '"')
        return out;
      if (c < 32)
        fail();
      if (c != '\\') {
        out.push_back(char(c));
        continue;
      }
      if (position >= source.size())
        fail();
      const char e = source[position++];
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out.push_back(e);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        unsigned code = hex();
        if (code >= 0xd800 && code <= 0xdbff) {
          if (position + 2 > source.size() ||
              source.substr(position, 2) != "\\u")
            fail();
          position += 2;
          const unsigned low = hex();
          if (low < 0xdc00 || low > 0xdfff)
            fail();
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        } else if (code >= 0xdc00 && code <= 0xdfff)
          fail();
        utf8(out, code);
        break;
      }
      default:
        fail();
      }
    }
    fail();
  }
  Json value(unsigned depth) {
    space();
    if (depth > 64 || ++nodes > 2000000)
      throw std::length_error("Slug3D JSON complexity budget exceeded");
    if (position >= source.size())
      fail();
    if (source[position] == '"')
      return Json{string()};
    if (take('[')) {
      Json::Array a;
      if (!take(']')) {
        do {
          a.push_back(value(depth + 1));
        } while (take(','));
        if (!take(']'))
          fail();
      }
      return Json{std::move(a)};
    }
    if (take('{')) {
      Json::Object o;
      if (!take('}')) {
        do {
          const auto key = string();
          if (!take(':'))
            fail();
          if (!o.emplace(key, value(depth + 1)).second)
            fail();
        } while (take(','));
        if (!take('}'))
          fail();
      }
      return Json{std::move(o)};
    }
    for (const auto [word, type] : {std::pair<std::string_view, int>{"true", 1},
                                    {"false", 0},
                                    {"null", 2}})
      if (source.substr(position, word.size()) == word) {
        position += word.size();
        if (type == 2)
          return Json{};
        return Json{type == 1};
      }
    const auto start = position;
    if (source[position] == '-')
      ++position;
    if (position >= source.size())
      fail();
    if (source[position] == '0')
      ++position;
    else {
      if (source[position] < '1' || source[position] > '9')
        fail();
      while (position < source.size() && source[position] >= '0' &&
             source[position] <= '9')
        ++position;
    }
    if (position < source.size() && source[position] == '.') {
      ++position;
      const auto digits = position;
      while (position < source.size() && source[position] >= '0' &&
             source[position] <= '9')
        ++position;
      if (position == digits)
        fail();
    }
    if (position < source.size() &&
        (source[position] == 'e' || source[position] == 'E')) {
      ++position;
      if (position < source.size() &&
          (source[position] == '+' || source[position] == '-'))
        ++position;
      const auto digits = position;
      while (position < source.size() && source[position] >= '0' &&
             source[position] <= '9')
        ++position;
      if (position == digits)
        fail();
    }
    double number = 0;
    const auto parsed = std::from_chars(source.data() + start,
                                        source.data() + position, number);
    if (parsed.ec != std::errc{} || parsed.ptr != source.data() + position ||
        !std::isfinite(number))
      fail();
    return Json{number};
  }

public:
  explicit Parser(std::string_view input) : source(input) {}
  Json parse() {
    auto out = value(0);
    space();
    if (position != source.size())
      fail();
    return out;
  }
};
const Json *field(const Json &value, std::string_view key) {
  const auto *object = std::get_if<Json::Object>(&value.data);
  if (!object)
    throw std::invalid_argument("Slug3D expected an object");
  const auto found = object->find(key);
  return found == object->end() ? nullptr : &found->second;
}
const Json &required(const Json &value, std::string_view key) {
  const auto *result = field(value, key);
  if (!result)
    throw std::invalid_argument("Missing Slug3D field: " + std::string(key));
  return *result;
}
const Json::Array &array(const Json &value) {
  const auto *result = std::get_if<Json::Array>(&value.data);
  if (!result)
    throw std::invalid_argument("Slug3D expected an array");
  return *result;
}
double number(const Json &value) {
  const auto *result = std::get_if<double>(&value.data);
  if (!result)
    throw std::invalid_argument("Slug3D expected a number");
  return *result;
}
float scalar(const Json &value) {
  const auto n = number(value);
  const auto f = float(n);
  if (!std::isfinite(f))
    throw std::invalid_argument("Slug3D number exceeds float range");
  return f;
}
std::string text(const Json &value) {
  const auto *result = std::get_if<std::string>(&value.data);
  if (!result)
    throw std::invalid_argument("Slug3D expected a string");
  return *result;
}
std::string textField(const Json &value, std::string_view key,
                      std::string fallback = {}) {
  const auto *p = field(value, key);
  return p ? text(*p) : fallback;
}
bool boolean(const Json &value, std::string_view key, bool fallback) {
  const auto *p = field(value, key);
  if (!p)
    return fallback;
  const auto *b = std::get_if<bool>(&p->data);
  if (!b)
    throw std::invalid_argument("Slug3D expected a boolean");
  return *b;
}
float scalarField(const Json &value, std::string_view key, float fallback = 0) {
  const auto *p = field(value, key);
  return p ? scalar(*p) : fallback;
}
std::uint32_t index(const Json &value, std::size_t count) {
  const double n = number(value);
  if (n < 0 || n >= double(count) || n != std::floor(n) ||
      n > double(std::numeric_limits<std::uint32_t>::max()))
    throw std::invalid_argument("Slug3D index out of range");
  return std::uint32_t(n);
}
Vec3 vector3(const Json &value) {
  const auto &a = array(value);
  if (a.size() < 3)
    throw std::invalid_argument("Slug3D expected a 3D vector");
  return {scalar(a[0]), scalar(a[1]), scalar(a[2])};
}
Vec3 axes(Vec3 p, bool blender) { return blender ? Vec3{p.x, -p.z, p.y} : p; }
Color color(const Json &value) {
  const auto &a = array(value);
  if (a.size() != 3 && a.size() != 4)
    throw std::invalid_argument("Slug3D expected an RGB/RGBA color");
  return {scalar(a[0]), scalar(a[1]), scalar(a[2]),
          a.size() == 4 ? scalar(a[3]) : 1};
}
AffineTransform3D matrix(const Json &record, bool blender) {
  const auto *value = field(record, "matrix_world");
  if (!value)
    return {};
  const auto &rows = array(*value);
  if (rows.size() != 4)
    throw std::invalid_argument("Slug3D matrix must have four rows");
  std::array<std::array<float, 4>, 4> m{};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto &row = array(rows[i]);
    if (row.size() != 4)
      throw std::invalid_argument("Slug3D matrix must have four columns");
    for (std::size_t j = 0; j < 4; ++j)
      m[i][j] = scalar(row[j]);
  }
  if (std::abs(m[3][0]) + std::abs(m[3][1]) + std::abs(m[3][2]) +
          std::abs(m[3][3] - 1) >
      1e-6f)
    throw std::invalid_argument("Slug3D matrix must be affine");
  const Vec3 x{m[0][0], m[1][0], m[2][0]}, y{m[0][1], m[1][1], m[2][1]},
      z{m[0][2], m[1][2], m[2][2]};
  return {axes(x, blender), axes(blender ? Vec3{-z.x, -z.y, -z.z} : y, blender),
          axes(blender ? y : z, blender),
          axes({m[0][3], m[1][3], m[2][3]}, blender)};
}
} // namespace

SceneAsset3D parseSlug3D(std::string_view source,
                         const Mesh3DImportLimits &limits) {
  if (source.size() > limits.maxInputBytes)
    throw std::length_error("Slug3D input byte budget exceeded");
  const auto root = Parser(source).parse();
  if (text(required(root, "format")) != "slug3d" ||
      number(required(root, "version")) != 1)
    throw std::invalid_argument("Unsupported Slug3D format/version");
  const auto system =
      textField(root, "coordinate_system", "slug-y-down-z-forward-rh");
  const bool blender = system == "blender-z-up-rh";
  if (!blender && system != "slug-y-down-z-forward-rh")
    throw std::invalid_argument("Unsupported Slug3D coordinate system");
  const auto &objects = array(required(root, "objects"));
  if (objects.size() > 16384)
    throw std::length_error("Slug3D object budget exceeded");
  SceneAsset3D asset;
  asset.objects.reserve(objects.size());
  std::size_t vertices = 0, triangles = 0;
  for (const auto &record : objects) {
    AssetObject3D object;
    object.name = textField(record, "name");
    object.transform = matrix(record, blender);
    object.style.shaded = true;
    object.style.cullBackfaces = false;
    object.style.edge.paint = Paint::solid(Color::fromRgb8(0xb9d2ff));
    object.style.edge.width = 1;
    if (const auto *material = field(record, "color"))
      object.style.fill = Paint::solid(color(*material));
    const auto &geometry = required(record, "geometry");
    const auto kind = text(required(geometry, "kind"));
    if (kind == "curve")
      object.style.edge.paint = object.style.fill;
    const Json *meshGeometry =
        kind == "mesh" ? &geometry : field(geometry, "mesh_fallback");
    if (kind != "mesh" && kind != "curve")
      throw std::invalid_argument("Unsupported Slug3D geometry kind");
    object.bevelDepth = scalarField(geometry, "bevel_depth");
    object.extrude = scalarField(geometry, "extrude");
    object.useMeshFallback =
        kind == "curve" && (object.bevelDepth > 0 || object.extrude > 0 ||
                            boolean(geometry, "use_mesh_fallback", false));
    if (object.useMeshFallback && !meshGeometry)
      throw std::invalid_argument(
          "Slug3D beveled/extruded curves require mesh_fallback");
    if (meshGeometry) {
      const auto &points = array(required(*meshGeometry, "vertices"));
      if (points.size() >
          limits.maxVertices - std::min(vertices, limits.maxVertices))
        throw std::length_error("Slug3D vertex budget exceeded");
      vertices += points.size();
      object.mesh.vertices.reserve(points.size());
      for (const auto &p : points)
        object.mesh.vertices.push_back(axes(vector3(p), blender));
      std::vector<Paint> materials;
      if (const auto *list = field(*meshGeometry, "materials"))
        for (const auto &material : array(*list))
          materials.push_back(Paint::solid(color(material)));
      const Json::Array *assignments = nullptr;
      if (const auto *a = field(*meshGeometry, "face_materials"))
        assignments = &array(*a);
      const auto &faces = array(required(*meshGeometry, "faces"));
      if (assignments && assignments->size() != faces.size())
        throw std::invalid_argument(
            "Slug3D material assignment count mismatch");
      for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const auto &input = array(faces[fi]);
        if (input.size() > limits.maxFaceVertices)
          throw std::length_error("Slug3D polygon vertex budget exceeded");
        std::vector<std::uint32_t> polygon;
        polygon.reserve(input.size());
        for (const auto &i : input)
          polygon.push_back(index(i, object.mesh.vertices.size()));
        auto part = triangulateFace3D(object.mesh.vertices, polygon);
        if (part.size() >
            limits.maxTriangles - std::min(triangles, limits.maxTriangles))
          throw std::length_error("Slug3D triangle budget exceeded");
        triangles += part.size();
        if (!materials.empty()) {
          const auto material =
              assignments ? index((*assignments)[fi], materials.size()) : 0;
          object.mesh.trianglePaints.insert(object.mesh.trianglePaints.end(),
                                            part.size(), materials[material]);
        }
        object.mesh.triangles.insert(object.mesh.triangles.end(), part.begin(),
                                     part.end());
        object.polygons.push_back(std::move(polygon));
      }
      if (const auto *edges = field(*meshGeometry, "sharp_edges"))
        for (const auto &e : array(*edges)) {
          const auto &pair = array(e);
          if (pair.size() != 2)
            throw std::invalid_argument("Slug3D edge needs two indices");
          object.sharpEdges.push_back(
              {index(pair[0], object.mesh.vertices.size()),
               index(pair[1], object.mesh.vertices.size())});
        }
      object.mesh.authoredSharpEdges = object.sharpEdges;
      prepareMesh3D(object.mesh);
    }
    if (kind == "curve") {
      for (const auto &spline : array(required(geometry, "splines"))) {
        Curve3D curve;
        const auto basis = text(required(spline, "basis"));
        if (basis == "bezier")
          curve.basis = CurveBasis3D::Bezier;
        else if (basis == "poly")
          curve.basis = CurveBasis3D::Polyline;
        else if (basis == "nurbs")
          curve.basis = CurveBasis3D::Nurbs;
        else
          throw std::invalid_argument("Unsupported Slug3D spline basis");
        curve.cyclic = boolean(spline, "cyclic", false);
        curve.endpoint = boolean(spline, "endpoint", true);
        curve.bezierKnots = boolean(spline, "bezier_knots", false);
        const float degree = scalarField(spline, "degree", 3);
        if (degree < 1 || degree > 15 || degree != std::floor(degree))
          throw std::invalid_argument("Invalid Slug3D NURBS degree");
        curve.degree = std::uint32_t(degree);
        const auto &points = array(required(spline, "points"));
        if (points.size() >
            limits.maxVertices - std::min(vertices, limits.maxVertices))
          throw std::length_error("Slug3D curve point budget exceeded");
        vertices += points.size();
        for (const auto &p : points) {
          CurvePoint3D point;
          if (curve.basis == CurveBasis3D::Bezier) {
            point.position = axes(vector3(required(p, "p")), blender);
            point.incoming = axes(vector3(required(p, "in")), blender);
            point.outgoing = axes(vector3(required(p, "out")), blender);
          } else {
            const auto &values = array(p);
            point.position = axes(vector3(p), blender);
            if (values.size() > 3)
              point.weight = scalar(values[3]);
            point.incoming = point.outgoing = point.position;
          }
          curve.points.push_back(point);
        }
        if (const auto *knots = field(spline, "knots"))
          for (const auto &u : array(*knots))
            curve.knots.push_back(scalar(u));
        if (!object.useMeshFallback)
          prepareCurve3D(curve);
        object.curves.push_back(std::move(curve));
      }
    }
    asset.objects.push_back(std::move(object));
  }
  return asset;
}
SceneAsset3D loadScene3D(const std::string &path,
                         const Mesh3DImportLimits &limits) {
  const std::filesystem::path file(std::u8string(path.begin(), path.end()));
  std::error_code error;
  const auto size = std::filesystem::file_size(file, error);
  if (error)
    throw std::runtime_error("Cannot open 3D asset: " + path);
  if (size > limits.maxInputBytes)
    throw std::length_error("3D asset byte budget exceeded");
  std::ifstream input(file, std::ios::binary);
  std::string data(static_cast<std::size_t>(size), '\0');
  if (!input.read(data.data(), std::streamsize(data.size())))
    throw std::runtime_error("Cannot read 3D asset: " + path);
  auto extension = file.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (extension == ".slug3d")
    return parseSlug3D(data, limits);
  if (extension != ".obj")
    throw std::invalid_argument("3D assets must use .slug3d or .obj");
  SceneAsset3D asset;
  AssetObject3D object;
  object.name = file.stem().string();
  object.mesh = parseObjMesh3D(data, limits);
  object.style.shaded = true;
  object.style.cullBackfaces = false;
  object.style.edge.width = 1;
  object.style.edge.paint = Paint::solid(Color::fromRgb8(0xb9d2ff));
  asset.objects.push_back(std::move(object));
  return asset;
}
Scene3DStats drawAsset3D(DrawList &draw, const SceneAsset3D &asset,
                         const Mesh3DView &view, const Scene3DConfig &config) {
  std::vector<SceneObject3D> instances;
  instances.reserve(asset.objects.size());
  for (std::size_t i = 0; i < asset.objects.size(); ++i) {
    const auto &source = asset.objects[i];
    SceneObject3D object;
    object.id = i + 1;
    object.mesh = source.mesh.triangles.empty() ? nullptr : &source.mesh;
    object.parent = source.transform;
    object.style = source.style;
    if (!source.useMeshFallback)
      object.curves = source.curves;
    instances.push_back(object);
  }
  return drawScene3D(draw, instances, view, config);
}
} // namespace slugia
