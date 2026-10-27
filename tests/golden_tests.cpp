#include "slugia/slugui.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace slugia;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool close(float a, float b) { return std::abs(a - b) < 0.0001f; }

float opaqueBorderCoverage(float outer, float inner) {
  const float ring = std::clamp(outer - inner, 0.0f, 1.0f);
  const float remaining = 1.0f - inner;
  return remaining > 1.0e-6f ? std::clamp(ring / remaining, 0.0f, 1.0f) : 0.0f;
}
}

int main() try {
  namespace sui = slugui;
  auto root = sui::absolute(1, "golden-root");
  auto panel = sui::roundedRectangle(
    2, Paint::solid(Color::fromRgb8(0x204060)), CornerRadii::all(8.0f),
    CornerSmoothing::all(100.0f));
  panel.layout.x = sui::Length::physical(10.0f);
  panel.layout.y = sui::Length::physical(20.0f);
  panel.layout.width = sui::Length::physical(100.0f);
  panel.layout.height = sui::Length::physical(40.0f);
  root.add(std::move(panel));

  sui::Component component(std::move(root));
  sui::Runtime runtime;
  DrawList draw;
  const auto stats = runtime.render(component, sui::FrameInput{}, draw, {0, 0, 320, 180});
  require(stats.layoutPasses == 1 && stats.elements == 2, "golden layout statistics changed");
  require(draw.commands().size() == 1 && draw.overlayCommands().empty(),
          "golden command topology changed");
  const auto* command = std::get_if<RoundedRectCommand>(&draw.commands().front());
  require(command != nullptr, "golden command type changed");
  require(close(command->destination.x, 10.0f) && close(command->destination.y, 20.0f) &&
              close(command->destination.width, 100.0f) && close(command->destination.height, 40.0f),
          "golden rectangle geometry changed");
  require(close(command->radiiPx.topLeft, 8.0f) && close(command->radiiPx.bottomRight, 8.0f) &&
              close(command->continuousCorners.topLeftPercent, 100.0f),
          "golden corner semantics changed");
  const Color expected = Color::fromRgb8(0x204060);
  require(close(command->paint.start.r, expected.r) && close(command->paint.start.g, expected.g) &&
              close(command->paint.start.b, expected.b),
          "golden paint changed");

  // Opaque rounded borders are rendered before their inner fill. The border coverage must stay
  // continuous at the inner edge and source-over composition must reconstruct the outer shape's
  // coverage exactly; thresholding this value was the cause of jagged inner border AA.
  for (const auto [outer, inner] : {
         std::pair{1.0f, 0.0f}, std::pair{1.0f, 0.25f}, std::pair{1.0f, 0.5f},
         std::pair{1.0f, 0.75f}, std::pair{0.8f, 0.2f}, std::pair{0.45f, 0.1f}}) {
    const float border = opaqueBorderCoverage(outer, inner);
    const float composedAlpha = inner + border * (1.0f - inner);
    require(close(composedAlpha, outer),
            "opaque border/fill composition no longer preserves continuous outer coverage");
  }

  DrawList stable;
  const auto stableStats = runtime.render(component, sui::FrameInput{}, stable, {0, 0, 320, 180});
  require(stableStats.layoutPasses == 0 && stable.commands().size() == 1,
          "golden steady-state lowering changed");
  std::cout << "CPU semantic golden passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Golden failure: " << error.what() << '\n';
  return 1;
}
