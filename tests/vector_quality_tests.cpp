#include "slugia/slugia.hpp"
#include <slughorn/slughorn.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace slugia;
namespace {
void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
// Independent double-precision ray intersection. No bands, atlas texture reads,
// shader root classification, or renderer coverage code is used here.
bool inside(const slughorn::Atlas::Shape &shape, double x, double y) {
  int winding = 0;
  for (const auto &c : shape.curves) {
    const double a = double(c.y1) - 2.0 * c.y2 + c.y3;
    const double b = 2.0 * (double(c.y2) - c.y1);
    const double extremum = a != 0 ? -b / (2 * a) : -1;
    const std::array<double, 3> ends{
        0, extremum > 0 && extremum < 1 ? extremum : 1, 1};
    const auto value = [&](double t) {
      if (t == 0)
        return double(c.y1);
      if (t == 1)
        return double(c.y3);
      const double s = 1 - t;
      return s * s * c.y1 + 2 * s * t * c.y2 + t * t * c.y3;
    };
    for (int j = 0; j < 2; ++j) {
      const double lo = ends[j], hi = ends[j + 1], y0 = value(lo),
                   y1 = value(hi);
      if (!((y0 <= y && y < y1) || (y1 <= y && y < y0)))
        continue;
      double low = lo, high = hi;
      for (int k = 0; k < 48; ++k) {
        const double mid = (low + high) * .5;
        if ((value(mid) < y) == (y1 > y0))
          low = mid;
        else
          high = mid;
      }
      const double t = (low + high) * .5, s = 1 - t;
      const double px = s * s * c.x1 + 2 * s * t * c.x2 + t * t * c.x3;
      if (px > x)
        winding += y1 > y0 ? 1 : -1;
    }
  }
  return shape.evenOdd ? (std::abs(winding) & 1) != 0 : winding != 0;
}
void save(const FramePixels &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (std::size_t i = 0; i < image.rgba.size(); i += 4)
    out.write(reinterpret_cast<const char *>(image.rgba.data() + i), 3);
}
struct Sample {
  ShapeId id;
  Rect bounds;
  std::string label;
};
struct Point {
  double x, y;
};
using Edge = std::array<Point, 2>;
double distanceSquared(Point p, Edge e) {
  const double x = e[1].x - e[0].x, y = e[1].y - e[0].y;
  const double length = x * x + y * y;
  const double t =
      length > 0
          ? std::clamp(((p.x - e[0].x) * x + (p.y - e[0].y) * y) / length, 0.0,
                       1.0)
          : 0;
  return std::pow(p.x - e[0].x - t * x, 2) + std::pow(p.y - e[0].y - t * y, 2);
}
void flatten(std::vector<Edge> &edges, Point a, Point b, Point c,
             int depth = 0) {
  if (depth >= 20 || distanceSquared(b, {a, c}) < .0025) {
    edges.push_back({a, c});
    return;
  }
  const auto mix = [](Point p, Point q) {
    return Point{(p.x + q.x) * .5, (p.y + q.y) * .5};
  };
  const auto ab = mix(a, b), bc = mix(b, c), mid = mix(ab, bc);
  flatten(edges, a, ab, mid, depth + 1);
  flatten(edges, mid, bc, c, depth + 1);
}
void checkPixels(const FramePixels &image, const VectorAtlas &atlas,
                 const std::vector<Sample> &samples) {
  std::size_t checked = 0, holes = 0, leaks = 0;
  std::string first;
  for (const auto &sample : samples) {
    const auto shape = atlas.native().getShape(slughorn::Key(sample.id));
    require(shape.has_value(), "Missing reference outline");
    const auto toScreen = [&](double x, double y) {
      return Point{sample.bounds.x + (x - shape->bearingX) / shape->width *
                                         sample.bounds.width,
                   sample.bounds.y + (shape->bearingY - y) / shape->height *
                                         sample.bounds.height};
    };
    std::vector<Edge> edges;
    for (const auto &c : shape->curves)
      flatten(edges, toScreen(c.x1, c.y1), toScreen(c.x2, c.y2),
              toScreen(c.x3, c.y3));
    const auto at = [&](double px, double py) {
      const double x = shape->bearingX + (px - sample.bounds.x) /
                                             sample.bounds.width * shape->width;
      const double y = shape->bearingY - (py - sample.bounds.y) /
                                             sample.bounds.height *
                                             shape->height;
      return inside(*shape, x, y);
    };
    const int x0 = std::max(0, int(std::floor(sample.bounds.x)));
    const int y0 = std::max(0, int(std::floor(sample.bounds.y)));
    const int x1 =
        std::min(int(image.width),
                 int(std::ceil(sample.bounds.x + sample.bounds.width)));
    const int y1 =
        std::min(int(image.height),
                 int(std::ceil(sample.bounds.y + sample.bounds.height)));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const bool expected = at(x + .5, y + .5);
        const Point point{x + .5, y + .5};
        bool nearEdge = false;
        for (const auto &edge : edges)
          if (distanceSquared(point, edge) < 2.25) {
            nearEdge = true;
            break;
          }
        if (nearEdge)
          continue; // 1.5 px margin, with <= .05 px flattening error.
        const auto pixel = image.rgba[(std::size_t(y) * image.width + x) * 4];
        ++checked;
        // Binary interior/exterior classification. Edge color accuracy is
        // tested separately; this gate catches missing spans and leaking filled
        // regions.
        if ((expected && pixel < 128) || (!expected && pixel > 128)) {
          if (holes + leaks < 5)
            std::cerr << "mismatch " << sample.label << " bounds "
                      << sample.bounds.x << ',' << sample.bounds.y << ','
                      << sample.bounds.width << " at " << x << ',' << y
                      << " expected " << expected << " pixel " << int(pixel)
                      << '\n';
          if (expected)
            ++holes;
          else
            ++leaks;
          if (first.empty() || expected)
            first = sample.label + " at " + std::to_string(x) + "," +
                    std::to_string(y) + " pixel=" + std::to_string(pixel);
        }
      }
  }
  if (holes || leaks)
    save(image, "vector-quality-failure.ppm");
  require(holes == 0 && leaks == 0,
          "Outline mismatch: holes=" + std::to_string(holes) +
              " leaks=" + std::to_string(leaks) + " / " +
              std::to_string(checked) + "; first=" + first);
}
} // namespace

