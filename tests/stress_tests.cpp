#include "slugia/slugui.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace slugia;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() try {
  namespace sui = slugui;
  constexpr std::uint32_t columns = 100;
  constexpr std::uint32_t rows = 100;
  constexpr float stride = 20.0f;

  sui::PropertyStore properties;
  const auto hotWidth = properties.define<sui::Length>(
    "stress-hot-width", sui::Length::physical(12.0f));
  auto root = sui::absolute(1, "stress-root");
  root.children.reserve(columns * rows);
  const Paint paint = Paint::solid(Color::fromRgb8(0x304050));
  for (std::uint32_t y = 0; y < rows; ++y) {
    for (std::uint32_t x = 0; x < columns; ++x) {
      auto cell = sui::roundedRectangle(2 + y * columns + x, paint);
      cell.layout.x = sui::Length::physical(x * stride);
      cell.layout.y = sui::Length::physical(y * stride);
      cell.layout.width = (x == 50 && y == 50)
        ? sui::ValueSource<sui::Length>{hotWidth}
        : sui::ValueSource<sui::Length>{sui::Length::physical(12.0f)};
      cell.layout.height = sui::Length::physical(12.0f);
      cell.interaction = {true, true, 0};
      root.add(std::move(cell));
    }
  }

  sui::Component component(std::move(root), std::move(properties));
  sui::Runtime runtime;
  DrawList first;
  sui::FrameInput input;
  input.cursor = {1005.0f, 1005.0f};
  const Rect viewport{0, 0, columns * stride, rows * stride};
  const auto initial = runtime.render(component, input, first, viewport);
  require(initial.layoutPasses == 1, "initial stress layout must run exactly once");
  require(initial.elements == columns * rows + 1, "all stress elements must resolve");
  require(initial.hitCandidates < 256, "spatial hit testing regressed toward O(n)");

  DrawList stable;
  input.cursor = {1205.0f, 805.0f};
  const auto steady = runtime.render(component, input, stable, viewport);
  require(steady.layoutPasses == 0, "unchanged large UI must not relayout");
  require(steady.measureEvaluations == 0,
          "unchanged large UI must not remeasure any element");
  require(steady.hitCandidates < 256, "steady hit testing must stay spatially bounded");

  component.properties().set(hotWidth, sui::Length::physical(18.0f));
  DrawList changed;
  const auto incremental = runtime.render(component, input, changed, viewport);
  require(incremental.layoutPasses == 1, "geometry change must trigger one layout pass");
  require(incremental.measureEvaluations <= 3,
          "single geometry property must not remeasure the full 10k-element tree");

  constexpr std::uint32_t instanceCount = 4096;
  sui::PropertyStore defaults;
  const auto instanceWidth = defaults.define<sui::Length>(
    "instance-width", sui::Length::physical(64.0f));
  auto instanceRoot = sui::absolute(20001, "shared-instance-root");
  auto instanceChild = sui::roundedRectangle(20002, paint);
  instanceChild.layout.width = instanceWidth;
  instanceChild.layout.height = sui::Length::physical(20.0f);
  instanceChild.interaction = {true, true, 0};
  instanceRoot.add(std::move(instanceChild));
  auto definition = std::make_shared<const sui::ComponentDefinition>(
    std::move(instanceRoot), std::move(defaults));

  std::vector<sui::ComponentInstance> instances;
  instances.reserve(instanceCount);
  for (std::uint32_t index = 0; index < instanceCount; ++index)
    instances.emplace_back(definition, 100000 + index);
  const auto* sharedRoot = &instances.front().root();
  for (const auto& instance : instances)
    require(&instance.root() == sharedRoot,
            "component instances must share one immutable definition tree");

  instances[123].properties().set(instanceWidth, sui::Length::physical(96.0f));
  require(instances[123].properties().get(instanceWidth).value == 96.0f,
          "instance-local stress override must update the selected instance");
  require(instances[124].properties().get(instanceWidth).value == 64.0f,
          "instance-local stress override must not leak to adjacent instances");

  sui::Scene scene;
  constexpr std::uint32_t sceneColumns = 64;
  for (std::uint32_t index = 0; index < instanceCount; ++index) {
    const float x = static_cast<float>(index % sceneColumns) * 72.0f;
    const float y = static_cast<float>(index / sceneColumns) * 28.0f;
    scene.add(instances[index], {x, y, 64.0f, 20.0f});
  }
  sui::SceneRuntime sceneRuntime;
  sui::FrameInput sceneInput;
  sceneInput.cursor = {73.0f, 29.0f};
  DrawList sceneFirst;
  const auto sceneInitial = sceneRuntime.render(scene, sceneInput, sceneFirst);
  require(sceneInitial.instances == instanceCount,
          "scene stress must retain every shared component instance");
  require(sceneInitial.instanceCandidates < 64,
          "scene hit testing must remain spatially bounded across 4k instances");

  DrawList sceneStable;
  sceneInput.cursor = {1441.0f, 281.0f};
  const auto sceneSteady = sceneRuntime.render(scene, sceneInput, sceneStable);
  require(sceneSteady.layoutPasses == 0,
          "unchanged multi-instance scene must not relayout component definitions");
  require(sceneSteady.measureEvaluations == 0,
          "unchanged multi-instance scene must reuse all component measurements");
  require(sceneSteady.instanceCandidates < 64,
          "steady scene hit testing must not regress toward O(instance count)");

  std::cout << "SlugUI stress passed: elements=" << steady.elements
            << " candidates=" << steady.hitCandidates
            << " incrementalMeasures=" << incremental.measureEvaluations
            << " sharedInstances=" << instances.size()
            << " sceneCandidates=" << sceneSteady.instanceCandidates << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Stress failure: " << error.what() << '\n';
  return 1;
}
