#include "figma_import_bridge.hpp"
#include "figma_sample.generated.hpp"

#include <utility>

namespace slugia::example {

std::optional<slugui::Component> buildFigmaImport(VectorAtlas& atlas, bool enabled) {
  if (!enabled) return std::nullopt;
  FigmaSampleGenerated generated(atlas);
  return std::optional<slugui::Component>{std::move(generated.component)};
}

} // namespace slugia::example
