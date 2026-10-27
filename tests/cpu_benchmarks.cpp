#include "slugia/slugui.hpp"
#include "slugia/vector_path.hpp"
#if SLUGIA_ENABLE_3D
#include "slugia/vector3d.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace slugia;

namespace {
using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

slugui::Component makeGrid(std::uint32_t columns, std::uint32_t rows) {
  namespace sui = slugui;
  auto root = sui::absolute(1, "benchmark-grid");
  root.children.reserve(static_cast<std::size_t>(columns) * rows);
  const Paint paint = Paint::solid(Color::fromRgb8(0x304050));
  for (std::uint32_t y = 0; y < rows; ++y) {
    for (std::uint32_t x = 0; x < columns; ++x) {
      auto cell = sui::roundedRectangle(2 + y * columns + x, paint);
      cell.layout.x = sui::Length::physical(static_cast<float>(x) * 20.0f);
      cell.layout.y = sui::Length::physical(static_cast<float>(y) * 20.0f);
      cell.layout.width = sui::Length::physical(12.0f);
      cell.layout.height = sui::Length::physical(12.0f);
      cell.interaction = {true, true, 0};
      root.add(std::move(cell));
    }
  }
  return sui::Component(std::move(root));
}

void printPercentiles(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const auto at = [&](double percentile) {
    const std::size_t index = static_cast<std::size_t>(
      percentile * static_cast<double>(samples.size() - 1));
    return samples[index];
  };
  double total = 0.0;
  for (double sample : samples) total += sample;
  std::cout << std::fixed << std::setprecision(3)
            << " avg=" << total / static_cast<double>(samples.size()) << "ms"
            << " p50=" << at(0.50) << "ms"
            << " p95=" << at(0.95) << "ms"
            << " p99=" << at(0.99) << "ms\n";
}

template<class Emit>
void benchmarkGeometry(const char* name, Emit&& emit) {
  DrawList frame;
  std::vector<double> samples;
  samples.reserve(500);
  std::size_t commands=0;
  for (int iteration=0; iteration<532; ++iteration) {
    const auto begin=Clock::now();
    frame.clear();
    emit(frame,iteration);
    const auto end=Clock::now();
    commands+=frame.commands().size();
    if (iteration>=32) samples.push_back(milliseconds(end-begin));
  }
  std::cout << name << " (32 warmup, 500 samples, commands=" << commands << "):";
  printPercentiles(std::move(samples));
}
} // namespace

int main() {
  namespace sui = slugui;
  constexpr std::uint32_t columns = 80;
  constexpr std::uint32_t rows = 80;
  auto component = makeGrid(columns, rows);
  sui::Runtime runtime;
  sui::FrameInput input;
  input.cursor = {805.0f, 805.0f};
  const Rect viewport{0, 0, columns * 20.0f, rows * 20.0f};

  DrawList first;
  const auto initialStart = Clock::now();
  const auto initialStats = runtime.render(component, input, first, viewport);
  const auto initialEnd = Clock::now();
  std::cout << "SlugUI " << initialStats.elements << " elements\n";
  std::cout << " initial=" << std::fixed << std::setprecision(3)
            << milliseconds(initialEnd - initialStart) << "ms"
            << " hitCandidates=" << initialStats.hitCandidates << '\n';

  std::vector<double> stableSamples;
  stableSamples.reserve(200);
  DrawList frame;
  for (std::uint32_t iteration = 0; iteration < 200; ++iteration) {
    frame.clear();
    input.cursor = {5.0f + static_cast<float>((iteration * 17) % 1500),
                    5.0f + static_cast<float>((iteration * 29) % 1500)};
    const auto begin = Clock::now();
    const auto stats = runtime.render(component, input, frame, viewport);
    const auto end = Clock::now();
    if (stats.layoutPasses != 0) {
      std::cerr << "stable frame unexpectedly relaid out\n";
      return 1;
    }
    stableSamples.push_back(milliseconds(end - begin));
  }
  std::cout << " stable:";
  printPercentiles(std::move(stableSamples));
  const auto paint=Paint::solid(Color::fromRgb8(0x65caff));
  benchmarkGeometry("2D changing cubic contour",[&](DrawList& draw,int i) {
    const float offset=static_cast<float>(i%120);
    auto curves=makeCubicStrokeQuadratics({0,0},{30,offset},{70,-offset},{100,0},3,LineCap::Round);
    const auto bounds=quadraticBounds(curves);
    draw.dynamicVectorPath(std::move(curves),bounds,paint);
  });
#if SLUGIA_ENABLE_3D
  const auto sphere=makeUvSphereMesh3D(42,48,24);
  Mesh3DView view;
  Mesh3DStyle style;
  style.edge.width=1;
  benchmarkGeometry("3D convex sphere lowering",[&](DrawList& draw,int i) {
    view.transform.rotation={0.2f,static_cast<float>(i)*0.001f,0.1f};
    drawMesh3D(draw,sphere,view,style);
  });
  auto general=sphere;
  general.convex=false;
  benchmarkGeometry("3D general sphere lowering",[&](DrawList& draw,int i) {
    view.transform.rotation={0.2f,static_cast<float>(i)*0.001f,0.1f};
    drawMesh3D(draw,general,view,style);
  });
#endif
  return 0;
}
