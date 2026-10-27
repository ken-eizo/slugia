#include "slugia/slugia.hpp"
#include <algorithm>
#include <chrono>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <slughorn/slughorn.hpp>
#include <stdexcept>
#include <vector>

using namespace slugia;
namespace {
struct Point {
  double x, y;
};
using Edge = std::array<Point, 2>;
double distanceSquared(Point p, Edge e) {
  const double x = e[1].x - e[0].x, y = e[1].y - e[0].y, length = x * x + y * y;
  const double t =
      length > 0
          ? std::clamp(((p.x - e[0].x) * x + (p.y - e[0].y) * y) / length, 0.0,
                       1.0)
          : 0;
  const double dx = p.x - e[0].x - t * x, dy = p.y - e[0].y - t * y;
  return dx * dx + dy * dy;
}
void flatten(std::vector<Edge> &edges, Point a, Point b, Point c,
             int depth = 0) {
  if (depth >= 20 || distanceSquared(b, {a, c}) < .0025) {
    edges.push_back({a, c});
    return;
  }
  const Point ab{(a.x + b.x) * .5, (a.y + b.y) * .5},
      bc{(b.x + c.x) * .5, (b.y + c.y) * .5};
  const Point mid{(ab.x + bc.x) * .5, (ab.y + bc.y) * .5};
  flatten(edges, a, ab, mid, depth + 1);
  flatten(edges, mid, bc, c, depth + 1);
}
// Independent monotonic-interval winding, without atlas bands or shader roots.
bool inside(const slughorn::Atlas::Shape &shape, double x, double y) {
  int winding = 0;
  for (const auto &q : shape.curves) {
    const double a = double(q.y1) - 2.0 * q.y2 + q.y3;
    const double b = 2.0 * (double(q.y2) - q.y1);
    const double cut = a != 0 ? -b / (2 * a) : -1;
    const double stops[]{0, cut > 0 && cut < 1 ? cut : 1, 1};
    const auto value = [&](double t) {
      if (t == 0)
        return double(q.y1);
      if (t == 1)
        return double(q.y3);
      const double s = 1 - t;
      return s * s * q.y1 + 2 * s * t * q.y2 + t * t * q.y3;
    };
    for (int j = 0; j < 2; ++j) {
      double lo = stops[j], hi = stops[j + 1];
      const double y0 = value(lo), y1 = value(hi);
      if (!((y0 <= y && y < y1) || (y1 <= y && y < y0)))
        continue;
      for (int k = 0; k < 48; ++k) {
        const double mid = (lo + hi) * .5;
        if ((value(mid) < y) == (y1 > y0))
          lo = mid;
        else
          hi = mid;
      }
      const double t = (lo + hi) * .5, s = 1 - t;
      if (s * s * q.x1 + 2 * s * t * q.x2 + t * t * q.x3 > x)
        winding += y1 > y0 ? 1 : -1;
    }
  }
  return shape.evenOdd ? (std::abs(winding) & 1) != 0 : winding != 0;
}
void save(const FramePixels &pixels, const char *name) {
  std::ofstream out(name, std::ios::binary);
  out << "P6\n" << pixels.width << ' ' << pixels.height << "\n255\n";
  for (std::size_t i = 0; i < pixels.rgba.size(); i += 4)
    out.write(reinterpret_cast<const char *>(pixels.rgba.data() + i), 3);
}
} // namespace
// Diagnostic gate for the disabled tile prototype. Compare every source path
// separately so compositing cannot hide a missing tile. The normal atlas image
// is checked against independent winding below at differing pixels as well.
int main(int argc, char **argv) try {
  const std::string_view mode = argc > 1 ? argv[1] : "";
  const bool indexed = mode == "--indexed";
  const bool researchProfile = mode == "--research-profile" || mode == "--research-profile-low-spec";
  const bool extraFineBands = mode == "--packed-bands2048" || mode == "--packed-bands2048-low-spec";
  const bool packedBands = mode == "--packed-bands" || mode == "--packed-bands-low-spec" || extraFineBands || researchProfile;
  const bool compact = mode == "--compact-curves" || mode == "--compact-curves-low-spec" || packedBands;
  const bool expandedBands = mode == "--bands1024" || mode == "--bands1024-low-spec" || compact;
  const bool finestBands =
      mode == "--bands512" || mode == "--bands512-low-spec" || indexed || compact;
  const bool fineBands = mode == "--bands256" || mode == "--bands256-low-spec";
  const bool lowSpec = mode == "--bands-low-spec" ||
                       mode == "--bands256-low-spec" ||
                       mode == "--bands512-low-spec" || mode == "--bands1024-low-spec" || mode == "--compact-curves-low-spec" || mode == "--packed-bands-low-spec" || mode == "--packed-bands2048-low-spec" || mode == "--research-profile-low-spec";
  const bool bands = lowSpec || fineBands || finestBands || expandedBands || mode == "--bands";
  const bool storage =
      bands || (argc > 1 && std::string_view(argv[1]) == "--storage");
  auto svg = SvgDocument::load(SLUGIA_TEST_TIGER);
  VectorAtlas atlas;
  svg.registerShapes(atlas);
  atlas.build();
  auto scene = svg.drawList(atlas);
  Window baseWindow({768, 768, "SVG baseline", false, false});
  Window tileWindow({768, 768, "SVG tile diagnostic", false, false});
  RendererConfig config =
      lowSpec ? RendererConfig::lowSpec() : RendererConfig{};
  config.vsync = false;
  config.allowTearing = true;
  config.enableAtlasStoragePaths = lowSpec;
  config.validation = true;
  config.enableReadback = true;
  config.clearColor = Color::fromRgb8(0);
  config.enableCompactPersistentCurves = compact && lowSpec;
  config.enablePackedAtlasBands = packedBands && lowSpec;
  if (indexed) {
    config.enableAtlasStoragePaths = true;
    config.enableAtlasBandRefinement = true;
    config.atlasBandRefinementCells = 512;
  }
  VulkanRenderer baseline(baseWindow, atlas, config);
  if (researchProfile) {
    config = RendererConfig::vectorPerformance(lowSpec);
    config.vsync = false;
    config.allowTearing = true;
    config.validation = true;
    config.enableReadback = true;
    config.clearColor = Color::fromRgb8(0);
  }
  config.enableCompactPersistentCurves = compact;
  config.enablePackedAtlasBands = packedBands;
  config.enableIndexedVectorQuads = indexed;
  config.enablePathTileCulling = !storage;
  config.enableAtlasStoragePaths = storage;
  config.enableAtlasBandRefinement = bands;
  config.atlasBandRefinementCells = extraFineBands ? 2048 : expandedBands ? 1024 : finestBands ? 512 : fineBands ? 256 : 128;
  VulkanRenderer tiled(tileWindow, atlas, config);
  if (lowSpec && tiled.stats().atlasStorageShapeCount !=
                     baseline.stats().atlasStorageShapeCount)
    throw std::runtime_error(
        "Refinement changed the admitted original storage paths");
  if (tiled.stats().atlasStorageCurveBytes >
          config.persistentGeometryBufferBytes ||
      tiled.stats().atlasStorageBandBytes >
          config.persistentGeometryBufferBytes)
    throw std::runtime_error(
        "Atlas storage exceeds the persistent payload budget");
  if (storage && tiled.stats().atlasStorageShapeCount == 0)
    throw std::runtime_error(
        "Storage diagnostic did not convert any atlas shapes");
  if (packedBands && tiled.stats().atlasPackedBandShapeCount == 0)
    throw std::runtime_error("Packed diagnostic did not exercise packed indices");
  if (bands && tiled.stats().atlasRefinedShapeCount == 0)
    throw std::runtime_error("Band diagnostic did not refine any atlas shapes");
  if (extraFineBands && !lowSpec && tiled.stats().atlasRefinedShapesByCells[6] == 0)
    throw std::runtime_error("2048-cell diagnostic did not exercise 2048 cells");
  if (expandedBands && !extraFineBands && !lowSpec &&
      tiled.stats().atlasRefinedShapesByCells[5] == 0)
    throw std::runtime_error("1024-cell diagnostic did not exercise 1024 cells");
  if (!baseline.stats().validationEnabled || !tiled.stats().validationEnabled)
    throw std::runtime_error("Tile diagnostic requires Vulkan validation");
  const auto size = svg.size();
  View2D view{size.x * .5,
              size.y * .5,
              std::min(720.0 / size.x, 720.0 / size.y),
              {384, 384}};
  std::size_t badPaths = 0, totalPixels = 0;
  std::size_t nonidenticalPixels = 0;
  int maximumDelta = 0;
  const bool matrix =
      storage || (argc > 1 && std::string_view(argv[1]) == "--matrix");
  const auto begin = argc > 1 && !matrix ? std::stoul(argv[1]) : 0;
  const auto end = argc > 1 && !matrix ? begin + 1 : scene.commands().size();
  if (end > scene.commands().size())
    throw std::invalid_argument("Path index out of range");
  std::vector<View2D> views{view};
  if (matrix) {
    views.push_back({100.125, 100.0625, 1.625, {383.5f, 384.25f}});
    views.push_back({73.125, 117.75, 7.375, {384.125f, 383.75f}});
    views.push_back({160.0, 105.0, 64.0, {383.25f, 384.5f}});
    if (bands) {
      views.push_back({100.125, 100.0625, .125, {383.25f, 384.5f}});
      views.push_back({100.125, 100.0625, .03125, {383.25f, 384.5f}});
      // Focus a real curved boundary on a promoted complex Tiger path. This
      // exercises CPU localization of the refined storage layout at deep zoom.
      const auto &c = std::get<DrawCommand>(scene.commands().at(10));
      const auto s = atlas.native().getShape(slughorn::Key(c.shape)).value();
      const auto &q = s.curves.at(1);
      constexpr double t = .37, u = 1 - t;
      const double x = u * u * q.x1 + 2 * u * t * q.x2 + t * t * q.x3;
      const double y = u * u * q.y1 + 2 * u * t * q.y2 + t * t * q.y3;
      views.push_back(
          {c.destination.x + (x - s.bearingX) / s.width * c.destination.width,
           c.destination.y + (s.bearingY - y) / s.height * c.destination.height,
           std::exp2(20.0),
           {383.25f, 384.5f}});
    }
  }
  for (std::size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex) {
    const auto viewStarted = std::chrono::steady_clock::now();
    view = views[viewIndex];
    std::cout << "SVG quality view=" << viewIndex << " zoom=" << view.zoom
              << " paths=" << (end - begin) << " started\n" << std::flush;
    for (auto i = begin; i < end; ++i) {
      const auto &c = std::get<DrawCommand>(scene.commands()[i]);
      DrawList path;
      path.shape(c.shape, c.destination,
                 Paint::solid(Color::fromRgb8(0xffffff)));
      const auto a = baseline.createRetainedDrawList(path);
      const auto b = tiled.createRetainedDrawList(path);
      DrawList fa, fb;
      fa.setClip({0, 0, 768, 768});
      fb.setClip({0, 0, 768, 768});
      fa.retainedDrawListView(a, view);
      fb.retainedDrawListView(b, view);
      const auto pa = baseline.drawAndReadback(fa);
      const auto pb = tiled.drawAndReadback(fb);
      const auto shape =
          atlas.native().getShape(slughorn::Key(c.shape)).value();
      if (argc > 2 && !matrix) {
        std::ofstream dump(argv[2]);
        if (!dump)
          throw std::runtime_error("Cannot write path diagnostic JSON");
        dump << std::setprecision(17) << "{\"destination\":[" << c.destination.x
             << ',' << c.destination.y << ',' << c.destination.width << ','
             << c.destination.height << "],\"em_rect\":[" << shape.bearingX
             << ',' << shape.bearingY << ','
             << float(shape.bearingX + shape.width) << ','
             << float(shape.bearingY - shape.height) << "],\"view\":["
             << view.centerX << ',' << view.centerY << ',' << view.zoom << ','
             << view.origin.x << ',' << view.origin.y << "],\"curves\":[";
        bool comma = false;
        for (const auto &q : shape.curves) {
          if (comma)
            dump << ',';
          comma = true;
          dump << '[' << q.x1 << ',' << q.y1 << ',' << q.x2 << ',' << q.y2
               << ',' << q.x3 << ',' << q.y3 << ']';
        }
        dump << "]}";
      }
      const double screenX =
          (c.destination.x - view.centerX) * view.zoom + view.origin.x;
      const double screenY =
          (c.destination.y - view.centerY) * view.zoom + view.origin.y;
      const double spanX =
          float(shape.bearingX + shape.width) - double(shape.bearingX);
      const double spanY =
          float(shape.bearingY - shape.height) - double(shape.bearingY);
      const auto toScreen = [&](double x, double y) {
        return Point{screenX + (x - shape.bearingX) / spanX *
                                   c.destination.width * view.zoom,
                     screenY + (y - shape.bearingY) / spanY *
                                   c.destination.height * view.zoom};
      };
      std::vector<Edge> edges;
      bool edgesBuilt = false;
      const auto reference = [&](double x, double y) {
        return inside(
            shape,
            shape.bearingX +
                (x - screenX) / (c.destination.width * view.zoom) * spanX,
            shape.bearingY +
                (y - screenY) / (c.destination.height * view.zoom) * spanY);
      };
      std::size_t changed = 0, binary = 0, baseWrong = 0, tileWrong = 0,
                  stablePoints = 0;
      for (std::size_t p = 0; p < pa.rgba.size(); p += 4) {
        const int delta = std::abs(int(pa.rgba[p]) - int(pb.rgba[p]));
        maximumDelta = std::max(maximumDelta, delta);
        nonidenticalPixels += delta != 0;
        const bool different = delta > 3;
        changed += different;
        binary += (pa.rgba[p] > 128) != (pb.rgba[p] > 128);
        if (different) {
          // The reference is consulted only at differing pixels. Constructing
          // its full flattened outline eagerly at 2^20 zoom can generate
          // millions of unused edges even for byte-identical GPU images.
          // Keep all path/pixel comparisons and the same independent oracle;
          // build its edge-distance index once, when it is actually needed.
          if (!edgesBuilt) {
            for (const auto &q : shape.curves)
              flatten(edges, toScreen(q.x1, q.y1), toScreen(q.x2, q.y2),
                      toScreen(q.x3, q.y3));
            edgesBuilt = true;
          }
          const double x = (p / 4) % pa.width + .5, y = (p / 4) / pa.width + .5;
          const bool expected = reference(x, y);
          bool stable = true;
          for (const auto &edge : edges)
            if (distanceSquared({x, y}, edge) < 2.25) {
              stable = false;
              break;
            }
          if (stable) {
            ++stablePoints;
            if ((pa.rgba[p] > 128) != expected && baseWrong == 0)
              std::cerr << "baseline reference mismatch path=" << i
                        << " at=" << x << ',' << y << " expected=" << expected
                        << " pixel=" << int(pa.rgba[p]) << '\n';
            if ((pb.rgba[p] > 128) != expected && tileWrong == 0)
              std::cerr << "tile reference mismatch path=" << i << " at=" << x
                        << ',' << y << " expected=" << expected
                        << " pixel=" << int(pb.rgba[p]) << '\n';
            baseWrong += (pa.rgba[p] > 128) != expected;
            tileWrong += (pb.rgba[p] > 128) != expected;
          }
        }
      }
      if (changed) {
        if (badPaths++ == 0) {
          save(pa, "svg-tile-baseline.ppm");
          save(pb, "svg-tile-experiment.ppm");
        }
        totalPixels += changed;
        std::cerr << "view=" << viewIndex << " path=" << i
                  << " changed=" << changed << " binary=" << binary
                  << " stable=" << stablePoints
                  << " baseline_wrong=" << baseWrong
                  << " tile_wrong=" << tileWrong << '\n';
      }
      baseline.destroyRetainedDrawList(a);
      tiled.destroyRetainedDrawList(b);
    }
    const auto ca = baseline.createRetainedDrawList(scene),
               cb = tiled.createRetainedDrawList(scene);
    DrawList da, db;
    da.setClip({9.125f, 17.25f, 713.5f, 729.25f});
    db.setClip({9.125f, 17.25f, 713.5f, 729.25f});
    da.retainedDrawListView(ca, view);
    db.retainedDrawListView(cb, view);
    const auto ia = baseline.drawAndReadback(da),
               ib = tiled.drawAndReadback(db);
    std::size_t sceneChanged = 0;
    for (std::size_t p = 0; p < ia.rgba.size(); p += 4) {
      int delta = 0;
      for (std::size_t channel = 0; channel < 4; ++channel)
        delta = std::max(delta, std::abs(int(ia.rgba[p + channel]) -
                                         int(ib.rgba[p + channel])));
      sceneChanged += delta > 3;
      maximumDelta = std::max(maximumDelta, delta);
      nonidenticalPixels += delta != 0;
    }
    if (sceneChanged) {
      ++badPaths;
      totalPixels += sceneChanged;
      std::cerr << "view=" << viewIndex << " composite changed=" << sceneChanged
                << '\n';
      save(ia, "svg-composite-baseline.ppm");
      save(ib, "svg-composite-experiment.ppm");
    }
    baseline.destroyRetainedDrawList(ca);
    tiled.destroyRetainedDrawList(cb);
    std::cout << "SVG quality view=" << viewIndex << " completed_ms="
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - viewStarted).count()
              << '\n' << std::flush;
  }
  if (baseline.stats().validationErrors || tiled.stats().validationErrors)
    throw std::runtime_error("Tile diagnostic validation errors");
  std::cout << "Tile diagnostic: paths=" << (end - begin) * views.size()
            << " composites=" << views.size() << " failed=" << badPaths
            << " pixels_above_delta3=" << totalPixels
            << " nonidentical_pixels=" << nonidenticalPixels
            << " max_delta=" << maximumDelta << '\n';
  std::cout << "Storage shapes=" << tiled.stats().atlasStorageShapeCount
            << " cells=" << config.atlasBandRefinementCells
            << " refined=" << tiled.stats().atlasRefinedShapeCount
            << " packed=" << tiled.stats().atlasPackedBandShapeCount
            << " curve_bytes=" << tiled.stats().atlasStorageCurveBytes
            << " band_bytes=" << tiled.stats().atlasStorageBandBytes << '\n';
  std::cout << "Refined at 512=" << tiled.stats().atlasRefinedShapesByCells[4]
            << " 1024=" << tiled.stats().atlasRefinedShapesByCells[5]
            << " 2048=" << tiled.stats().atlasRefinedShapesByCells[6] << '\n';
  return badPaths ? 1 : 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