int main(int argc, char **argv) try {
  const int zoomExponent = argc > 1 ? std::stoi(argv[1]) : 40;
  const std::string_view mode = argc > 2 ? argv[2] : "";
  const bool indexed = mode == "--indexed";
  const bool researchProfile = mode == "--research-profile";
  const bool packedBands = mode == "--packed-bands" || researchProfile;
  const bool compact = mode == "--compact-curves" || packedBands;
  const bool storage = indexed || compact ||
      mode == "--atlas-storage" || mode == "--atlas-storage-low-spec";
  require(mode.empty() || storage, "Unknown quality renderer mode");
  require(zoomExponent >= 0 && zoomExponent <= 60,
          "Zoom exponent must be 0..60");
  VectorAtlas atlas;
  for (auto weight : {400, 700, 900})
    require(
        atlas.loadFont(SLUGIA_QUALITY_FONT,
                       {"quality", std::uint16_t(weight), false, "Regular"}),
        "Load quality test font");
  std::vector<ShapeId> stress;
  // Connected outlines force both curve-tail and band-index reads across
  // texture rows.
  for (int i = 0; i < 180; ++i) {
    Path p;
    p.ellipse(0, 0, 1.0f + float(i) * .0001f, .8f);
    stress.push_back(atlas.addPath(p));
  }
  std::vector<std::pair<ShapeId, std::string>> rootRegressions;
  // A quadratic's control hull crosses rays that its actual curve never
  // reaches. The two candidate roots then have a negative discriminant and
  // must cancel, including when using the stable Vieta solve. Rotate the same
  // closed contour to exercise both axes and both root orders.
  for (int rotation = 0; rotation < 4; ++rotation) {
    const auto rotate = [&](float x, float y) {
      for (int i = 0; i < rotation; ++i) {
        const float oldX = x;
        x = -y;
        y = oldX;
      }
      return Vec2{x, y};
    };
    const auto a = rotate(0, 0), b = rotate(1, 2), c = rotate(0, 4),
               d = rotate(-1, 4), e = rotate(-1, 0);
    Path p;
    p.moveTo(a.x, a.y)
        .quadraticTo(b.x, b.y, c.x, c.y)
        .lineTo(d.x, d.y)
        .lineTo(e.x, e.y)
        .close();
    rootRegressions.emplace_back(atlas.addPath(p),
                                 "negative discriminant rotation=" +
                                     std::to_string(rotation));
  }
  atlas.build();
  Window window({512, 512, "Vector outline quality", false, false});
  RendererConfig config = researchProfile ? RendererConfig::vectorPerformance() : storage && mode != "--atlas-storage-low-spec"
                              ? RendererConfig{}
                              : RendererConfig::lowSpec();
  config.enableAtlasStoragePaths = storage;
  config.enableIndexedVectorQuads = indexed;
  config.enableCompactPersistentCurves = compact;
  config.enablePackedAtlasBands = packedBands;
  config.enableAtlasBandRefinement = storage;
  config.atlasBandRefinementCells = researchProfile ? 1024 : 512;
  config.enableReadback = true;
  config.validation = true;
  config.clearColor = Color::fromRgb8(0);
  VulkanRenderer renderer(window, atlas, config);
  require(!packedBands || renderer.stats().atlasPackedBandShapeCount != 0,
          "Packed glyph diagnostic must exercise packed band lists");
  require(renderer.stats().validationEnabled,
          "Quality tests require Vulkan validation");
  require(!storage || renderer.stats().atlasStorageShapeCount != 0,
          "Storage quality must exercise converted vector outlines");
  std::vector<std::pair<ShapeId, std::string>> shapes;
  for (auto weight : {400, 700, 900})
    for (int c = 33; c <= 126; ++c)
      shapes.emplace_back(atlas.glyph(c, "quality", std::uint16_t(weight)),
                          std::string(1, char(c)) +
                              " weight=" + std::to_string(weight));
  for (std::size_t i = 0; i < stress.size(); ++i)
    shapes.emplace_back(stress[i], "packed ellipse " + std::to_string(i));
  shapes.insert(shapes.end(), rootRegressions.begin(), rootRegressions.end());
  for (float size : {52.0f, 31.75f, 14.0f}) {
    for (std::size_t base = 0; base < shapes.size(); base += 64) {
      DrawList draw;
      std::vector<Sample> samples;
      for (std::size_t j = 0; j < 64 && base + j < shapes.size(); ++j) {
        const Rect bounds{float(j % 8) * 64 + 6.25f, float(j / 8) * 64 + 6.75f,
                          size, size};
        draw.shape(shapes[base + j].first, bounds,
                   Paint::solid(Color::fromRgb8(0xffffff)));
        samples.push_back(
            {shapes[base + j].first, bounds, shapes[base + j].second});
      }
      checkPixels(renderer.drawAndReadback(draw), atlas, samples);
    }
  }
  {
    const auto id = atlas.glyph('O', "quality", 400);
    const auto shape = atlas.native().getShape(slughorn::Key(id)).value();
    DrawList source;
    source.shape(id, {0, 0, 1, 1}, Paint::solid(Color::fromRgb8(0xffffff)));
    const auto retained = renderer.createRetainedDrawList(source);
    const auto &q = shape.curves.at(1);
    const double t = .37, s = 1 - t;
    const double px = s * s * q.x1 + 2 * s * t * q.x2 + t * t * q.x3;
    const double py = s * s * q.y1 + 2 * s * t * q.y2 + t * t * q.y3;
    const double spanX =
        float(shape.bearingX + shape.width) - double(shape.bearingX);
    const double spanY =
        float(shape.bearingY - shape.height) - double(shape.bearingY);
    const double focusX = (px - shape.bearingX) / spanX,
                 focusY = (py - shape.bearingY) / spanY;
    for (int exponent = -20; exponent <= zoomExponent; ++exponent) {
      View2D view{
          focusX, focusY, std::exp2(double(exponent)), {256.25f, 255.75f}};
      DrawList draw;
      draw.setClip({0, 0, 512, 512});
      draw.retainedDrawListView(retained, view);
      const auto pixels = renderer.drawAndReadback(draw);
      const auto reference = [&](double x, double y) {
        const double u = view.centerX + (x - view.origin.x) / view.zoom;
        const double v = view.centerY + (y - view.origin.y) / view.zoom;
        return inside(shape, shape.bearingX + u * spanX,
                      shape.bearingY + v * spanY);
      };
      std::size_t mismatch = 0, checked = 0;
      for (int y = 8; y < 512; y += 7)
        for (int x = 8; x < 512; x += 7) {
          const bool value = reference(x + .5, y + .5);
          bool stable = true;
          for (double dy : {-1.5, 0.0, 1.5})
            for (double dx : {-1.5, 0.0, 1.5})
              stable &= reference(x + .5 + dx, y + .5 + dy) == value;
          if (!stable)
            continue;
          ++checked;
          mismatch += (pixels.rgba[(std::size_t(y) * pixels.width + x) * 4] >
                       128) != value;
        }
      if (mismatch)
        save(pixels, "vector-zoom-failure.ppm");
      require(mismatch == 0, "Zoom 2^" + std::to_string(exponent) +
                                 " mismatch=" + std::to_string(mismatch) + "/" +
                                 std::to_string(checked));
      std::cout << "zoom 2^" << exponent << " passed\n";
    }
    renderer.destroyRetainedDrawList(retained);
  }
  require(renderer.stats().validationErrors == 0,
          "Quality Vulkan validation errors");
  std::cout << "Vector quality: " << shapes.size()
            << " outlines at 3 fractional sizes, no interior holes/leaks\n";
  std::cout << "Storage shapes=" << renderer.stats().atlasStorageShapeCount
            << " refined=" << renderer.stats().atlasRefinedShapeCount << '\n';
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
