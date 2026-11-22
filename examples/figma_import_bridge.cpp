#include "figma_import_bridge.hpp"
#include "figma_sample.generated.hpp"

#include <utility>
#include <cstdint>

namespace slugia::example {

std::uint64_t compiledFigmaSourceFingerprint() {
  return FigmaSampleGenerated::sluguiSourceFingerprint;
}

std::optional<slugui::Component> buildFigmaImport(VectorAtlas& atlas, bool enabled) {
  if (!enabled) return std::nullopt;
  FigmaSampleGenerated generated(atlas);
  return std::optional<slugui::Component>{std::move(generated.component)};
}

} // namespace slugia::example
