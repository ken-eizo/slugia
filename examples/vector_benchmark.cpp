#include "slugia/slugia.hpp"
#include <slughorn/slughorn.hpp>
#ifdef SLUGIA_VECTOR_EMBEDDED_FONT
#include "geist_font.generated.hpp"
#include "tiger_svg.generated.hpp"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace slugia;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  return values.at(std::min(values.size() - 1,
                            std::size_t(std::ceil(p * values.size())) - 1));
}
template<class T> void writeSamples(std::ostream &out, const std::vector<T> &samples) {
  out << '[';
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i)
      out << ',';
    out << samples[i];
  }
  out << ']';
}
void capture(const FramePixels &image, const std::string &file) {
  std::ofstream out(file, std::ios::binary);
  if (!out)
    throw std::runtime_error("Cannot write capture");
  out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
  for (std::size_t i = 0; i < image.rgba.size(); i += 4)
    out.write(reinterpret_cast<const char *>(image.rgba.data() + i), 3);
}
struct Options {
  bool bench = false, smoke = false, overdraw = false, lowSpec = false,
       profile = false, noTiles = true, atlasStorage = false,
       atlasBands = false, indexedQuads = false, compactCurves = false, packedBands = false,
       profilePhases = false, researchPreset = false, embeddedSvg = false;
  std::string svg = SLUGIA_VECTOR_TIGER, output, capture;
  int doublings = 12, samples = 40;
  int bandCells = 128;
  double frameBudget = 16.667, memoryMiB = 256, timeSeconds = 120;
};
Options options(int argc, char **argv) {
  Options o;
  bool explicitBandCells = false;
#ifdef SLUGIA_VECTOR_EMBEDDED_FONT
  o.embeddedSvg = true;
#endif
#ifdef SLUGIA_VECTOR_FAST_PREVIEW
  const auto selected = RendererConfig::vectorPerformance();
  o.researchPreset = true;
  o.atlasStorage = selected.enableAtlasStoragePaths;
  o.atlasBands = selected.enableAtlasBandRefinement;
  o.compactCurves = selected.enableCompactPersistentCurves;
  o.packedBands = selected.enablePackedAtlasBands;
  o.bandCells = selected.atlasBandRefinementCells;
#endif
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&]() {
      if (i + 1 >= argc)
        throw std::invalid_argument("Missing option value");
      return std::string(argv[++i]);
    };
    if (a == "--benchmark")
      o.bench = true;
    else if (a == "--smoke")
      o.smoke = true;
    else if (a == "--overdraw")
      o.overdraw = true;
    else if (a == "--low-spec")
      o.lowSpec = true;
    else if (a == "--no-path-tiles")
      o.noTiles = true;
    else if (a == "--path-tiles")
      o.noTiles = false;
    else if (a == "--atlas-storage")
      o.atlasStorage = true;
    else if (a == "--indexed-quads")
      o.indexedQuads = true;
    else if (a == "--compact-curves")
      o.compactCurves = true;
    else if (a == "--profile-phases")
      o.profilePhases = true;
    else if (a == "--packed-bands") {
      o.packedBands = true;
      o.atlasStorage = true;
    }
    else if (a == "--atlas-bands")
      o.atlasStorage = o.atlasBands = true;
    else if (a == "--atlas-band-cells") {
      o.bandCells = std::stoi(value());
      explicitBandCells = true;
      o.atlasStorage = o.atlasBands = true;
    } else if (a == "--baseline") {
      o.researchPreset = false;
      o.atlasStorage = o.atlasBands = o.compactCurves = o.packedBands = o.indexedQuads = false;
      o.noTiles = true;
      if (!explicitBandCells) o.bandCells = 128;
    }
    else if (a == "--profile-ranges") {
      o.profile = true;
      o.bench = true;
    } else if (a == "--svg") {
      o.svg = value();
      o.embeddedSvg = false;
    }
    else if (a == "--output")
      o.output = value();
    else if (a == "--capture")
      o.capture = value();
    else if (a == "--doublings")
      o.doublings = std::stoi(value());
    else if (a == "--samples")
      o.samples = std::stoi(value());
    else if (a == "--frame-budget-ms")
      o.frameBudget = std::stod(value());
    else if (a == "--memory-mib")
      o.memoryMiB = std::stod(value());
    else if (a == "--seconds")
      o.timeSeconds = std::stod(value());
    else
      throw std::invalid_argument("Unknown vector benchmark option: " + a);
  }
  if (o.lowSpec && !explicitBandCells && !o.researchPreset)
    o.bandCells = 128;
  if (o.profilePhases && o.samples > 200)
    throw std::invalid_argument("CPU phase profiles support at most 200 raw samples per stage");
  if (o.bandCells < 32 || o.bandCells > 2048 ||
      (o.bandCells & (o.bandCells - 1)) != 0 || o.doublings < 0 ||
      o.doublings > 30 || o.samples < 8 || o.samples > 2000 ||
      !std::isfinite(o.frameBudget) || o.frameBudget <= 0 ||
      o.frameBudget > 1000 || !std::isfinite(o.memoryMiB) || o.memoryMiB < 16 ||
      o.memoryMiB > 1024 || !std::isfinite(o.timeSeconds) ||
      o.timeSeconds <= 0 || o.timeSeconds > 3600)
    throw std::invalid_argument(
        "Benchmark options exceed the declared work budget");
  return o;
}
TextStyle label(float size = 16) {
  TextStyle s;
  s.fontName = "Geist";
  s.size = size;
  return s;
}
} // namespace
int main(int argc, char **argv) try {
  const auto opt = options(argc, argv);
  const auto importStart = Clock::now();
#ifdef SLUGIA_VECTOR_EMBEDDED_FONT
  auto svg = opt.embeddedSvg
      ? SvgDocument(std::string_view(reinterpret_cast<const char*>(example::assets::tigerSvg),
                                     sizeof(example::assets::tigerSvg)))
      : SvgDocument::load(opt.svg);
#else
  auto svg = SvgDocument::load(opt.svg);
#endif
  VectorAtlas atlas;
  svg.registerShapes(atlas);
#ifdef SLUGIA_VECTOR_EMBEDDED_FONT
  atlas.loadFontMemory(example::assets::geistFont,
                       {"Geist", 400, false, "Regular"});
#else
  if (!opt.bench)
    atlas.loadFont(SLUGIA_VECTOR_FONT, {"Geist", 400, false, "Regular"});
#endif
  atlas.build();
  const auto scene = svg.drawList(atlas);
  const auto importMs = elapsed(importStart);
  if (scene.commands().empty())
    throw std::runtime_error("SVG contains no drawable geometry");
  const auto dimensions = svg.size();
  Window window({opt.bench ? 1024 : 1280, opt.bench ? 1024 : 820,
                 "slugia Vector - SVG and deep zoom", true,
                 !(opt.bench || opt.smoke)});
  auto config = opt.researchPreset ? RendererConfig::vectorPerformance(opt.lowSpec)
                                 : opt.lowSpec ? RendererConfig::lowSpec() : RendererConfig{};
  config.enablePathTileCulling = !opt.noTiles;
  config.enableAtlasStoragePaths = opt.atlasStorage;
  config.enableAtlasBandRefinement = opt.atlasBands;
  config.enableIndexedVectorQuads = opt.indexedQuads;
  config.enableCompactPersistentCurves = opt.compactCurves;
  config.enablePackedAtlasBands = opt.packedBands;
  config.enableCpuPhaseTiming = opt.profilePhases;
  config.atlasBandRefinementCells = opt.bandCells;
  config.validation = opt.smoke;
  config.enableReadback = !opt.capture.empty();
  config.vsync = false;
  config.allowTearing = true;
  config.gpuTimingInterval = 1;
  config.clearColor = Color::fromRgb8(0x101724);
  const auto rendererStart = Clock::now();
  VulkanRenderer renderer(window, atlas, config);
  const auto rendererInitMs = elapsed(rendererStart);
  std::cout << "SVG shapes=" << svg.shapeCount()
            << ", draws=" << scene.commands().size() << ", import=" << importMs
            << " ms, GPU=" << renderer.deviceName() << '\n';
  if (opt.profile) {
    const auto count = scene.commands().size();
    for (std::size_t begin = 0; begin < count; begin += (count + 15) / 16) {
      const auto end = std::min(count, begin + (count + 15) / 16);
      DrawList part;
      std::size_t curves = 0, maxCurves = 0, maxIndex = begin;
      for (auto i = begin; i < end; ++i) {
        const auto &c = std::get<DrawCommand>(scene.commands()[i]);
        part.shape(c.shape, c.destination, c.paint);
        const auto n =
            atlas.native().getShape(slughorn::Key(c.shape))->curves.size();
        curves += n;
        if (n > maxCurves) {
          maxCurves = n;
          maxIndex = i;
        }
      }
      const auto id = renderer.createRetainedDrawList(part);
      View2D view{dimensions.x * .5,
                  dimensions.y * .5,
                  std::min(1000.0 / dimensions.x, 1000.0 / dimensions.y),
                  {512, 512}};
      std::vector<double> gpu;
      for (int f = 0; f < 12; ++f) {
        DrawList frame;
        frame.setClip({0, 0, 1024, 1024});
        frame.retainedDrawListView(id, view);
        renderer.draw(frame);
        if (f >= 4)
          gpu.push_back(renderer.stats().gpuMilliseconds);
      }
      std::cout << "range " << begin << ".." << end << " curves=" << curves
                << " max=" << maxCurves << " index=" << maxIndex
                << " gpu_p50=" << percentile(gpu, .5) << " ms\n";
      renderer.destroyRetainedDrawList(id);
    }
    return 0;
  }
  if (opt.bench) {
    std::ofstream file;
    if (!opt.output.empty()) {
      file.open(opt.output);
      if (!file)
        throw std::runtime_error("Cannot write benchmark results");
    }
    std::ostream &report = file.is_open() ? file : std::cout;
    report << std::setprecision(9)
           << "{\n  \"schema\":1, \"device\":"
           << std::quoted(renderer.deviceName())
           << ", \"present_mode\":" << std::quoted(renderer.presentModeName())
           << ", \"api_version_raw\":" << renderer.stats().apiVersion
           << ", \"driver_version_raw\":" << renderer.stats().driverVersion
           << ", \"vendor_id\":" << renderer.stats().vendorId
           << ", \"device_id\":" << renderer.stats().deviceId
           << ", \"frames_in_flight\":" << renderer.stats().maxFramesInFlight
           << ", \"gpu_timestamp_period_ns\":" << renderer.stats().gpuTimestampPeriodNanoseconds
           << ", \"gpu_timestamp_valid_bits\":" << renderer.stats().gpuTimestampValidBits
           << ", \"cpu_phase_timing_enabled\":" << (opt.profilePhases ? "true" : "false")
           << ", \"measurement_profile\":" << std::quoted(opt.profilePhases ? "cpu_api_phases_v2" : "retained_static_v1")
           << ", \"gpu_tail_drained\":" << (opt.profilePhases ? "true" : "false")
           << ", \"frontier_eligible\":" << (opt.profilePhases ? "false" : "true")
           << ", \"asset_source\":" << std::quoted(opt.embeddedSvg ? "embedded" : "file")
#ifdef SLUGIA_VECTOR_EMBEDDED_FONT
           << ", \"embedded_asset_sha256\":" << std::quoted(opt.embeddedSvg ? example::assets::tigerSvgSha256 : "")
#endif
           << ", \"configuration_profile\":" << std::quoted(
                config.enableAtlasStoragePaths && config.enableAtlasBandRefinement &&
                config.enableCompactPersistentCurves && config.enablePackedAtlasBands &&
                config.atlasBandRefinementCells == 1024 ? "vector_performance_2026_10" : "custom_or_compatibility")
           << ", \"asset\":" << std::quoted(opt.svg) << ", \"mode\":\""
           << (opt.overdraw ? "overdraw" : "tiles")
           << "\", \"workload_class\":\"static_retained"
           << "\", \"geometry_updates_per_frame\":0"
           << ", \"timer_scope\":\"prepare_poll_record_submit_present\""
           << ", \"gpu_timestamp_frame_alignment\":\"asynchronous_unpaired\""
           << ", \"readback_in_timed_interval\":false"
           << ", \"preprocessing_in_timed_interval\":false"
           << ", \"percentile_estimator\":\"nearest_rank\""
           << ", \"width\":1024, \"height\":1024, \"samples\":" << opt.samples
           << ", \"frame_budget_ms\":" << opt.frameBudget
           << ", \"import_ms\":" << importMs << ", \"renderer_profile\":\""
           << (opt.lowSpec ? "low_spec" : "standard")
           << "\", \"experimental_path_tiles\":"
           << (opt.noTiles ? "false" : "true")
           << ", \"experimental_atlas_storage\":"
           << (opt.atlasStorage ? "true" : "false")
           << ", \"experimental_atlas_bands\":"
           << (opt.atlasBands ? "true" : "false")
            << ", \"experimental_indexed_quads\":" << (opt.indexedQuads ? "true" : "false")
           << ", \"atlas_band_cells\":" << opt.bandCells
           << ", \"experimental_compact_curves\":" << (opt.compactCurves ? "true" : "false")
           << ", \"persistent_curve_stride_bytes\":" << (opt.compactCurves ? 24 : 32)
           << ", \"experimental_packed_bands\":" << (opt.packedBands ? "true" : "false")
           << ", \"atlas_storage_shapes\":"
           << renderer.stats().atlasStorageShapeCount
           << ", \"atlas_packed_band_shapes\":" << renderer.stats().atlasPackedBandShapeCount
           << ", \"atlas_refined_shapes\":"
           << renderer.stats().atlasRefinedShapeCount
           << ", \"atlas_refined_shapes_by_cells\":["
           << renderer.stats().atlasRefinedShapesByCells[0] << ','
           << renderer.stats().atlasRefinedShapesByCells[1] << ','
           << renderer.stats().atlasRefinedShapesByCells[2] << ','
           << renderer.stats().atlasRefinedShapesByCells[3] << ','
           << renderer.stats().atlasRefinedShapesByCells[4] << ','
           << renderer.stats().atlasRefinedShapesByCells[5] << ','
           << renderer.stats().atlasRefinedShapesByCells[6] << ']'
           << ", \"atlas_storage_curve_bytes\":"
           << renderer.stats().atlasStorageCurveBytes
           << ", \"atlas_storage_band_bytes\":"
           << renderer.stats().atlasStorageBandBytes
           << ", \"renderer_init_ms\":" << rendererInitMs
           << ", \"path_cache_bytes\":" << config.dynamicPathCacheBytes
           << ", \"persistent_buffer_bytes_each\":"
           << config.persistentGeometryBufferBytes
           << ", \"memory_estimate_model\":\"owned_shadow_capped_staging_v2\""
           << ", \"retained_instance_stride_bytes\":"
           << renderer.stats().retainedInstanceStrideBytes
           << ", \"retained_upload_staging_limit_bytes\":"
           << renderer.stats().retainedUploadStagingLimitBytes
           << ",\n  \"stages\":[\n";
    const auto begin = Clock::now();
    bool comma = false;
    std::string stop = "requested_extent";
    for (int exponent = 0; exponent <= opt.doublings; ++exponent) {
      const std::size_t copies = std::size_t(1) << exponent;
      const std::size_t count = scene.commands().size() * copies;
      // Logical command + owned CPU shadow + device payload + capped staging.
      // Lowering transfers ownership into the shadow; recording reserves the
      // exact command count. Import, allocator capacity and driver overhead are
      // additional and remain covered by the runner's unchanged RSS limit.
      const auto memoryStats = renderer.stats();
      const double instanceBytes =
          count * double(memoryStats.retainedInstanceStrideBytes);
      const double estimated =
          (count * double(sizeof(DisplayCommand)) + 2.0 * instanceBytes +
           std::min(instanceBytes,
                    double(memoryStats.retainedUploadStagingLimitBytes))) /
          1048576.0;
      if (estimated > opt.memoryMiB) {
        stop = "memory_budget";
        break;
      }
      if (elapsed(begin) > opt.timeSeconds * 1000) {
        stop = "time_budget";
        break;
      }
      const auto prepare = Clock::now();
      const auto columns =
          opt.overdraw ? 1 : std::size_t(std::ceil(std::sqrt(double(copies))));
      const auto rows = opt.overdraw ? 1 : (copies + columns - 1) / columns;
      DrawList grid;
      grid.reserve(count);
      for (std::size_t copy = 0; copy < copies; ++copy)
        for (const auto &c : scene.commands()) {
          const auto &shape = std::get<DrawCommand>(c);
          Rect rect = shape.destination;
          if (!opt.overdraw) {
            rect.x += float(copy % columns) * dimensions.x;
            rect.y += float(copy / columns) * dimensions.y;
          }
          grid.shape(shape.shape, rect, shape.paint);
        }
      const auto retained = renderer.createRetainedDrawList(grid);
      const double prepareMs = elapsed(prepare);
      View2D view{dimensions.x * double(columns) * .5,
                  dimensions.y * double(rows) * .5,
                  std::min(1000.0 / (dimensions.x * double(columns)),
                           1000.0 / (dimensions.y * double(rows))),
                  {512, 512}};
      std::vector<double> wall, cpu, gpu;
      constexpr std::array<const char*, 13> phaseNames{
          "prepare", "events", "record_list", "draw", "fence_wait", "query_read", "acquire",
          "record_commands", "queue_submit", "queue_present", "post_present", "build_ir", "upload"};
      std::array<std::vector<double>, phaseNames.size()> phases;
      std::vector<std::uint64_t> submittedSerials, gpuSampleSerials;
      std::size_t uploaded = 0;
      bool timedOut = false;
      for (int sample = -5; sample < opt.samples; ++sample) {
        const auto frameStart = Clock::now();
        auto afterPrepare = frameStart, afterEvents = frameStart,
             afterRecord = frameStart, afterDraw = frameStart;
        renderer.prepareFrame();
        if (opt.profilePhases) afterPrepare = Clock::now();
        window.pollEvents();
        if (opt.profilePhases) afterEvents = Clock::now();
        DrawList frame;
        frame.setClip({0, 0, 1024, 1024});
        frame.retainedDrawListView(retained, view);
        if (opt.profilePhases) afterRecord = Clock::now();
        renderer.draw(frame);
        if (opt.profilePhases) afterDraw = Clock::now();
        if (sample >= 0) {
          wall.push_back(opt.profilePhases ?
              std::chrono::duration<double, std::milli>(afterDraw - frameStart).count() : elapsed(frameStart));
          const auto s = renderer.stats();
          cpu.push_back(s.cpuBuildMilliseconds);
          gpu.push_back(s.gpuMilliseconds);
          uploaded += s.uploadedBytes;
          if (opt.profilePhases) {
            const auto ms = [](auto a, auto b) {
              return std::chrono::duration<double, std::milli>(b - a).count();
            };
            const std::array<double, phaseNames.size()> values{
                ms(frameStart, afterPrepare), ms(afterPrepare, afterEvents),
                ms(afterEvents, afterRecord), ms(afterRecord, afterDraw),
                s.cpuPhases.fenceWaitMilliseconds, s.cpuPhases.queryReadMilliseconds,
                s.cpuPhases.acquireMilliseconds, s.cpuPhases.recordMilliseconds,
                s.cpuPhases.queueSubmitMilliseconds, s.cpuPhases.queuePresentMilliseconds,
                s.cpuPhases.postPresentMilliseconds, s.cpuBuildMilliseconds, s.cpuUploadMilliseconds};
            for (std::size_t i = 0; i < values.size(); ++i) phases[i].push_back(values[i]);
            submittedSerials.push_back(s.lastSubmittedSerial);
            gpuSampleSerials.push_back(s.gpuSampleSubmissionSerial);
          }
        }
        if (elapsed(begin) > opt.timeSeconds * 1000) {
          timedOut = true;
          break;
        }
      }
      renderer.waitIdle();
      const auto completedTiming = renderer.stats();
      if (!opt.capture.empty() && exponent == 0) {
        DrawList frame;
        frame.setClip({0, 0, 1024, 1024});
        frame.retainedDrawListView(retained, view);
        capture(renderer.drawAndReadback(frame), opt.capture);
      }
      if (wall.empty()) {
        renderer.destroyRetainedDrawList(retained);
        stop = "time_budget";
        break;
      }
      const double p50 = percentile(wall, .5), p95 = percentile(wall, .95),
                   p99 = percentile(wall, .99);
      const double gpu95 = percentile(gpu, .95), cpu95 = percentile(cpu, .95);
      const bool gpuAvailable = renderer.stats().gpuTimingAvailable;
      std::vector<double> joinedGpu;
      if (opt.profilePhases && gpuAvailable) {
        for (auto serial : submittedSerials) {
          const auto found = std::lower_bound(gpuSampleSerials.begin(), gpuSampleSerials.end(), serial);
          if (found != gpuSampleSerials.end() && *found == serial)
            joinedGpu.push_back(gpu[std::size_t(found - gpuSampleSerials.begin())]);
          else if (completedTiming.gpuSampleSubmissionSerial == serial)
            joinedGpu.push_back(completedTiming.gpuMilliseconds);
        }
      }
      const double joinedGpu95 = joinedGpu.empty() ? 0 : percentile(joinedGpu, .95);
      const bool pass = !timedOut && gpuAvailable && p95 <= opt.frameBudget &&
                        gpu95 <= opt.frameBudget &&
                        (!opt.profilePhases || (joinedGpu.size() == wall.size() && joinedGpu95 <= opt.frameBudget)) &&
                        renderer.stats().validationErrors == 0;
      if (comma)
        report << ",\n";
      comma = true;
      report << "    {\"copies\":" << copies << ",\"quads\":" << count
             << ",\"prepare_ms\":" << prepareMs
             << ",\"estimated_working_mib\":" << estimated
             << ",\"wall_p50_ms\":" << p50 << ",\"wall_p95_ms\":" << p95
             << ",\"wall_p99_ms\":" << p99 << ",\"cpu_build_p95_ms\":" << cpu95
             << ",\"gpu_p95_ms\":"
             << (gpuAvailable ? std::to_string(gpu95) : "null")
             << ",\"gpu_p50_ms\":"
             << (gpuAvailable ? std::to_string(percentile(gpu, .5)) : "null")
             << ",\"gpu_p99_ms\":"
             << (gpuAvailable ? std::to_string(percentile(gpu, .99)) : "null")
             << ",\"uploaded_bytes_per_frame\":" << uploaded / wall.size()
             << ",\"retained_upload_staging_peak_bytes\":"
             << renderer.stats().retainedUploadStagingPeakBytes
             << ",\"pass\":" << (pass ? "true" : "false")
             << ",\"measured_samples\":" << wall.size() << ",\"raw_wall_ms\":";
      writeSamples(report, wall);
      report << ",\"raw_cpu_build_ms\":";
      writeSamples(report, cpu);
      report << ",\"raw_gpu_ms\":";
      if (gpuAvailable)
        writeSamples(report, gpu);
      else
        report << "null";
      if (opt.profilePhases) {
        report << ",\"cpu_phases_ms\":{";
        for (std::size_t i = 0; i < phaseNames.size(); ++i) {
          if (i) report << ',';
          report << std::quoted(phaseNames[i]) << ':';
          writeSamples(report, phases[i]);
        }
        report << "},\"cpu_phase_p95_ms\":{";
        for (std::size_t i = 0; i < phaseNames.size(); ++i) {
          if (i) report << ',';
          report << std::quoted(phaseNames[i]) << ':' << percentile(phases[i], .95);
        }
        report << "},\"raw_submitted_serial\":";
        writeSamples(report, submittedSerials);
        report << ",\"raw_gpu_sample_serial\":";
        if (gpuAvailable) writeSamples(report, gpuSampleSerials);
        else report << "null";
        report << ",\"gpu_completed_tail_sample\":";
        if (gpuAvailable)
          report << "{\"submission_serial\":" << completedTiming.gpuSampleSubmissionSerial
                 << ",\"milliseconds\":" << completedTiming.gpuMilliseconds << '}';
        else report << "null";
        report << ",\"gpu_serial_joined_samples\":" << joinedGpu.size()
               << ",\"gpu_serial_joined_p95_ms\":";
        if (gpuAvailable && joinedGpu.size() == wall.size()) report << joinedGpu95;
        else report << "null";
      }
      report << '}';
      report.flush();
      std::cerr << "2^" << exponent << " copies=" << copies
                << " quads=" << count << " p95=" << p95 << " ms GPU=" << gpu95
                << " ms " << (pass ? "PASS" : "FAIL") << '\n';
      renderer.destroyRetainedDrawList(retained);
      if (!pass) {
        stop = timedOut       ? "time_budget"
               : gpuAvailable ? "frame_budget"
                              : "gpu_timing_unavailable";
        break;
      }
    }
    report << "\n  ],\"stop_reason\":\"" << stop
           << "\",\"validation_enabled\":"
           << (renderer.stats().validationEnabled ? "true" : "false")
           << ",\"validation_errors\":" << renderer.stats().validationErrors << "\n}\n";
    if (opt.smoke && (!renderer.stats().validationEnabled ||
                      renderer.stats().validationErrors))
      throw std::runtime_error("Benchmark capture validation failed");
    return 0; // Measured failure is data, never promoted to a passing result.
  }
  const auto retained = renderer.createRetainedDrawList(scene);
  TextStyle text = label(28);
  const auto retainedText = renderer.createRetainedText(
      "Vector outlines: AaBb 0123456789\nZoom without bitmap substitution",
      {0, 0, 800, 100}, text);
  View2D view{dimensions.x * .5,
              dimensions.y * .5,
              std::min(1080.0 / dimensions.x, 620.0 / dimensions.y),
              {640, 440}};
  const double initialZoom = view.zoom;
  int frames = 0;
  while (!window.shouldClose()) {
    renderer.prepareFrame();
    window.pollEvents();
    const auto size = window.framebufferSize();
    const auto &input = window.input();
    view.origin = {size.x * .5f, size.y * .53f};
    if (input.key(Key::Escape).pressed)
      window.requestClose();
    if (input.key('R').pressed) {
      view.centerX = dimensions.x * .5;
      view.centerY = dimensions.y * .5;
      view.zoom = initialZoom;
    }
    if (input.mouse(MouseButton::Left).down) {
      view.centerX -= input.cursorDelta().x / view.zoom;
      view.centerY -= input.cursorDelta().y / view.zoom;
    }
    if (input.scroll().delta.y != 0) {
      const auto mouse = input.cursorPosition();
      const double x = view.centerX + (mouse.x - view.origin.x) / view.zoom,
                   y = view.centerY + (mouse.y - view.origin.y) / view.zoom;
      view.zoom = std::exp2(std::clamp(
          std::log2(view.zoom) + input.scroll().delta.y * .25, -60.0, 60.0));
      view.centerX = x - (mouse.x - view.origin.x) / view.zoom;
      view.centerY = y - (mouse.y - view.origin.y) / view.zoom;
    }
    DrawList frame;
    frame.setClip({0, 0, size.x, size.y});
    frame.retainedDrawListView(retained, view);
    // A vector text document can use the same precision-preserving camera.
    if (input.key('T').down) {
      View2D tv = view;
      tv.centerX -= dimensions.x * .5;
      tv.centerY -= dimensions.y * .5;
      frame.retainedTextView(retainedText, tv);
    }
    frame.textStatic(opt.atlasBands ? "slugia Vector - Performance"
                                    : "slugia Vector",
                     {24, 18, 1100, 40}, label(28));
    frame.textStatic(
        "Wheel: zoom at pointer   Drag: pan   R: reset   Hold T: vector text",
        {24, 64, 1100, 30}, label());
    std::ostringstream status;
    status << std::setprecision(8) << "Zoom " << view.zoom << "x  |  "
           << svg.shapeCount() << " SVG shapes";
    frame.text(status.str(), {24, size.y - 40, 1100, 30}, label());
    if (opt.smoke && !opt.capture.empty() && frames == 2)
      capture(renderer.drawAndReadback(frame), opt.capture);
    else
      renderer.draw(frame);
    if (opt.smoke && ++frames >= 3)
      window.requestClose();
  }
  renderer.waitIdle();
  if (opt.smoke && (!renderer.stats().validationEnabled ||
                    renderer.stats().validationErrors))
    throw std::runtime_error("Vector smoke validation failed");
  return 0;
} catch (const std::exception &e) {
  std::cerr << "Vector benchmark: " << e.what() << '\n';
  return 1;
}
